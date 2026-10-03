#include "audio_output.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

int AudioOutput::Open() {
  Close();
  if (QAudioDeviceInfo::availableDevices(QAudio::AudioOutput).isEmpty()) return 0;
  device_ = QAudioDeviceInfo::defaultOutputDevice();
  if (device_.isNull()) return 0;
  format_.setCodec("audio/pcm");
  format_.setChannelCount(2);
  format_.setSampleSize(16);
  format_.setSampleType(QAudioFormat::SignedInt);
  format_.setByteOrder(QAudioFormat::LittleEndian);
  // 输出配置必须与重采样器相同。只在本例已实现的 S16 stereo 范围内协商。
  QList<int> rates{48000, 44100};
  for (int rate : device_.supportedSampleRates())
    if (!rates.contains(rate)) rates.append(rate);
  bool supported = false;
  for (int rate : rates) {
    if (rate <= 0) continue;
    format_.setSampleRate(rate);
    if (device_.isFormatSupported(format_)) {
      supported = true;
      break;
    }
  }
  if (!supported)
    throw std::runtime_error("设备不支持本例的双声道 S16 PCM 输出");
  output_ = std::make_unique<QAudioOutput>(device_, format_);
  // 目标缓冲 100ms；实际值由后端决定，start 后读取 bufferSize/periodSize。
  output_->setBufferSize(format_.bytesForDuration(100000));
  SetVolume(volume_, muted_);
  return format_.sampleRate();
}

void AudioOutput::Restart(qint64 position_us) {
  if (!output_) return;
  sink_ = nullptr;
  output_->reset();
  blocks_.clear();
  period_.clear();
  pending_bytes_ = assembled_frames_ = written_bytes_ = 0;
  block_offset_ = 0;
  base_us_ = end_us_ = paused_us_ = position_us;
  paused_ = false;
  // 无参数 start 返回设备内部可写 QIODevice，模块只借用，不 delete。
  sink_ = output_->start();
  if (!sink_ || output_->error() != QAudio::NoError)
    throw std::runtime_error("无法启动音频输出设备");
  if (output_->periodSize() <= 0 || output_->periodSize() % 4 != 0)
    throw std::runtime_error("设备周期没有按完整双声道采样对齐");
}

void AudioOutput::Close() {
  sink_ = nullptr;
  if (output_) output_->reset();
  output_.reset();
  blocks_.clear();
  period_.clear();
  pending_bytes_ = 0;
}

qint64 AudioOutput::FramesForUs(qint64 us) const {
  return static_cast<qint64>(
      std::llround(us * (format_.sampleRate() / 1000000.0)));
}
qint64 AudioOutput::UsForBytes(qint64 bytes) const {
  return bytes * 1000000 / (format_.sampleRate() * 4LL);
}

void AudioOutput::Append(AudioBlock block) {
  if (!output_ || block.pcm.isEmpty()) return;
  if (block.pcm.size() % 4)
    throw std::runtime_error("PCM 块不是完整的双声道采样");
  pending_bytes_ += block.pcm.size();
  if (pending_bytes_ > format_.bytesForDuration(2000000))
    throw std::runtime_error("音频积压超过两秒；本例要求音视频正常交错的媒体");
  end_us_ = std::max(end_us_, block.pts_us + UsForBytes(block.pcm.size()));
  blocks_.push_back(std::move(block));
}

void AudioOutput::FillPeriod(bool finished) {
  const int capacity = output_->periodSize();
  while (period_.size() < capacity && !blocks_.empty()) {
    const AudioBlock& block = blocks_.front();
    const qint64 start =
        FramesForUs(block.pts_us - base_us_) + block_offset_ / 4;
    const qint64 gap = start - assembled_frames_;
    if (gap > 0) {
      // 音轨晚于视频开始时，只按一个周期补静音，不能一次分配整个间隙。
      const int bytes = static_cast<int>(
          std::min<qint64>(gap * 4, capacity - period_.size()));
      period_.append(QByteArray(bytes, '\0'));
      assembled_frames_ += bytes / 4;
      continue;
    }
    if (gap < 0) {
      const int skip = static_cast<int>(
          std::min<qint64>(-gap * 4, block.pcm.size() - block_offset_));
      block_offset_ += skip;
      pending_bytes_ -= skip;
    }
    const int bytes =
        std::min(capacity - period_.size(), block.pcm.size() - block_offset_);
    period_.append(block.pcm.constData() + block_offset_, bytes);
    block_offset_ += bytes;
    pending_bytes_ -= bytes;
    assembled_frames_ += bytes / 4;
    if (block_offset_ == block.pcm.size()) {
      blocks_.pop_front();
      block_offset_ = 0;
    }
  }
  if (finished && blocks_.empty() && !period_.isEmpty() &&
      period_.size() < capacity) {
    // 最后一周期补静音后提交；媒体结束位置仍取 end_us_，不包含补齐部分。
    const int padding = capacity - period_.size();
    period_.append(QByteArray(padding, '\0'));
    assembled_frames_ += padding / 4;
  }
}

void AudioOutput::Pump(bool finished) {
  if (!output_ || paused_) return;
  if (output_->state() == QAudio::StoppedState &&
      output_->error() != QAudio::NoError)
    throw std::runtime_error("音频设备停止，错误码 " +
                             std::to_string(output_->error()));
  while (output_->bytesFree() >= output_->periodSize()) {
    FillPeriod(finished);
    if (period_.size() < output_->periodSize()) break;
    const qint64 count = sink_->write(period_.constData(), period_.size());
    if (count < 0) throw std::runtime_error("写入音频设备失败");
    if (count == 0) break;
    written_bytes_ += count;
    period_.remove(0, static_cast<int>(count));
  }
}

qint64 AudioOutput::PositionUs() const {
  if (!output_) return base_us_;
  if (paused_) return paused_us_;
  // Qt 5 Windows 后端 processedUSecs 统计已提交字节，不等于已经播完。
  // 每次按周期写入后，以“提交量 - 缓冲占用”估算已消费量，精度约为一个周期。
  const qint64 queued =
      std::max(0, output_->bufferSize() - output_->bytesFree());
  return std::min(end_us_, base_us_ + UsForBytes(std::max<qint64>(
                                          0, written_bytes_ - queued)));
}
qint64 AudioOutput::BufferedEndUs() const { return end_us_; }
bool AudioOutput::Drained() const {
  return !output_ || (blocks_.empty() && period_.isEmpty() &&
                      output_->bytesFree() >= output_->bufferSize());
}
void AudioOutput::Pause() {
  if (!output_ || paused_) return;
  paused_us_ = PositionUs();
  output_->suspend();
  paused_ = true;
}
void AudioOutput::Resume() {
  if (!output_ || !paused_) return;
  output_->resume();
  paused_ = false;
}
void AudioOutput::SetVolume(int percent, bool muted) {
  volume_ = std::clamp(percent, 0, 100);
  muted_ = muted;
  if (output_) {
    const qreal linear =
        QAudio::convertVolume(volume_ / 100.0, QAudio::LogarithmicVolumeScale,
                              QAudio::LinearVolumeScale);
    output_->setVolume(muted ? 0.0 : linear);
  }
}
QString AudioOutput::Description() const {
  return QString::fromUtf8("%1 · %2 Hz / stereo / S16")
      .arg(device_.deviceName())
      .arg(format_.sampleRate());
}
