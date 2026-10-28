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
#include "media_types.h"
#include "track_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// 全部 FFmpeg 操作只在工作线程执行；不调用 QWidget 或音频设备。
class MediaDecoder : public QObject {
  Q_OBJECT
 public:
  MediaDecoder();            // 设置各资源的释放方式，尚未打开文件。
  ~MediaDecoder() override;  // 释放输入及两条轨道的解码、转换资源。
  void RequestSession(quint64 session);  // GUI 可直接调用：只写原子代次，中断旧网络操作。

 public slots:
  void Open(MediaSource source, quint64 session);  // 按输入类型打开，再共用轨道探测和解码。
  void Close(quint64 session); // 直播暂停/重连在解码线程释放输入，停止上游连接。
  void SetAudioEnabled(bool enabled);  // 无输出设备时跳过音轨，只解码视频。
  void SetOutputRate(int rate);  // 设置设备选定的输出采样率，先于第一次 Read。
  void Read(
      quint64 session);  // 读取一个包并取完其输出，结果通过 BatchReady 返回。
  void Seek(qint64 position_us,
            quint64 session);  // 定位、清状态并切换会话编号。

 signals:
  void TimelineReady(quint64 session, qint64 origin_us, qint64 seek_origin_us);
  void AudioDiscontinuity(quint64 session, qint64 actual_us, qint64 expected_us);
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
  void ResetDecodeState(qint64 target_us);  // 清理临时帧、重采样器和时间计数。

  std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)>
      input_;  // 容器与读包状态。
  TrackDecoder video_;  // 视频解码与 RGB 转换，归本工作线程所有。
  TrackDecoder audio_;  // 可选音轨的解码与 PCM 转换。
  std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet_;  // 复用的容器输入包。
  const AVStream* video_stream_ = nullptr;  // 借用 input_ 的视频轨道。
  const AVStream* audio_stream_ = nullptr;  // 借用 input_ 的音频轨道，可为空。
  int output_rate_ = 48000;   // Qt 设备支持的目标采样率。
  quint64 session_ = 0;       // 打开/跳转代次，供 GUI 丢弃过期结果。
  std::atomic<quint64> requested_session_{0};  // GUI 与回调之间唯一共享的可变状态。
  std::chrono::steady_clock::time_point io_deadline_;  // 本次操作最晚结束时刻。
  bool online_ = false;      // 仅在线输入使用网络超时；解码流程相同。
  qint64 origin_us_ = 0;      // 所有轨道共同减去的容器起点。
  qint64 seek_origin_us_ = 0;  // HLS 分片索引使用首包 DTS 起点；其他输入使用容器起点。
  bool packet_pending_ = false;  // 探测 HLS 起点时借读的首包，留给首次 Read 解码。
  qint64 frame_duration_us_ = 40000;  // 视频时长缺失时按帧率估计。
  bool decode_audio_ = true;   // GUI 协商设备后决定是否消费音轨编码包。
  bool eof_ = false;            // 是否已完成文件及转换器排空。
  bool live_ = false;           // 直播以本次实际解码起点归零，重连不继承点播位置。
};
#endif  // VIDEO_PLAYER_MEDIA_DECODER_H_
