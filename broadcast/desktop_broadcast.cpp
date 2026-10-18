#include "desktop_broadcast.h"
#include "desktop_publisher.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

DesktopBroadcast::DesktopBroadcast(QWidget* parent) : QDialog(parent),
    screens_(new QComboBox(this)), title_(new QLineEdit(QStringLiteral("我的桌面直播"), this)),
    system_audio_(new QCheckBox(QStringLiteral("电脑声音"), this)),
    microphone_(new QCheckBox(QStringLiteral("麦克风"), this)), preview_(new QLabel(this)),
    status_(new QLabel(QStringLiteral("选择显示器，点击开始后自动推流。"), this)),
    audio_status_(new QLabel(this)), start_(new QPushButton(QStringLiteral("开始直播"), this)),
    end_(new QPushButton(QStringLiteral("结束直播"), this)), publisher_(new DesktopPublisher(this)) {
    setWindowTitle(QStringLiteral("桌面直播"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(600, 490);
    setMinimumWidth(480);
    title_->setMaxLength(120);
    screens_->setObjectName("broadcastScreen"); title_->setObjectName("broadcastTitle");
    system_audio_->setObjectName("broadcastSystemAudio"); microphone_->setObjectName("broadcastMicrophone");
    status_->setObjectName("broadcastStatus"); preview_->setObjectName("broadcastPreview");
    start_->setObjectName("broadcastStart"); end_->setObjectName("broadcastEnd");
    system_audio_->setChecked(true);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumSize(400, 225);
    preview_->setStyleSheet("background:#111827;border-radius:6px;");
    status_->setWordWrap(true); audio_status_->setWordWrap(true);
    audio_status_->setStyleSheet("color:#a55b00;");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16); layout->setSpacing(12);
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("直播名称"), title_); form->addRow(QStringLiteral("共享显示器"), screens_);
    layout->addLayout(form); layout->addWidget(preview_);
    auto* audio = new QHBoxLayout;
    audio->addWidget(system_audio_); audio->addWidget(microphone_); audio->addStretch();
    audio->addWidget(new QLabel(QStringLiteral("720p · 30 帧"), this));
    layout->addLayout(audio); layout->addWidget(audio_status_); layout->addWidget(status_);
    auto* footer = new QHBoxLayout;
    footer->addStretch(); footer->addWidget(start_); footer->addWidget(end_); layout->addLayout(footer);
    connect(start_, &QPushButton::clicked, this, &DesktopBroadcast::Start);
    connect(end_, &QPushButton::clicked, this, &DesktopBroadcast::End);
    connect(system_audio_, &QCheckBox::toggled, publisher_, &DesktopPublisher::SetSystemAudio);
    connect(microphone_, &QCheckBox::toggled, publisher_, &DesktopPublisher::SetMicrophone);
    connect(&capture_timer_, &QTimer::timeout, this, &DesktopBroadcast::Capture);
    connect(screens_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { Capture(); });
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { RefreshScreens(); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen* removed) {
        if (screen_ == removed && publisher_->isRunning()) {
            capture_timer_.stop(); publisher_->Stop();
            status_->setText(QStringLiteral("共享的显示器已断开，请重新选择后开始。"));
        }
        RefreshScreens();
    });
    connect(publisher_, &DesktopPublisher::Publishing, this, [this] {
        if (!ending_) status_->setText(QStringLiteral("直播中 · 电脑声音和麦克风可独立开关"));
    });
    connect(publisher_, &DesktopPublisher::Failed, this, [this](const QString& error) {
        capture_timer_.stop();
        status_->setText(QStringLiteral("推流失败：") + error + QStringLiteral("。可点击重新开播。"));
    });
    connect(publisher_, &QThread::finished, this, [this] { UpdateControls(); FinishClose(); });
    connect(publisher_, &DesktopPublisher::AudioUnavailable, this, [this](bool system, const QString& error) {
        auto* check = system ? system_audio_ : microphone_;
        const QSignalBlocker block(check); check->setChecked(false);
        audio_status_->setText((system ? QStringLiteral("电脑声音：") : QStringLiteral("麦克风：")) + error);
    });
    connect(publisher_, &DesktopPublisher::Statistics, this, [this](qint64 ms, int kbps) {
        if (!ending_) status_->setText(QStringLiteral("直播中 · %1:%2 · %3 kbps")
            .arg(ms / 60000, 2, 10, QChar('0')).arg(ms / 1000 % 60, 2, 10, QChar('0')).arg(kbps));
    });
    RefreshScreens();
    capture_timer_.setTimerType(Qt::PreciseTimer);
    capture_timer_.start(200);
    UpdateControls();
}

DesktopBroadcast::~DesktopBroadcast() { Stop(); publisher_->wait(); }
QString DesktopBroadcast::LiveId() const { return live_["live_id"].toString(); }
bool DesktopBroadcast::Busy() const { return authorizing_ || ending_ || publisher_->isRunning() || !LiveId().isEmpty(); }
void DesktopBroadcast::Stop() { capture_timer_.stop(); publisher_->Stop(); }

void DesktopBroadcast::SetExisting(const QJsonObject& conversation, const QJsonObject& live) {
    conversation_ = conversation; live_ = live;
    title_->setText(live["title"].toString());
    status_->setText(QStringLiteral("点击开始直播，将桌面发布到这张直播卡片。"));
    UpdateControls();
}

void DesktopBroadcast::SetAuthorization(const QJsonObject& conversation, const QJsonObject& live,
                                         const QJsonObject& descriptor, const QString& error) {
    authorizing_ = false;
    if (!error.isEmpty()) {
        status_->setText(QStringLiteral("申请开播失败：") + error);
        if (close_after_end_ && LiveId().isEmpty()) { close_after_end_ = false; close(); return; }
        if (close_after_end_) { End(); return; }
        UpdateControls(); return;
    }
    conversation_ = conversation; live_ = live;
    if (close_after_end_) { End(); return; }
    const QUrl url(descriptor["url"].toString());
    const QUrl root(qEnvironmentVariable("CHAT_LIVE_RTMP_URL", "rtmp://39.105.18.142:1935/live"));
    if (!url.isValid() || url.scheme() != "rtmp" || url.host() != root.host() ||
        url.port(1935) != root.port(1935) || !url.userInfo().isEmpty() || !url.fragment().isEmpty() ||
        url.path() != root.path() + '/' + LiveId() || QUrlQuery(url).queryItemValue("token").isEmpty()) {
        status_->setText(QStringLiteral("服务器返回的推流地址无效")); UpdateControls(); return;
    }
    Capture();
    if (image_.isNull()) { status_->setText(QStringLiteral("无法采集所选显示器")); UpdateControls(); return; }
    audio_status_->clear();
    publisher_->Configure(url.toString(), system_audio_->isChecked(), microphone_->isChecked());
    publisher_->SubmitImage(image_);
    status_->setText(QStringLiteral("连接直播服务器并启动编码…"));
    capture_timer_.start(33); publisher_->start();
    UpdateControls();
}

void DesktopBroadcast::Start() {
    if (authorizing_ || ending_ || publisher_->isRunning()) return;
    if (title_->text().trimmed().isEmpty()) { status_->setText(QStringLiteral("请填写直播名称")); return; }
    Capture();
    if (image_.isNull()) { status_->setText(QStringLiteral("无法采集所选显示器")); return; }
    authorizing_ = true;
    status_->setText(QStringLiteral("正在申请开播…")); UpdateControls();
    emit StartRequested(title_->text().trimmed());
}

void DesktopBroadcast::End() {
    if (ending_ || authorizing_) return;
    capture_timer_.stop(); publisher_->Stop();
    if (LiveId().isEmpty()) { close_after_end_ = true; FinishClose(); return; }
    ending_ = true;
    status_->setText(QStringLiteral("正在结束直播…")); UpdateControls();
    emit EndRequested();
}

void DesktopBroadcast::SetEnded(const QString& error) {
    ending_ = false;
    Stop();
    if (!error.isEmpty()) {
        if (close_after_end_) { show(); raise(); }
        close_after_end_ = false;
        status_->setText(QStringLiteral("服务端结束未确认：") + error + QStringLiteral("，请重试结束。"));
    } else {
        live_ = {}; conversation_ = {};
        setProperty("create_request_id", QVariant());
        status_->setText(QStringLiteral("直播已结束"));
    }
    UpdateControls();
    FinishClose();
}

void DesktopBroadcast::Capture() {
    const auto screens = QGuiApplication::screens();
    const int index = screens_->currentIndex();
    if (index < 0 || index >= screens.size()) { image_ = {}; return; }
    screen_ = screens[index];
    const auto grabbed = screen_->grabWindow(0);
    if (grabbed.isNull()) {
        image_ = {};
        if (publisher_->isRunning()) { publisher_->Stop(); status_->setText(QStringLiteral("桌面采集失败，已停止推流")); }
        return;
    }
    const auto scaled = grabbed.toImage().scaled(1280, 720, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    image_ = QImage(1280, 720, QImage::Format_RGB888);
    image_.fill(Qt::black);
    { QPainter painter(&image_); painter.drawImage((1280 - scaled.width()) / 2, (720 - scaled.height()) / 2, scaled); }
    preview_->setPixmap(QPixmap::fromImage(image_).scaled(480, 270, Qt::KeepAspectRatio, Qt::FastTransformation));
    if (publisher_->isRunning()) publisher_->SubmitImage(image_);
}

void DesktopBroadcast::RefreshScreens() {
    const QSignalBlocker block(screens_);
    QScreen* selected = screen_ ? screen_.data() : QGuiApplication::primaryScreen();
    screens_->clear();
    const auto screens = QGuiApplication::screens();
    for (auto* screen : screens) screens_->addItem(QStringLiteral("%1%2 · %3×%4")
        .arg(screen == QGuiApplication::primaryScreen() ? QStringLiteral("主屏 · ") : QString(), screen->name())
        .arg(screen->size().width()).arg(screen->size().height()));
    screens_->setCurrentIndex(screens.indexOf(selected));
    if (screens_->currentIndex() < 0 && !screens.isEmpty()) screens_->setCurrentIndex(0);
}

void DesktopBroadcast::UpdateControls() {
    const bool active = authorizing_ || ending_ || publisher_->isRunning();
    start_->setEnabled(!active); end_->setEnabled(!authorizing_ && !ending_ && !LiveId().isEmpty());
    start_->setText(LiveId().isEmpty() ? QStringLiteral("开始直播") : QStringLiteral("重新开播"));
    screens_->setEnabled(!active); title_->setEnabled(!active && LiveId().isEmpty());
}

void DesktopBroadcast::closeEvent(QCloseEvent* event) {
    if (Busy()) {
        close_after_end_ = true; event->ignore();
        // 窗口立即消失，保留对象接收结束回执和线程完成信号，不阻塞用户界面。
        hide();
        if (!authorizing_) End();
        else status_->setText(QStringLiteral("等待创建结果后结束直播…"));
        return;
    }
    Stop(); QDialog::closeEvent(event);
}

void DesktopBroadcast::FinishClose() {
    if (!close_after_end_ || authorizing_ || ending_ || !LiveId().isEmpty() || publisher_->isRunning()) return;
    close_after_end_ = false;
    close();
}
