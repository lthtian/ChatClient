#include "playback.h"

#include "video_view.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

Playback::Playback(VideoView* view, QObject* parent) : QObject(parent), view_(view) {}
void Playback::Configure(bool live, bool buffered_input) {
  live_ = live;
  buffered_input_ = buffered_input;
  anchored_ = !live;
}
void Playback::Reset(qint64 position_us) {
  pictures_.clear();
  picture_bytes_ = 0;
  base_us_ = last_video_end_us_ = position_us;
  eof_ = running_ = started_ = audio_clock_ = false;
  anchored_ = !live_;
  clock_.invalidate();
  if (audio_.IsOpen()) audio_.Restart(position_us);
}
void Playback::Close() { audio_.Close(); Reset(); audio_end_us_ = -1; }
void Playback::SetAudioEndUs(qint64 end_us) { audio_end_us_ = end_us; }
void Playback::Anchor(qint64 position_us) {
  base_us_ = last_video_end_us_ = position_us;
  running_ = false;
  audio_clock_ = false;
  clock_.invalidate();
  if (audio_.IsOpen()) audio_.Restart(position_us);
  anchored_ = true;
}
void Playback::TrimLivePictures() {
  // 直播保持接近当前时刻：已解码画面可丢弃，编码参考链仍由输入解码层维护。
  while (pictures_.size() > 1 &&
      (pictures_.size() > 16 || picture_bytes_ > 32 * 1024 * 1024 ||
       pictures_.back().pts_us - pictures_.front().pts_us > 200000)) {
    picture_bytes_ -= pictures_.front().image.sizeInBytes();
    pictures_.pop_front();
  }
}
void Playback::Append(MediaBatch batch) {
  if (live_ && !anchored_) {
    qint64 first = std::numeric_limits<qint64>::max();
    if (!batch.video.isEmpty()) first = batch.video.front().pts_us;
    if (audio_.IsOpen() && !batch.audio.isEmpty())
      first = std::min(first, batch.audio.front().pts_us);
    if (first != std::numeric_limits<qint64>::max()) {
      // 首批数据也可能积压；起播位置须与保留的近期画面对齐。
      if (!batch.video.isEmpty()) first = std::max(first, batch.video.back().pts_us - 150000);
      Anchor(first);
    }
  }
  if (live_ && !batch.video.isEmpty() && batch.video.back().pts_us - PositionUs() > 600000) {
    // 音频和画面一起追赶，不能只丢图像却让设备继续播放过时的 PCM。
    // 断粮冻结后收到较新的媒体，同样需要离开过时的播放位置。
    Anchor(std::max(PositionUs(), batch.video.back().pts_us - 150000));
  }
  for (auto& picture : batch.video) {
    picture_bytes_ += picture.image.sizeInBytes();
    pictures_.push_back(std::move(picture));
  }
  if (live_) TrimLivePictures();
  if (picture_bytes_ > 64 * 1024 * 1024 || pictures_.size() > 180)
    throw std::runtime_error("声画交错间隔过大，画面缓存达到本例上限");
  if (audio_.IsOpen())
    for (auto& block : batch.audio) audio_.Append(std::move(block));
  eof_ = eof_ || batch.eof;
}
qint64 Playback::BufferAheadUs(qint64 requested_us) const {
  if (pictures_.empty()) return std::min<qint64>(requested_us, 150000);
  const auto& picture = pictures_.back();
  const qint64 bytes = std::max<qint64>(1, picture.image.sizeInBytes());
  // 解复用会同时推进音视频；音频预读也要为随之读出的 RGB 帧预留空间。
  const qint64 frames = std::max<qint64>(1, 32 * 1024 * 1024 / bytes - 2);
  return std::min(requested_us, frames * std::max<qint64>(1, picture.duration_us));
}
bool Playback::NeedsData() const {
  if (eof_) return false;
  const qint64 position = PositionUs();
  const qint64 ahead = BufferAheadUs(live_ || !buffered_input_ ? 150000 : 1000000);
  const bool need_video = pictures_.empty() ||
      (pictures_.back().pts_us < position + ahead && picture_bytes_ < 32 * 1024 * 1024);
  const bool need_audio = audio_.IsOpen() && audio_.BufferedEndUs() < position + ahead &&
      (audio_end_us_ < 0 || audio_.BufferedEndUs() + 1000 < audio_end_us_);
  return need_video || need_audio;
}
bool Playback::TryStart(bool play_after, bool show_preview) {
  if (running_) return true;
  if (pictures_.empty() && !(started_ && eof_)) {
    if (eof_) throw std::runtime_error("定位后没有可显示的画面");
    return false;
  }
  // 起播和持续预读共用内存预算，避免等待音频时超过视频缓存上限。
  const qint64 ready_us = BufferAheadUs(live_ || !buffered_input_ ? 100000 : 500000);
  const bool video_ready = !buffered_input_ || !play_after || eof_ ||
      picture_bytes_ >= 32 * 1024 * 1024 ||
      (!pictures_.empty() && pictures_.back().pts_us + pictures_.back().duration_us >= base_us_ + ready_us);
  const bool audio_ready = !audio_.IsOpen() || eof_ || audio_.BufferedEndUs() >= base_us_ + ready_us ||
      (audio_end_us_ >= 0 && audio_.BufferedEndUs() + 1000 >= audio_end_us_);
  if (!video_ready || !audio_ready) return false;
  started_ = true;
  if (play_after) Resume();
  else {
    // 暂停 seek 只显示目标预览，不向设备写 PCM；已有 PCM 留给恢复播放。
    if (show_preview && !pictures_.empty()) PresentFront();
    audio_.Pause();
  }
  return true;
}
void Playback::PresentFront() {
  auto picture = std::move(pictures_.front());
  pictures_.pop_front();
  picture_bytes_ -= picture.image.sizeInBytes();
  last_video_end_us_ = picture.pts_us + picture.duration_us;
  view_->SetImage(std::move(picture.image), picture.geometry);
  emit FramePresented(picture.pts_us);
}
qint64 Playback::PositionUs() const {
  if (!running_) return base_us_;
  if (audio_clock_) return audio_.PositionUs();
  return base_us_ + (clock_.isValid() ? clock_.nsecsElapsed() / 1000 : 0);
}
void Playback::Freeze(qint64 position_us) {
  base_us_ = position_us;
  running_ = false;
  audio_.Pause();
  clock_.invalidate();
}
void Playback::Pause() { Freeze(PositionUs()); }
void Playback::Resume() {
  audio_clock_ = audio_.IsOpen() && (audio_end_us_ < 0 || base_us_ < audio_end_us_);
  audio_.Resume();
  clock_.start();
  running_ = true;
}
Playback::Result Playback::Tick() {
  if (!running_) return Result::Buffering;
  const bool audio_finished = eof_ ||
      (audio_end_us_ >= 0 && audio_.BufferedEndUs() + 1000 >= audio_end_us_);
  audio_.Pump(audio_finished);
  if (audio_clock_ && audio_finished && audio_.Drained()) {
    // 音轨先结束时，以同一个位置接续单调时钟，让剩余视频继续显示。
    base_us_ = audio_.PositionUs();
    audio_clock_ = false;
    clock_.restart();
  }
  const qint64 position = PositionUs();
  while (!pictures_.empty() && pictures_.front().pts_us <= position) {
    if (pictures_.size() > 1 && pictures_[1].pts_us <= position) {
      // 若另一张图也已到显示时间，跳过这张迟到图；只绘制最后一张到期画面。
      last_video_end_us_ = pictures_.front().pts_us + pictures_.front().duration_us;
      picture_bytes_ -= pictures_.front().image.sizeInBytes();
      pictures_.pop_front();
    } else PresentFront();
  }
  if (eof_ && pictures_.empty() && position >= last_video_end_us_ && audio_.Drained()) {
    Freeze(std::max(last_video_end_us_, audio_.IsOpen() ? audio_.BufferedEndUs() : 0));
    return Result::Ended;
  }
  // 网络断粮时冻结设备和时钟，保留当前画面及未消费数据等待补充。
  if (buffered_input_ && !eof_ &&
      ((audio_clock_ && !audio_finished && audio_.BufferedEndUs() - position < 30000) ||
       (pictures_.empty() && position >= last_video_end_us_))) {
    Freeze(audio_clock_ ? position : std::min(position, last_video_end_us_));
    return Result::Buffering;
  }
  return Result::Playing;
}

QJsonObject Playback::Diagnostics() const {
  return {{"clock", audio_clock_ ? "audio_device_estimate" : "monotonic"},
      {"video_front_pts_us", pictures_.empty() ? QJsonValue() : QJsonValue(double(pictures_.front().pts_us))},
      {"video_tail_end_us", pictures_.empty() ? QJsonValue() :
          QJsonValue(double(pictures_.back().pts_us + pictures_.back().duration_us))},
      {"video_queue_bytes", double(picture_bytes_)}, {"video_queue_frames", int(pictures_.size())},
      {"buffer_target_us", double(BufferAheadUs(live_ || !buffered_input_ ? 150000 : 1000000))},
      {"eof", eof_}, {"audio", audio_.Diagnostics()}};
}
