#ifndef VIDEO_PLAYER_AUDIO_OUTPUT_H_
#define VIDEO_PLAYER_AUDIO_OUTPUT_H_

#include <QAudioDeviceInfo>
#include <QAudioOutput>
#include <deque>
#include <memory>

#include "media_decoder.h"

// GUI 线程中的设备输出。只接收目标格式 PCM，不负责解码或显示画面。
class AudioOutput {
 public:
  int Open();  // 返回设备实际采样率；没有设备返回 0，设备配置失败抛异常。
  void Restart(qint64 position_us);  // 丢弃旧声音，从指定媒体位置建立输出计数。
  void Close();                      // 立即清空并释放设备，不等待旧声音播放完。
  void Append(AudioBlock block);     // 保留带时间戳的 PCM，按容量上限检查积压。
  void Pump(
      bool finished);  // 在设备有空间时写 PCM；结束时给不足一周期的尾部补零。
  void Pause();        // 暂停设备并保留缓冲。
  void Resume();       // 恢复设备中的同一批数据。
  void SetVolume(int percent, bool muted);  // 设置本播放器音量，百分比 0～100。
  qint64 PositionUs() const;     // 扣除设备排队数据后的媒体位置估计，单位微秒。
  qint64 BufferedEndUs() const;  // 已接收有效 PCM 的媒体结束位置。
  bool Drained() const;          // 应用和设备中的数据是否都已消费。
  bool IsOpen() const {
    return output_ != nullptr;
  }  // 是否已创建音频输出对象。
  int SampleRate() const { return format_.sampleRate(); }  // 实际目标采样率。
  QString Description() const;  // 设备名称与协商格式，用于状态说明。

 private:
  void FillPeriod(bool finished);  // 按时间戳补间隙、裁重叠，组成一个设备周期。
  qint64 FramesForUs(qint64 us) const;  // 微秒换算为每声道采样位置，四舍五入。
  qint64 UsForBytes(qint64 bytes) const;  // S16 stereo 字节量换算为微秒。

  QAudioDeviceInfo device_;  // 系统默认输出设备的描述，不是正在播放的实例。
  QAudioFormat
      format_;  // 协商后的 PCM 格式；固定双声道 S16，采样率按支持情况选择。
  std::unique_ptr<QAudioOutput> output_;  // 实际音频输出实例，归本模块所有。
  QIODevice* sink_ =
      nullptr;  // start() 返回的借用指针；reset/stop 后立即作废。
  std::deque<AudioBlock>
      blocks_;                // 已接收、尚未组成写入块的 PCM，保留其媒体位置。
  QByteArray period_;         // 待提交的一个设备周期，部分写入后保留剩余字节。
  qint64 pending_bytes_ = 0;  // blocks_ 中 PCM 字节量，用于限制应用缓冲。
  qint64 base_us_ = 0;        // 本次 start 对应的媒体位置，打开或 seek 时重建。
  qint64 assembled_frames_ =
      0;                      // 已组成周期的每声道采样数，包含时间间隙的静音。
  qint64 written_bytes_ = 0;  // 已被 QIODevice 接受的字节量，不代表已播放量。
  qint64 end_us_ = 0;         // 最近收到的有效音频结束位置，不含尾部补零。
  qint64 paused_us_ = 0;      // 暂停时冻结的媒体时钟。
  int block_offset_ = 0;      // 队首 PCM 已消耗的字节偏移。
  int volume_ = 70;           // 用户音量设置，静音时仍保留。
  bool muted_ = false;        // 是否临时将设备音量置零。
  bool paused_ = false;       // 是否处于设备暂停状态。
};
#endif  // VIDEO_PLAYER_AUDIO_OUTPUT_H_
