#pragma once
#include "playback.h"
#include <QDialog>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

class QCamera;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
class CameraSurface;
class RtcConnection;
class VideoView;

// 发布与观看共用媒体会话生命周期，聊天权限和卡片仍由 MainWindow 管理。
class CameraWindow : public QDialog {
    Q_OBJECT
public:
    explicit CameraWindow(bool publishing, QWidget* parent = nullptr); // 发布时显示相机选择和预览。
    ~CameraWindow() override; // 停止摄像头和会话，正常关闭在 finished 后执行。
    void SetExisting(QJsonObject live); // 绑定卡片的直播 ID 和标题。
    void SetAuthorization(QJsonObject live, QJsonObject descriptor, const QString& error); // 业务授权返回后建立 RTC。
    void SetEnded(const QString& error = {}); // 收到结束确认或远端状态结束。
    void Stop(); // 停本地媒体，发布者随后请求业务结束。
    void Start(); // 请求创建/观看授权，重试时重新取得短期票据。
    QString LiveId() const; // 当前卡片 ID。
    bool Busy() const; // 创建、已绑定的发布或媒体关闭过程是否仍需处理。
signals:
    void StartRequested(const QString& title); // 聊天层负责 live_create/live_publish/live_read。
    void EndRequested(); // 发布端请求 live_end；观看端只 DELETE 自己的媒体会话。
    void FramePresented(qint64 time_us); // 真实显示时刻，便于必要联调观测。
protected:
    void closeEvent(QCloseEvent* event) override; // 立即隐藏，异步释放后销毁。
private:
    void SelectCamera(); // 重建选中设备，预览帧交给 GUI 显示和有界编码入口。
    void Tick(); // 取解码批次、音频输出和按公共时间轴显示视频。
    void FinishClose(); // 等授权、业务结束及 RTC 线程退出后完成关闭。
    void UpdateControls(); // 根据设备和会话生命周期启用控件。
    bool publishing_; // 窗口职责，构造后不改变。
    QComboBox* cameras_ = nullptr; // 本机相机列表，不自动选择屏幕采集。
    QCheckBox* microphone_ = nullptr; // 默认关闭，仅明确勾选才采集麦克风。
    QLineEdit* title_; // 发布标题，创建卡片后保持不变。
    QLabel* status_; // 设备、协商、播放或错误说明。
    QPushButton* start_; // 开始或重试；观看也重新申请凭证。
    QPushButton* stop_; // 发布结束或关闭观看。
    VideoView* view_; // 复用已有 RGB 绘制组件。
    QCamera* camera_ = nullptr; // Qt 相机，归 GUI 线程所有。
    CameraSurface* surface_; // 把摄像头帧复制为拥有像素的 QImage。
    QPointer<RtcConnection> connection_; // 异步工作线程，finished 后删除。
    QImage latest_; // 最新预览画面，初次开始时交给编码线程。
    QJsonObject live_; // 本窗口绑定的聊天直播卡片。
    QTimer timer_; // 10ms 消费已解码媒体，不承担解码工作。
    Playback playback_; // GUI 中共用的缓存、音频时钟和画面展示。
    bool authorizing_ = false; // 等待聊天授权，关闭时也要等回执处理业务资源。
    bool ending_ = false; // live_end 请求正在进行。
    bool closing_ = false; // 关闭窗口后等待异步清理。
};
