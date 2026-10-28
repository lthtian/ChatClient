#pragma once

#include "audio_output.h"
#include <QElapsedTimer>
#include <QObject>
#include <deque>

class VideoView;

// GUI 线程中的公共播放调度；输入层只交付 MediaBatch，窗口负责定时调用 Tick。
class Playback : public QObject {
  Q_OBJECT
 public:
  enum class Result { Playing, Buffering, Ended };
  explicit Playback(VideoView* view, QObject* parent = nullptr);  // 借用窗口拥有的画布。
  void Configure(bool live, bool buffered_input);  // 选择直播策略及是否需要网络预缓冲。
  void Reset(qint64 position_us = 0);  // 清媒体缓存、设备时间和播放锚点，保留设备。
  void Close();  // 立即释放音频设备并清空本次播放状态。
  void SetAudioEndUs(qint64 end_us);  // 已知音轨结束位置，直播或未知时为 -1。
  void Append(MediaBatch batch);     // 接收独立拥有内存的图像和 PCM，检查容量。
  bool TryStart(bool play_after, bool show_preview = true);  // 数据就绪后播放或显示暂停预览。
  void Pause();   // 冻结媒体位置并暂停设备，保留未播放数据。
  void Resume();  // 从同一媒体位置继续设备与时钟。
  Result Tick();  // 补音频、选择到期画面，并返回结束或断粮状态。
  bool NeedsData() const;  // 由拉取输入决定是否继续 Read；RTC 仍持续接收。
  qint64 PositionUs() const;  // 唯一的播放时钟，暂停时保持不变。
  bool IsRunning() const { return running_; }  // 是否正在消费媒体。
  QJsonObject Diagnostics() const;  // GUI 线程读取时钟、队列与音频输出状态。
  bool Eof() const { return eof_; }           // 是否收到输入排空标记。
  AudioOutput& Audio() { return audio_; }  // 窗口协商设备、设置音量；播放时序由本类管理。
  const AudioOutput& Audio() const { return audio_; }  // 查询实际设备状态。

 signals:
  void FramePresented(qint64 pts_us);  // 图像真正交给画布后通知窗口或观察者。

 private:
  qint64 BufferAheadUs(qint64 requested_us) const;  // 同时限制预读时间和 RGB 内存预算。
  void PresentFront();  // 移出队首画面并交给画布，维护缓存计数。
  void Freeze(qint64 position_us);  // 暂停设备并保存同一媒体时间位置。
  void Anchor(qint64 position_us);  // 直播起播或追赶时重建设备和公共播放起点。
  void TrimLivePictures();  // 丢弃过时的已解码画面，不破坏编码参考链。

  VideoView* view_;             // 借用 GUI 画布，本类不销毁它。
  AudioOutput audio_;           // 设备输出及带时间戳的 PCM 缓冲。
  QElapsedTimer clock_;         // 无音频或音轨结束后的单调时钟。
  std::deque<VideoPicture> pictures_;  // 待显示的独立图像。
  qint64 picture_bytes_ = 0;    // 图像缓存字节量。
  qint64 base_us_ = 0;          // 暂停位置或单调时钟锚点。
  qint64 last_video_end_us_ = 0;  // 最近消费画面的结束位置。
  qint64 audio_end_us_ = -1;    // 输入描述的音轨结束位置。
  bool live_ = false;           // 允许有限丢画面和追赶当前直播位置。
  bool buffered_input_ = false; // 网络输入起播及断粮需要缓冲。
  bool anchored_ = true;        // 直播首批媒体是否已经确定播放起点。
  bool started_ = false;        // 本次输入是否曾准备好可显示画面。
  bool running_ = false;        // 时钟是否正在推进。
  bool audio_clock_ = false;    // 是否采用设备消费进度为主时钟。
  bool eof_ = false;            // 解码器及重采样器都已排空。
};
