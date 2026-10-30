#include "track_decoder.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>

namespace {
constexpr AVRational kUs{1, AV_TIME_BASE};
void Check(int result, const char* api) {
  if (result >= 0) return;
  char message[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(result, message, sizeof(message));
  throw std::runtime_error(std::string(api) + ": " + message);
}
bool Canceled(const std::function<bool()>& canceled) {
  return canceled && canceled();
}
}

TrackDecoder::TrackDecoder()
    : context_(nullptr, [](AVCodecContext* p) { avcodec_free_context(&p); }),
      packet_(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); }),
      frame_(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); }),
      seek_frame_(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); }),
      rgb_(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); }),
      scaler_(sws_alloc_context(), [](SwsContext* p) { sws_free_context(&p); }),
      resampler_(nullptr, [](SwrContext* p) { swr_free(&p); }) {
  if (!packet_ || !frame_ || !seek_frame_ || !rgb_ || !scaler_) throw std::bad_alloc();
  scaler_->flags = SWS_BILINEAR;
  scaler_->threads = 1;
}
TrackDecoder::~TrackDecoder() { av_channel_layout_uninit(&source_layout_); }

void TrackDecoder::Open(const AVCodecParameters& parameters, AVRational time_base,
                        qint64 frame_duration_us, AVRational stream_sar) {
  Close();
  if (time_base.num <= 0 || time_base.den <= 0)
    throw std::runtime_error("轨道时间基无效");
  if (parameters.codec_type != AVMEDIA_TYPE_VIDEO && parameters.codec_type != AVMEDIA_TYPE_AUDIO)
    throw std::runtime_error("只支持音频或视频轨道");
  const AVCodec* codec = avcodec_find_decoder(parameters.codec_id);
  if (!codec) throw std::runtime_error("找不到对应解码器");
  context_.reset(avcodec_alloc_context3(codec));
  if (!context_) throw std::bad_alloc();
  // 编码参数复制到本轨道，调用者随后释放输入也不会留下 AVStream 借用。
  Check(avcodec_parameters_to_context(context_.get(), &parameters), "avcodec_parameters_to_context");
  context_->pkt_timebase = time_base;
  context_->thread_count = 1;
  Check(avcodec_open2(context_.get(), codec, nullptr), "avcodec_open2");
  stream_sar_ = stream_sar;
  av_reduce(&stream_sar_.num, &stream_sar_.den, stream_sar.num, stream_sar.den, INT_MAX);
  display_matrix_.clear();
  const auto* matrix = av_packet_side_data_get(parameters.coded_side_data,
      parameters.nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
  if (matrix) {
    if (matrix->size < 9 * sizeof(int32_t)) throw std::runtime_error("显示矩阵长度无效");
    display_matrix_ = QByteArray(reinterpret_cast<const char*>(matrix->data), 9 * sizeof(int32_t));
  }
  time_base_ = time_base;
  frame_duration_us_ = std::max<qint64>(1, frame_duration_us);
}
void TrackDecoder::Close() { Reset(); context_.reset(); }
bool TrackDecoder::IsOpen() const { return context_ && avcodec_is_open(context_.get()); }
void TrackDecoder::SetOutputRate(int rate) {
  if (rate <= 0) throw std::runtime_error("输出采样率无效");
  if (resampler_ && rate != output_rate_)
    throw std::runtime_error("音频转换期间不能更换输出采样率");
  output_rate_ = rate;
}
void TrackDecoder::SetVideoLimit(QSize limit) { video_limit_ = limit; }
void TrackDecoder::Reset(qint64 target_us) {
  if (IsOpen()) avcodec_flush_buffers(context_.get());
  av_packet_unref(packet_.get());
  av_frame_unref(frame_.get());
  av_frame_unref(seek_frame_.get());
  delivered_video_ = false;
  av_frame_unref(rgb_.get());
  resampler_.reset();
  av_channel_layout_uninit(&source_layout_);
  source_rate_ = 0;
  source_format_ = AV_SAMPLE_FMT_NONE;
  target_us_ = next_video_us_ = target_us;
  audio_samples_ = 0;
  audio_started_ = false;
}

void TrackDecoder::Decode(const AVPacket* packet, qint64& origin_us, MediaBatch& batch,
                          const std::function<bool()>& canceled) {
  if (!IsOpen()) throw std::runtime_error("轨道解码器尚未打开");
  if (Canceled(canceled)) return;
  int result = avcodec_send_packet(context_.get(), packet);
  if (result == AVERROR(EAGAIN)) {
    // 此包尚未接受：先取完已有输出，再提交同一个包。
    Receive(origin_us, batch, canceled);
    if (Canceled(canceled)) return;
    result = avcodec_send_packet(context_.get(), packet);
  }
  Check(result, "avcodec_send_packet");
  const int received = Receive(origin_us, batch, canceled);
  if (!packet && !Canceled(canceled) && received != AVERROR_EOF)
    throw std::runtime_error("解码器排空时仍请求输入");
}
void TrackDecoder::DecodeUnit(const QByteArray& bytes, qint64 pts, qint64 dts,
                              qint64& origin_us, MediaBatch& batch) {
  av_packet_unref(packet_.get());
  Check(av_new_packet(packet_.get(), bytes.size()), "av_new_packet");
  std::copy(bytes.begin(), bytes.end(), packet_->data);
  packet_->pts = pts;
  packet_->dts = dts;
  Decode(packet_.get(), origin_us, batch);
  av_packet_unref(packet_.get());
}
void TrackDecoder::Drain(qint64& origin_us, MediaBatch& batch,
                         const std::function<bool()>& canceled) {
  Decode(nullptr, origin_us, batch, canceled);
  if (!Canceled(canceled) && !delivered_video_ && seek_frame_->buf[0]) {
    av_frame_unref(frame_.get());
    av_frame_move_ref(frame_.get(), seek_frame_.get());
    ConvertVideo(origin_us, batch, true);
    av_frame_unref(frame_.get());
  }
  // 读完容器不等于播放完：PCM 转换器也可能保留尾部采样。
  if (resampler_)
    while (!Canceled(canceled) && ConvertAudio(nullptr, origin_us, batch) > 0) {}
}
int TrackDecoder::Receive(qint64& origin_us, MediaBatch& batch,
                          const std::function<bool()>& canceled) {
  while (!Canceled(canceled)) {
    const int result = avcodec_receive_frame(context_.get(), frame_.get());
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return result;
    Check(result, "avcodec_receive_frame");
    // 直播首份输出确定两轨共用的原点，后续帧不再分别重置音视频时间。
    if (origin_us == AV_NOPTS_VALUE)
      origin_us = frame_->best_effort_timestamp == AV_NOPTS_VALUE ? 0 :
          av_rescale_q(frame_->best_effort_timestamp, time_base_, kUs);
    if (context_->codec_type == AVMEDIA_TYPE_VIDEO)
      ConvertVideo(origin_us, batch);
    else {
      PrepareAudio();
      ConvertAudio(frame_.get(), origin_us, batch);
    }
    av_frame_unref(frame_.get());
  }
  return AVERROR_EXIT;
}
void TrackDecoder::ConvertVideo(qint64 origin_us, MediaBatch& batch, bool tail_preview) {
  const qint64 pts = tail_preview ? seek_frame_pts_us_ :
      frame_->best_effort_timestamp == AV_NOPTS_VALUE ? next_video_us_ :
      av_rescale_q(frame_->best_effort_timestamp, time_base_, kUs) - origin_us;
  const qint64 duration = std::max<qint64>(1, frame_->duration > 0 ?
      av_rescale_q(frame_->duration, time_base_, kUs) : frame_duration_us_);
  next_video_us_ = pts + duration;
  if (!tail_preview && next_video_us_ <= target_us_) {
    av_frame_unref(seek_frame_.get());
    // 容器时长与逐帧时间舍入可能相差一个流时基单位；只保留这一精度内的尾部候选。
    if (!delivered_video_ && target_us_ > 0 &&
        target_us_ - next_video_us_ < std::max<qint64>(1, av_rescale_q(1, time_base_, kUs))) {
      Check(av_frame_ref(seek_frame_.get(), frame_.get()), "av_frame_ref");
      seek_frame_pts_us_ = pts;
    }
    return;
  }
  if (frame_->width <= 0 || frame_->height <= 0 ||
      (!video_limit_.isEmpty() && (frame_->width > video_limit_.width() || frame_->height > video_limit_.height())))
    throw std::runtime_error("视频画面尺寸无效或超出输入限制");
  av_frame_unref(rgb_.get());
  rgb_->format = AV_PIX_FMT_RGB24;
  rgb_->width = frame_->width;
  rgb_->height = frame_->height;
  rgb_->color_range = AVCOL_RANGE_JPEG;
  rgb_->colorspace = AVCOL_SPC_RGB;
  rgb_->color_primaries = frame_->color_primaries;
  rgb_->color_trc = frame_->color_trc;
  Check(sws_scale_frame(scaler_.get(), rgb_.get(), frame_.get()), "sws_scale_frame");
  QImage image(rgb_->data[0], rgb_->width, rgb_->height, rgb_->linesize[0], QImage::Format_RGB888);
  // 独立拥有像素，工作线程复用 AVFrame 不会破坏 GUI 尚未显示的画面。
  QImage owned = image.copy();
  if (owned.isNull()) throw std::bad_alloc();
  VideoGeometry geometry;
  AVRational sar = stream_sar_.num > 0 && stream_sar_.den > 0 ? stream_sar_ : frame_->sample_aspect_ratio;
  av_reduce(&sar.num, &sar.den, sar.num, sar.den, INT_MAX);
  if (sar.num > 0 && sar.den > 0) { geometry.sar_num = sar.num; geometry.sar_den = sar.den; }
  const auto* frame_matrix = av_frame_get_side_data(frame_.get(), AV_FRAME_DATA_DISPLAYMATRIX);
  if (frame_matrix || !display_matrix_.isEmpty()) {
    if (frame_matrix && frame_matrix->size < 9 * sizeof(int32_t))
      throw std::runtime_error("显示矩阵长度无效");
    int32_t values[9];
    std::memcpy(values, frame_matrix ? frame_matrix->data :
        reinterpret_cast<const uint8_t*>(display_matrix_.constData()), sizeof(values));
    geometry.SetDisplayMatrix(values);
  }
  const qint64 display_pts = tail_preview ? pts : std::max(target_us_, pts);
  batch.video.push_back({std::move(owned), display_pts, next_video_us_ - display_pts, geometry});
  delivered_video_ = true;
  av_frame_unref(seek_frame_.get());
}
void TrackDecoder::PrepareAudio() {
  const auto format = static_cast<AVSampleFormat>(frame_->format);
  if (frame_->sample_rate <= 0 || frame_->nb_samples <= 0 ||
      !av_channel_layout_check(&frame_->ch_layout) || av_get_bytes_per_sample(format) <= 0)
    throw std::runtime_error("音频帧参数无效");
  if (resampler_) {
    if (source_rate_ != frame_->sample_rate || source_format_ != format ||
        av_channel_layout_compare(&source_layout_, &frame_->ch_layout) != 0)
      throw std::runtime_error("本例不支持同一音轨中途改变采样格式");
    return;
  }
  source_rate_ = frame_->sample_rate;
  source_format_ = format;
  Check(av_channel_layout_copy(&source_layout_, &frame_->ch_layout), "av_channel_layout_copy");
  const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  SwrContext* raw = nullptr;
  const int result = swr_alloc_set_opts2(&raw, &stereo, AV_SAMPLE_FMT_S16, output_rate_,
      &source_layout_, source_format_, source_rate_, 0, nullptr);
  resampler_.reset(raw);
  Check(result, "swr_alloc_set_opts2");
  Check(swr_init(raw), "swr_init");
}
int TrackDecoder::ConvertAudio(const AVFrame* source, qint64 origin_us, MediaBatch& batch) {
  if (source && audio_started_ && source->best_effort_timestamp != AV_NOPTS_VALUE) {
    const qint64 actual = av_rescale_q(source->best_effort_timestamp, time_base_, kUs) - origin_us;
    const qint64 expected = audio_origin_us_ + av_rescale_q(
        audio_samples_ + swr_get_delay(resampler_.get(), output_rate_), AVRational{1, output_rate_}, kUs);
    if (std::abs(actual - expected) > 100000) {
      if (audio_discontinuity) audio_discontinuity(actual, expected);
      // 分片丢失后 PCM 不能按采样数拼接掉时间空隙，否则音频时钟会永久落后视频。
      while (ConvertAudio(nullptr, origin_us, batch) > 0) {}
      swr_close(resampler_.get());
      Check(swr_init(resampler_.get()), "swr_init");
      audio_started_ = false;
    }
  }
  if (source && !audio_started_) {
    audio_origin_us_ = source->best_effort_timestamp == AV_NOPTS_VALUE ? target_us_ :
        av_rescale_q(source->best_effort_timestamp, time_base_, kUs) - origin_us;
    audio_started_ = true;
    audio_samples_ = 0;
  }
  const int bound = swr_get_out_samples(resampler_.get(), source ? source->nb_samples : 0);
  Check(bound, "swr_get_out_samples");
  if (bound > 2 * output_rate_) throw std::runtime_error("单个音频块超过本例缓冲上限");
  QByteArray pcm(std::max(1, bound) * 4, '\0');
  auto* data = reinterpret_cast<uint8_t*>(pcm.data());
  const int produced = swr_convert(resampler_.get(), &data, std::max(1, bound),
      source ? source->extended_data : nullptr, source ? source->nb_samples : 0);
  Check(produced, "swr_convert");
  const qint64 first_sample = audio_samples_;
  audio_samples_ += produced;
  // 按累计采样数换算时间，不反复相加被舍入的单块时长。seek 裁剪精确到采样。
  const qint64 target_sample = av_rescale_rnd(
      target_us_ - audio_origin_us_, output_rate_, AV_TIME_BASE, AV_ROUND_UP);
  const int skip = static_cast<int>(std::clamp<qint64>(target_sample - first_sample, 0, produced));
  if (produced > skip) {
    pcm.resize(produced * 4);
    if (skip) pcm.remove(0, skip * 4);
    const qint64 pts = audio_origin_us_ +
        av_rescale_q(first_sample + skip, AVRational{1, output_rate_}, kUs);
    batch.audio.push_back({std::move(pcm), pts});
  }
  return produced;
}
