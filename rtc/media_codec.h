#pragma once
#include "media_decoder.h"
#include <QByteArray>
#include <QImage>
#include <functional>
#include <memory>

// 编码后的访问单元交给 RTP 层；此处不创建容器、HTTP 或 RTMP 连接。
class CameraEncoder {
public:
    using Output = std::function<void(bool video, QByteArray bytes, qint64 time_us)>;
    explicit CameraEncoder(Output output); // H.264 baseline 720p30、Opus 48k stereo / 20ms。
    ~CameraEncoder(); // 释放 FFmpeg 上下文和复用帧。
    void Video(const QImage& image, qint64 time_us, bool keyframe); // 等比缩放补边，按采集时刻编码。
    void Audio(const float* stereo, qint64 sample_position); // 输入每声道 960 个采样。
private:
    struct Data;
    std::unique_ptr<Data> data_; // 编码线程独占，编码回调在同一线程执行。
};

// 接收已经重组成完整访问单元的数据；解码器不处理 ICE、SRTP 或 RTP 头。
class CameraDecoder {
public:
    explicit CameraDecoder(int output_rate); // 输出采样率由 Qt 音频设备协商，0 表示只看视频。
    ~CameraDecoder(); // 释放解码器和图像/音频转换器。
    void Decode(bool video, const QByteArray& bytes, qint64 time_us, MediaBatch& batch);
    void ResetVideo(); // 参考链损坏后清空解码缓存，再等待完整 IDR。
private:
    struct Data;
    std::unique_ptr<Data> data_; // 接收线程独占；结果复制为拥有内存的 QImage/QByteArray。
};
