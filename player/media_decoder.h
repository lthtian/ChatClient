#ifndef VIDEO_PLAYER_MEDIA_DECODER_H_
#define VIDEO_PLAYER_MEDIA_DECODER_H_

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QVector>
#include <memory>
#include <atomic>
#include <chrono>

#include "media_source.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// 可跨线程传递的独立画面；时间均相对于同一个文件起点，单位微秒。
struct VideoPicture {
  QImage image;            // 拥有像素的 RGB 图像，不借用 AVFrame 内存。
  qint64 pts_us = 0;       // 画面的显示位置。
  qint64 duration_us = 0;  // 画面的预计持续时间。
};

// 设备格式的 PCM 块：S16、小端、左右声道交错。
struct AudioBlock {
  QByteArray pcm;     // 拥有采样字节，长度为每声道采样数 × 4。
  qint64 pts_us = 0;  // 第一个采样在公共媒体时间轴上的位置。
};

// 一次读包产生的结果；一个包可能没有输出，也可能产生多个帧。
struct MediaBatch {
  QVector<VideoPicture> video;  // 本次可用的视频画面。
  QVector<AudioBlock> audio;    // 本次可用的 PCM 块。
  bool eof = false;             // 两个解码器及重采样器均已排空。
};
Q_DECLARE_METATYPE(MediaBatch)

// 全部 FFmpeg 操作只在工作线程执行；不调用 QWidget 或音频设备。
class MediaDecoder : public QObject {
  Q_OBJECT
 public:
  MediaDecoder();            // 设置各资源的释放方式，尚未打开文件。
  ~MediaDecoder() override;  // 释放复制的声道布局，智能指针释放其他资源。
  void RequestSession(quint64 session);  // GUI 可直接调用：只写原子代次，中断旧网络操作。

 public slots:
  void Open(MediaSource source, quint64 session);  // 按输入类型打开，再共用轨道探测和解码。
  void SetAudioEnabled(bool enabled);  // 无输出设备时跳过音轨，只解码视频。
  void SetOutputRate(int rate);  // 设置设备选定的输出采样率，先于第一次 Read。
  void Read(
      quint64 session);  // 读取一个包并取完其输出，结果通过 BatchReady 返回。
  void Seek(qint64 position_us,
            quint64 session);  // 定位、清状态并切换会话编号。

 signals:
  void Opened(quint64 session, qint64 duration_us, bool has_audio,
              qint64 audio_end_us);  // 返回轨道信息；音轨结束位置未知时为 -1。
  void BatchReady(quint64 session, MediaBatch batch);  // 交付一次请求的结果。
  void Seeked(quint64 session);  // 定位完成，可以继续请求数据。
  void Failed(quint64 session, QString message);  // 返回可展示的错误说明。

 private:
  static int Interrupted(void* opaque);  // FFmpeg 在阻塞读取期间检查取消和操作期限。
  bool Canceled() const;                // 当前请求是否已被 GUI 的更新代次替代。
  void BeginIo();                       // 为一次打开/读取/定位设置有限的网络等待时间。
  void OpenInput(const MediaSource& source);  // 两种输入的差异集中在此处。
  void OpenCodec(const AVStream* stream,
                 AVCodecContext** output);  // 建立解码实例。
  void Send(AVCodecContext* codec, const AVPacket* packet, bool video,
            MediaBatch& batch);  // 处理送包 EAGAIN，并接收全部可用输出。
  int Receive(AVCodecContext* codec, bool video,
              MediaBatch& batch);  // 接收至需输入或 EOF。
  void ConvertVideo(
      MediaBatch& batch);  // 转 RGB，并滤掉 seek 目标前的完整画面。
  void PrepareAudio();     // 按实际音频帧初始化或核对重采样器。
  int ConvertAudio(const AVFrame* source,
                   MediaBatch& batch);      // 转 PCM、标记时间及裁剪。
  void ResetDecodeState(qint64 target_us);  // 清理临时帧、重采样器和时间计数。

  std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)>
      input_;  // 容器与读包状态。
  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)>
      video_;  // 视频解码实例。
  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)>
      audio_;                                              // 可选音频解码实例。
  std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet_;  // 复用的输入包。
  std::unique_ptr<AVFrame, void (*)(AVFrame*)>
      frame_;  // 顺序接收两个解码器的结果。
  std::unique_ptr<AVFrame, void (*)(AVFrame*)> rgb_;  // 像素转换的目标帧。
  std::unique_ptr<SwsContext, void (*)(SwsContext*)>
      scaler_;  // 视频像素转换状态。
  std::unique_ptr<SwrContext, void (*)(SwrContext*)>
      resampler_;                           // 音频转换及延迟状态。
  const AVStream* video_stream_ = nullptr;  // 借用 input_ 的视频轨道。
  const AVStream* audio_stream_ = nullptr;  // 借用 input_ 的音频轨道，可为空。
  AVChannelLayout source_layout_{};  // 复制的输入声道布局，需单独 uninit。
  AVSampleFormat source_format_ = AV_SAMPLE_FMT_NONE;  // 当前输入采样格式。
  int source_rate_ = 0;                                // 当前输入采样率。
  int output_rate_ = 48000;   // Qt 设备支持的目标采样率。
  quint64 session_ = 0;       // 打开/跳转代次，供 GUI 丢弃过期结果。
  std::atomic<quint64> requested_session_{0};  // GUI 与回调之间唯一共享的可变状态。
  std::chrono::steady_clock::time_point io_deadline_;  // 本次操作最晚结束时刻。
  bool online_ = false;      // 仅在线输入使用网络超时；解码流程相同。
  qint64 origin_us_ = 0;      // 所有轨道共同减去的容器起点。
  qint64 seek_origin_us_ = 0;  // HLS 分片索引使用首包 DTS 起点；其他输入使用容器起点。
  bool packet_pending_ = false;  // 探测 HLS 起点时借读的首包，留给首次 Read 解码。
  qint64 target_us_ = 0;      // 当前 seek 目标；更早的数据只用于恢复解码状态。
  qint64 next_video_us_ = 0;  // 视频时间戳缺失时的连续估算值。
  qint64 frame_duration_us_ = 40000;  // 视频时长缺失时按帧率估计。
  qint64 audio_origin_us_ = 0;        // 本次连续音频转换的首采样位置。
  qint64 audio_samples_ = 0;  // 已转换的每声道采样总数，用于避免累积舍入误差。
  bool decode_audio_ = true;   // GUI 协商设备后决定是否消费音轨编码包。
  bool audio_started_ = false;  // 是否已建立音频时间起点。
  bool eof_ = false;            // 是否已完成文件及转换器排空。
};
#endif  // VIDEO_PLAYER_MEDIA_DECODER_H_
