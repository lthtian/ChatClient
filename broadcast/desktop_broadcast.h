#pragma once

#include <QDialog>
#include <QImage>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScreen;
class DesktopPublisher;

// 桌面直播控制窗口；聊天层处理授权和业务结束，本窗口处理采集与本地发布生命周期。
class DesktopBroadcast : public QDialog {
    Q_OBJECT
public:
    explicit DesktopBroadcast(QWidget* parent = nullptr); // 默认选择主屏，预览选中显示器。
    ~DesktopBroadcast() override; // 停止定时采集并等待推流线程退出。
    void SetExisting(const QJsonObject& conversation, const QJsonObject& live); // 从历史卡片重新开播同一场直播。
    void SetAuthorization(const QJsonObject& conversation, const QJsonObject& live, const QJsonObject& descriptor, const QString& error); // 接收聊天授权结果。
    void SetEnded(const QString& error); // 服务端结束结果，失败允许重试结束。
    void Stop(); // 请求关闭采集和发布，不在界面线程等待网络退出。
    QString LiveId() const; // 当前绑定的直播 ID；尚未创建时为空。
    bool Busy() const; // 正在申请授权、推流、结束或已绑定未结束直播。
signals:
    void StartRequested(const QString& title); // 用户点击开始，向聊天层申请创建或重推凭证。
    void EndRequested(); // 先停本地发布，再由聊天层结束服务端直播。
protected:
    void closeEvent(QCloseEvent* event) override; // 立即隐藏，结束确认及线程退出后销毁对象。
private:
    void Capture(); // GUI 线程抓屏，等比缩放并补黑边到 1280×720。
    void Start(); // 校验画面后请求授权，禁止重复启动。
    void End(); // 停采集、请求取消发布并通知聊天层结束。
    void UpdateControls(); // 根据本地状态设置开始、结束及选屏控件。
    void FinishClose(); // 服务端结束确认及推流线程退出后销毁隐藏的控制窗口。
    void RefreshScreens(); // 显示器插拔时更新选择；选中显示器断开则停止发布。
    QComboBox* screens_; // 显示器列表，默认主屏。
    QLineEdit* title_; // 直播标题，创建后保持不变。
    QCheckBox* system_audio_; // 电脑声音独立开关。
    QCheckBox* microphone_; // 麦克风独立开关。
    QLabel* preview_; // 缩小的采集画面预览。
    QLabel* status_; // 授权、连接、运行或失败提示。
    QLabel* audio_status_; // 音频设备失败提示，不覆盖推流状态。
    QPushButton* start_; // 开始或重试发布。
    QPushButton* end_; // 结束当前直播。
    DesktopPublisher* publisher_; // 采音、编码和网络输出线程。
    QTimer capture_timer_; // 主线程约 30fps 抓屏，网络不在此线程运行。
    QPointer<QScreen> screen_; // 正在采集的显示器，断开后自动变空。
    QImage image_; // 最新采集的完整 720p RGB 图像。
    QJsonObject conversation_; // 绑定创建时的聊天会话，切换联系人不改变它。
    QJsonObject live_; // 已创建的直播元数据。
    bool authorizing_ = false; // 正等待业务凭证。
    bool ending_ = false; // 已请求服务端结束，防止重复请求。
    bool close_after_end_ = false; // 标题栏关闭等待结束结果后再关闭窗口。
};
