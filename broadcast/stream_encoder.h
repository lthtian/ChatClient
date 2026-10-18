#pragma once

#include <QImage>
#include <QString>
#include <atomic>
#include <memory>

// 单场直播的 H.264/AAC 编码器与 FLV/RTMP 输出；只在推流线程使用。
class StreamEncoder {
public:
    StreamEncoder(const QString& url, const std::atomic_bool& stopping); // 建立 RTMP 发布连接，写编码配置。
    ~StreamEncoder(); // 关闭连接和编码器；直播停止通过断开发布会话结束。
    void Video(const QImage& image, qint64 index); // RGB 图像转 YUV420P，以 1/30 秒为时间基编码。
    void Audio(const float* stereo, qint64 position); // 编码 1024 个双声道采样，时间基为 1/48000 秒。
    qint64 BytesWritten() const; // AVIO 已写出字节，用于显示推流速率。
private:
    struct Data;
    std::unique_ptr<Data> data_; // 拥有本次发布所需的 FFmpeg 上下文和帧。
};
