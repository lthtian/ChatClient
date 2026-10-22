#pragma once

#include <QThread>
#include <atomic>
#include <mutex>

class AudioCapture;

// 设备初始化、采集和释放留在同一线程，启停设备不阻塞音视频编码。
class MicrophoneCapture : public QThread {
    Q_OBJECT
public:
    explicit MicrophoneCapture(qint64 origin_us); // 使用发布端公共 QPC 原点。
    ~MicrophoneCapture(); // 通知停止并等待设备线程释放 COM 资源。
    void SetEnabled(bool enabled); // 只交接开关状态，不在调用线程打开设备。
    void Stop(); // 请求退出，正在执行的设备调用返回后完成清理。
    void Mix(float* stereo, int samples, qint64 position); // 叠加对应区间；未就绪或关闭时保持静音。
signals:
    void Status(QString message); // 报告设备准备、可用或失败，失败不终止视频推流。
protected:
    void run() override; // 独占设备生命周期，按短周期读取 PCM。
private:
    const qint64 origin_us_; // 与视频共用的采样时间原点，不随开关麦克风重置。
    std::atomic_bool enabled_{false}, stopping_{false}; // 编码线程交接的开关和停止请求。
    std::mutex mutex_; // 保护 PCM 的 Poll/Mix 和 active_；不覆盖设备初始化或释放。
    AudioCapture* active_ = nullptr; // 借用 run 内的设备；为空时编码端输出静音。
};
