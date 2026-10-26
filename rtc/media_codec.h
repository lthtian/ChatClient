#pragma once
#include "media_types.h"
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
