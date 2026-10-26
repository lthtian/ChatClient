#include "camera_window.h"
#include "rtc_connection.h"
#include "video_view.h"
#include <QAbstractVideoSurface>
#include <QCamera>
#include <QCameraInfo>
#include <QCameraViewfinderSettings>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVideoSurfaceFormat>
#include <algorithm>
#include <functional>
#include <stdexcept>

// Qt 后端将画面送到 present；image().copy() 与相机缓冲解除所有权关系。
class CameraSurface : public QAbstractVideoSurface {
public:
    explicit CameraSurface(QObject* parent) : QAbstractVideoSurface(parent) {}
    std::function<void(QImage)> frame; // GUI 消费者；相机停机后对象才销毁。
    QList<QVideoFrame::PixelFormat> supportedPixelFormats(QAbstractVideoBuffer::HandleType type) const override {
        if (type != QAbstractVideoBuffer::NoHandle) return {};
        return {QVideoFrame::Format_RGB32, QVideoFrame::Format_ARGB32, QVideoFrame::Format_RGB24,
                QVideoFrame::Format_YUYV, QVideoFrame::Format_UYVY, QVideoFrame::Format_NV12,
                QVideoFrame::Format_YUV420P, QVideoFrame::Format_YV12};
    }
    bool present(const QVideoFrame& value) override {
        QImage image = value.image();
        if (image.isNull()) return false;
        if (surfaceFormat().scanLineDirection() == QVideoSurfaceFormat::BottomToTop) image = image.mirrored();
        if (frame) frame(image.copy());
        return true;
    }
};

CameraWindow::CameraWindow(bool publishing, QWidget* parent)
    : QDialog(parent, Qt::Window), publishing_(publishing), title_(new QLineEdit(this)),
      status_(new QLabel(this)), start_(new QPushButton(QStringLiteral("开始"), this)),
      stop_(new QPushButton(publishing ? QStringLiteral("结束直播") : QStringLiteral("关闭观看"), this)),
      view_(new VideoView(this)), surface_(new CameraSurface(this)), playback_(view_, this) {
    playback_.Configure(true, true);
    connect(&playback_, &Playback::FramePresented, this, &CameraWindow::FramePresented);
    setWindowTitle(publishing ? QStringLiteral("摄像头直播") : QStringLiteral("观看摄像头直播"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(880, 600);
    auto* layout = new QVBoxLayout(this);
    title_->setText(QStringLiteral("我的摄像头直播"));
    title_->setVisible(publishing);
    layout->addWidget(title_);
    if (publishing) {
        cameras_ = new QComboBox(this); microphone_ = new QCheckBox(QStringLiteral("麦克风"), this);
        for (const auto& camera : QCameraInfo::availableCameras()) cameras_->addItem(camera.description(), camera.deviceName());
        auto* choices = new QHBoxLayout;
        choices->addWidget(cameras_, 1); choices->addWidget(microphone_); layout->addLayout(choices);
        connect(cameras_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CameraWindow::SelectCamera);
        connect(microphone_, &QCheckBox::toggled, this, [this](bool enabled) {
            if (connection_) connection_->SetMicrophone(enabled);
        });
        surface_->frame = [this](QImage image) {
            latest_ = std::move(image); view_->SetImage(latest_);
            if (connection_) connection_->SetPicture(latest_);
            UpdateControls();
        };
        SelectCamera();
    }
    layout->addWidget(view_, 1); layout->addWidget(status_);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(start_); buttons->addWidget(stop_);
    if (!publishing) {
        auto* mute = new QCheckBox(QStringLiteral("静音"), this);
        buttons->addWidget(mute);
        connect(mute, &QCheckBox::toggled, this, [this](bool value) { playback_.Audio().SetVolume(70, value); });
    }
    buttons->addStretch(); layout->addLayout(buttons);
    connect(start_, &QPushButton::clicked, this, &CameraWindow::Start);
    connect(stop_, &QPushButton::clicked, this, &CameraWindow::close);
    connect(&timer_, &QTimer::timeout, this, &CameraWindow::Tick);
    timer_.setInterval(10); timer_.setTimerType(Qt::PreciseTimer);
    UpdateControls();
}
CameraWindow::~CameraWindow() {
    if (camera_) camera_->stop();
    if (connection_) { connection_->Stop(); connection_->wait(); delete connection_.data(); }
}
QString CameraWindow::LiveId() const { return live_["live_id"].toString(); }
bool CameraWindow::Busy() const { return authorizing_ || ending_ || connection_ || (publishing_ && !LiveId().isEmpty()); }
void CameraWindow::SetExisting(QJsonObject live) {
    live_ = std::move(live); title_->setText(live_["title"].toString()); UpdateControls();
}
void CameraWindow::SelectCamera() {
    if (camera_) { camera_->stop(); delete camera_; camera_ = nullptr; }
    latest_ = {};
    if (!cameras_ || cameras_->currentIndex() < 0) { status_->setText(QStringLiteral("未发现可用摄像头")); return; }
    const auto devices = QCameraInfo::availableCameras();
    const auto found = std::find_if(devices.begin(), devices.end(), [this](const QCameraInfo& item) {
        return item.deviceName() == cameras_->currentData().toString();
    });
    if (found == devices.end()) { status_->setText(QStringLiteral("所选摄像头已断开")); return; }
    camera_ = new QCamera(*found, this);
    camera_->setViewfinder(surface_);
    QCameraViewfinderSettings settings;
    settings.setResolution(1280, 720); settings.setMaximumFrameRate(30);
    camera_->setViewfinderSettings(settings);
    connect(camera_, QOverload<QCamera::Error>::of(&QCamera::error), this, [this](QCamera::Error) {
        status_->setText(camera_->errorString());
        if (connection_) connection_->Stop();
    });
    status_->setText(QStringLiteral("正在打开摄像头；麦克风默认关闭"));
    camera_->start();
}
void CameraWindow::Start() {
    if (authorizing_ || ending_ || connection_ || closing_) return;
    if (publishing_ && latest_.isNull()) return;
    authorizing_ = true; status_->setText(QStringLiteral("正在申请直播授权")); UpdateControls();
    emit StartRequested(title_->text().trimmed());
}
void CameraWindow::SetAuthorization(QJsonObject live, QJsonObject descriptor, const QString& error) {
    authorizing_ = false;
    if (!error.isEmpty()) { status_->setText(error); UpdateControls(); FinishClose(); return; }
    live_ = std::move(live);
    if (closing_) { Stop(); FinishClose(); return; }
    try {
        playback_.Reset();
        const int rate = publishing_ ? 0 : playback_.Audio().Open();
        auto* worker = new RtcConnection(std::move(descriptor), publishing_, rate, this);
        connection_ = worker;
        connect(worker, &RtcConnection::Status, status_, &QLabel::setText);
        connect(worker, &RtcConnection::Connected, this, [this, rate] {
            status_->setText(publishing_ ? QStringLiteral("摄像头直播中 · 720p · WebRTC")
                : (rate ? QStringLiteral("正在缓冲音视频") : QStringLiteral("无音频输出设备，正在缓冲画面")));
        });
        connect(worker, &RtcConnection::Failed, status_, &QLabel::setText);
        connect(worker, &QThread::finished, this, [this, worker] {
            connection_ = nullptr; worker->deleteLater(); timer_.stop(); playback_.Close();
            UpdateControls(); FinishClose();
        });
        if (publishing_) { worker->SetPicture(latest_); worker->SetMicrophone(microphone_->isChecked()); }
        worker->start(); timer_.start(); UpdateControls();
    } catch (const std::exception& error) { status_->setText(QString::fromUtf8(error.what())); UpdateControls(); }
}
void CameraWindow::Stop() {
    if (camera_) camera_->stop();
    if (connection_) connection_->Stop();
    timer_.stop(); playback_.Close();
    if (publishing_ && !LiveId().isEmpty() && !ending_) { ending_ = true; emit EndRequested(); }
    UpdateControls();
}
void CameraWindow::SetEnded(const QString& error) {
    ending_ = false;
    if (!error.isEmpty()) {
        closing_ = false; show(); status_->setText(error); UpdateControls(); return;
    }
    live_ = {}; if (connection_) connection_->Stop();
    timer_.stop(); playback_.Close(); status_->setText(QStringLiteral("直播已结束"));
    UpdateControls(); FinishClose();
}
void CameraWindow::closeEvent(QCloseEvent* event) {
    if (!Busy()) { QDialog::closeEvent(event); return; }
    closing_ = true; hide(); Stop(); event->ignore(); FinishClose();
}
void CameraWindow::FinishClose() {
    if (closing_ && !Busy()) QTimer::singleShot(0, this, &QWidget::close);
}
void CameraWindow::UpdateControls() {
    const bool idle = !authorizing_ && !ending_ && !connection_ && !closing_;
    start_->setEnabled(idle && (!publishing_ || !latest_.isNull()));
    title_->setReadOnly(!LiveId().isEmpty() || !idle);
    if (cameras_) cameras_->setEnabled(idle);
}
void CameraWindow::Tick() {
    if (publishing_ || !connection_) return;
    try {
        // RTC 持续接收；GUI 只取走已解码批次，播放策略与 HTTP 直播共用。
        playback_.Append(connection_->TakeMedia());
        if (!playback_.IsRunning()) {
            if (!playback_.TryStart(true)) return;
            status_->setText(playback_.Audio().IsOpen() ? QStringLiteral("WebRTC 直播") : QStringLiteral("WebRTC 直播 · 无声播放"));
        }
        if (playback_.Tick() == Playback::Result::Buffering)
            status_->setText(QStringLiteral("正在缓冲音视频"));
    } catch (const std::exception& error) {
        status_->setText(QString::fromUtf8(error.what())); connection_->Stop(); timer_.stop();
    }
}
