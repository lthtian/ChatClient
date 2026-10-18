#pragma once

#include <QtGlobal>
#include <memory>

// 与 WASAPI 返回的 QPC 时间戳使用同一个单调时钟，单位为微秒。
qint64 CaptureClockUs();

// 一路默认 Windows 音频端点；输出统一为 48 kHz 双声道交错 float。
class AudioCapture {
public:
    explicit AudioCapture(bool loopback); // true 采集电脑输出，false 采集麦克风。
    ~AudioCapture(); // 停止端点并释放 COM、重采样和短期 PCM 缓冲。
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;
    void Poll(qint64 origin_us); // 读取当前端点包，按公共时间原点标记 PCM。
    void Mix(float* stereo, int samples, qint64 position, float gain); // 叠加给定采样区间，缺失处保持静音。
private:
    struct Data;
    std::unique_ptr<Data> data_; // 仅供推流线程访问的设备、转换器和有界 PCM 队列。
};
