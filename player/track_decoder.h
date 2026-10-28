#pragma once

#include "media_types.h"
#include <QSize>
#include <functional>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// 一条音频或视频轨道的解码与格式转换；不持有输入、网络或界面对象。
class TrackDecoder {
 public:
  TrackDecoder();   // 分配可复用帧，尚未打开解码器。
  ~TrackDecoder();  // 释放声道布局和 FFmpeg 资源。
  void Open(const AVCodecParameters& parameters, AVRational time_base,
            qint64 frame_duration_us = 40000, AVRational stream_sar = {0, 1});  // 复制编码参数，保留包的时间基。
  void Close();                      // 释放当前轨道及转换状态。
  bool IsOpen() const;               // 是否已经打开解码实例。
  void SetOutputRate(int rate);      // 解码前指定音频设备采样率。
  void SetVideoLimit(QSize limit);   // 输入侧设置可接受的画面上限，空值表示不额外限制。
  void Reset(qint64 target_us = 0);  // 清参考帧、转换缓存及计数，从目标位置重新输出。
  void Decode(const AVPacket* packet, qint64& origin_us, MediaBatch& batch,
              const std::function<bool()>& canceled = {});
  // origin_us 可由首份解码输出建立；音视频必须传入同一变量，不能分别归零。
  void DecodeUnit(const QByteArray& bytes, qint64 pts, qint64 dts,
                  qint64& origin_us, MediaBatch& batch);  // 复制完整访问单元并按本轨时间基送入。
  void Drain(qint64& origin_us, MediaBatch& batch,
             const std::function<bool()>& canceled = {});  // 排空解码器和 PCM 尾部。

  std::function<void(qint64, qint64)> audio_discontinuity;  // 在解码线程报告音轨时间跳变。

 private:
  int Receive(qint64& origin_us, MediaBatch& batch,
              const std::function<bool()>& canceled);  // 收到需输入、EOF 或取消为止。
  void ConvertVideo(qint64 origin_us, MediaBatch& batch);  // 转为拥有像素的 RGB 图像。
  void PrepareAudio();  // 按实际音频帧配置或核对转换器。
  int ConvertAudio(const AVFrame* source, qint64 origin_us,
                   MediaBatch& batch);  // 重采样、维护采样时间并裁掉目标前的数据。

  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> context_;  // 本轨道解码实例。
  std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet_;  // 为网络访问单元提供带安全填充的编码缓冲。
  std::unique_ptr<AVFrame, void (*)(AVFrame*)> frame_;  // 顺序接收输出帧。
  std::unique_ptr<AVFrame, void (*)(AVFrame*)> rgb_;    // 像素转换的目标帧。
  std::unique_ptr<SwsContext, void (*)(SwsContext*)> scaler_;  // 视频格式转换状态。
  std::unique_ptr<SwrContext, void (*)(SwrContext*)> resampler_;  // PCM 转换及内部延迟。
  AVChannelLayout source_layout_{};  // 复制的输入声道布局，析构时单独释放。
  AVSampleFormat source_format_ = AV_SAMPLE_FMT_NONE;  // 输入 PCM 格式。
  int source_rate_ = 0;              // 输入音频采样率。
  int output_rate_ = 48000;          // 输出设备的采样率。
  AVRational time_base_{1, 1000000}; // 编码包和解码时间戳的单位。
  AVRational stream_sar_{0, 1};
  QByteArray display_matrix_;  // 复制流级显示矩阵，不借用输入资源。
  QSize video_limit_;               // 协议输入选择的尺寸限制。
  qint64 target_us_ = 0;             // 允许输出的最早媒体位置。
  qint64 next_video_us_ = 0;         // 缺失视频时间戳时的连续估计。
  qint64 frame_duration_us_ = 40000; // 缺失帧时长时的估计。
  qint64 audio_origin_us_ = 0;       // 当前连续 PCM 段的起始位置。
  qint64 audio_samples_ = 0;         // 已转换的每声道采样数。
  bool audio_started_ = false;       // 是否已经建立当前 PCM 段的时间起点。
};
