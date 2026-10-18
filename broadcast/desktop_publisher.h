#pragma once

#include <QImage>
#include <QMutex>
#include <QThread>
#include <atomic>

// GUI 提交最新桌面画面；本线程采集声音、编码并发布，网络阻塞不阻塞聊天窗口。
class DesktopPublisher : public QThread {
    Q_OBJECT
public:
    explicit DesktopPublisher(QObject* parent = nullptr); // 建立一场发布任务的线程对象。
    ~DesktopPublisher() override; // 请求取消并等待所有设备、网络资源释放。
    void Configure(const QString& url, bool system_audio, bool microphone); // 启动前设置服务器凭证和声音开关。
    void SubmitImage(QImage image); // 只保留最新一张 720p RGB 图像，不积压桌面帧。
    void SetSystemAudio(bool enabled); // 在线打开或关闭默认播放端点的 loopback 采集。
    void SetMicrophone(bool enabled); // 在线打开或关闭默认麦克风采集。
    void Stop(); // 原子取消标记，同时中断 FFmpeg 网络 I/O。
signals:
    void Publishing(); // 首批音视频已写入发布连接。
    void Failed(const QString& error); // 本次发布失败；不会自动创建另一场直播。
    void AudioUnavailable(bool system_audio, const QString& error); // 此路设备不可用，其他媒体继续发布。
    void Statistics(qint64 elapsed_ms, int kbps); // 每秒报告持续时间和实际发送速率。
protected:
    void run() override; // 拥有 COM 初始化、设备、编码器和本场公共时间原点。
private:
    QString url_; // 短期 RTMP 发布地址，只在本场线程生命周期内保存。
    QMutex image_mutex_; // 保护 GUI 与工作线程共享的图像引用。
    QImage image_; // 最新一张自有像素图像；Qt 隐式共享避免额外拷贝。
    std::atomic_bool stopping_{false}; // 无需工作线程事件循环即可请求取消。
    std::atomic_bool system_audio_{true}; // 用户选择的电脑声音开关。
    std::atomic_bool microphone_{false}; // 用户选择的麦克风开关。
};
