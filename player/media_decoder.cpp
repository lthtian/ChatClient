#include "media_decoder.h"

#include <QFileInfo>
#include <QThread>
#include <algorithm>
#include <stdexcept>

namespace {
constexpr AVRational kUs{1, AV_TIME_BASE};
void Check(int result, const char* api) {
  if (result >= 0) return;
  char message[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(result, message, sizeof(message));
  throw std::runtime_error(std::string(api) + ": " + message);
}
}  // namespace

MediaDecoder::MediaDecoder()
    : input_(nullptr, [](AVFormatContext* p) { avformat_close_input(&p); }),
      video_(nullptr, [](AVCodecContext* p) { avcodec_free_context(&p); }),
      audio_(nullptr, [](AVCodecContext* p) { avcodec_free_context(&p); }),
      packet_(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); }),
      frame_(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); }),
      rgb_(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); }),
      scaler_(sws_alloc_context(), [](SwsContext* p) { sws_free_context(&p); }),
      resampler_(nullptr, [](SwrContext* p) { swr_free(&p); }) {}

MediaDecoder::~MediaDecoder() { av_channel_layout_uninit(&source_layout_); }

void MediaDecoder::OpenCodec(const AVStream* stream, AVCodecContext** output) {
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) throw std::runtime_error("找不到对应解码器");
  *output = avcodec_alloc_context3(codec);
  if (!*output) throw std::bad_alloc();
  // 即使初始化中途失败，调用者也会把这个指针交给智能指针释放。
  Check(avcodec_parameters_to_context(*output, stream->codecpar),
        "avcodec_parameters_to_context");
  (*output)->pkt_timebase = stream->time_base;
  (*output)->thread_count = 1;
  Check(avcodec_open2(*output, codec, nullptr), "avcodec_open2");
}

void MediaDecoder::RequestSession(quint64 session) {
  // 原子代次由 GUI 直接更新，不访问 FFmpeg 对象；网络阻塞时排队槽无法及时执行。
  requested_session_.store(session, std::memory_order_relaxed);
}

bool MediaDecoder::Canceled() const {
  return requested_session_.load(std::memory_order_relaxed) != session_ ||
         QThread::currentThread()->isInterruptionRequested();
}

int MediaDecoder::Interrupted(void* opaque) {
  auto* self = static_cast<MediaDecoder*>(opaque);
  return self->Canceled() ||
         (self->online_ && std::chrono::steady_clock::now() >= self->io_deadline_);
}

void MediaDecoder::BeginIo() {
  io_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
}

void MediaDecoder::OpenInput(const MediaSource& source) {
  online_ = source.IsOnline();
  if (!online_ && !QFileInfo(source.location).isFile())
    throw std::runtime_error("请选择存在的本地视频文件");
  AVFormatContext* raw = avformat_alloc_context();
  if (!raw) throw std::bad_alloc();
  raw->interrupt_callback = {Interrupted, this};
  AVDictionary* options = nullptr;
  if (online_) {
    // HTTP 的读和 seek 由 FFmpeg 实现；不用 cache: 协议，不生成临时视频文件。
    av_dict_set(&options, "headers", source.headers.constData(), 0);
    av_dict_set(&options, "protocol_whitelist", "http,https,tcp,tls", 0);
    av_dict_set(&options, "max_redirects", "0", 0);
    av_dict_set(&options, "rw_timeout", "10000000", 0);
    // 每次最多请求 256 KiB，使暂停、定位时仍未消费的网络数据保持有限。
    av_dict_set(&options, "request_size", "262144", 0);
    av_dict_set(&options, "short_seek_size", "262144", 0);
    av_dict_set(&options, "multiple_requests", "0", 0);
    if (source.kind == MediaSource::Kind::Hls) {
      // HLS 解封装器把 headers 继续传给子列表、初始化文件和分片。
      av_dict_set(&options, "http_persistent", "0", 0);
      av_dict_set(&options, "http_multiple", "0", 0);
    }
    av_dict_set(&options, "probesize", "1048576", 0);
    av_dict_set(&options, "analyzeduration", "2000000", 0);
  }
  BeginIo();
  const int result = avformat_open_input(&raw, source.location.toUtf8().constData(),
      online_ ? av_find_input_format(source.kind == MediaSource::Kind::Hls ? "hls" : "mov") : nullptr, &options);
  av_dict_free(&options);
  input_.reset(raw);
  Check(result, "avformat_open_input");
  // HLS 根据时间索引选分片，不通过 m3u8 的字节定位能力判断能否 seek。
  if (source.kind == MediaSource::Kind::Http && (!input_->pb || !(input_->pb->seekable & AVIO_SEEKABLE_NORMAL)))
    throw std::runtime_error("服务端未提供可定位的 HTTP 视频输入");
}

void MediaDecoder::Open(MediaSource source, quint64 session) {
  if (session != requested_session_.load(std::memory_order_relaxed)) return;
  session_ = session;
  video_.reset();
  audio_.reset();
  input_.reset();
  video_stream_ = audio_stream_ = nullptr;
  decode_audio_ = true;
  try {
    if (!packet_ || !frame_ || !rgb_ || !scaler_) throw std::bad_alloc();
    ResetDecodeState(0);
    OpenInput(source);
    Check(avformat_find_stream_info(input_.get(), nullptr),
          "avformat_find_stream_info");
    for (unsigned i = 0; i < input_->nb_streams; ++i) {
      const auto* stream = input_->streams[i];
      if (!video_stream_ &&
          stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
          !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
        video_stream_ = stream;
      if (!audio_stream_ && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        audio_stream_ = stream;
    }
    if (!video_stream_) throw std::runtime_error("文件中没有视频流");
    AVCodecContext* context = nullptr;
    try {
      OpenCodec(video_stream_, &context);
    } catch (...) {
      avcodec_free_context(&context);
      throw;
    }
    video_.reset(context);
    if (audio_stream_) {
      context = nullptr;
      try {
        OpenCodec(audio_stream_, &context);
      } catch (...) {
        avcodec_free_context(&context);
        throw;
      }
      audio_.reset(context);
    }
    // 两条轨道必须减去同一个起点，否则原有的声画起始偏移会被抹掉。
    origin_us_ = input_->start_time == AV_NOPTS_VALUE ? 0 : input_->start_time;
    seek_origin_us_ = origin_us_;
    if (source.kind == MediaSource::Kind::Hls) {
      // 单档 HLS 的分片索引以首包 DTS 为起点，不能直接套用显示时间起点。
      // 保留借读的包，避免正常起播时丢掉首个编码包。
      BeginIo();
      Check(av_read_frame(input_.get(), packet_.get()), "av_read_frame");
      packet_pending_ = true;
      if (packet_->dts == AV_NOPTS_VALUE)
        throw std::runtime_error("HLS 首包缺少解码时间戳，无法确定定位起点");
      seek_origin_us_ = av_rescale_q(packet_->dts,
          input_->streams[packet_->stream_index]->time_base, kUs);
    }
    const AVRational rate = av_guess_frame_rate(
        input_.get(), input_->streams[video_stream_->index], nullptr);
    frame_duration_us_ = rate.num > 0 && rate.den > 0
                             ? av_rescale_q(1, av_inv_q(rate), kUs)
                             : 40000;
    qint64 duration = input_->duration == AV_NOPTS_VALUE ? 0 : input_->duration;
    qint64 audio_end = -1;
    if (audio_stream_ && audio_stream_->duration != AV_NOPTS_VALUE) {
      const qint64 start = audio_stream_->start_time == AV_NOPTS_VALUE
                               ? 0
                               : av_rescale_q(audio_stream_->start_time,
                                              audio_stream_->time_base, kUs) -
                                     origin_us_;
      audio_end = start + av_rescale_q(audio_stream_->duration,
                                       audio_stream_->time_base, kUs);
    }
    scaler_->flags = SWS_BILINEAR;
    scaler_->threads = 1;
    emit Opened(session_, std::max<qint64>(0, duration), audio_ != nullptr,
                audio_end);
  } catch (const std::exception& e) {
    if (!Canceled()) emit Failed(session_, QString::fromUtf8(e.what()));
  }
}

void MediaDecoder::SetAudioEnabled(bool enabled) { decode_audio_ = enabled; }

void MediaDecoder::SetOutputRate(int rate) { output_rate_ = rate; }

void MediaDecoder::ResetDecodeState(qint64 target_us) {
  av_packet_unref(packet_.get());
  packet_pending_ = false;
  av_frame_unref(frame_.get());
  av_frame_unref(rgb_.get());
  resampler_.reset();
  av_channel_layout_uninit(&source_layout_);
  target_us_ = target_us;
  next_video_us_ = target_us;
  audio_samples_ = 0;
  audio_started_ = false;
  eof_ = false;
}

void MediaDecoder::Seek(qint64 position_us, quint64 session) {
  if (session != requested_session_.load(std::memory_order_relaxed)) return;
  session_ = session;
  BeginIo();
  try {
    if (!input_ || !video_) throw std::runtime_error("文件尚未打开");
    const int64_t timestamp =
        av_rescale_q(position_us + seek_origin_us_, kUs, video_stream_->time_base);
    // 向前找关键帧，再顺序解码到目标位置；直接跳到任意编码包会缺少参考图像。
    Check(av_seek_frame(input_.get(), video_stream_->index, timestamp,
                        AVSEEK_FLAG_BACKWARD),
          "av_seek_frame");
    avcodec_flush_buffers(video_.get());
    if (audio_) avcodec_flush_buffers(audio_.get());
    ResetDecodeState(position_us);
    emit Seeked(session_);
  } catch (const std::exception& e) {
    if (!Canceled()) emit Failed(session_, QString::fromUtf8(e.what()));
  }
}

void MediaDecoder::Send(AVCodecContext* codec, const AVPacket* packet,
                        bool video, MediaBatch& batch) {
  int result = avcodec_send_packet(codec, packet);
  if (result == AVERROR(EAGAIN)) {
    Receive(codec, video, batch);
    result = avcodec_send_packet(codec, packet);
  }
  Check(result, "avcodec_send_packet");
  const int received = Receive(codec, video, batch);
  if (!packet && received != AVERROR_EOF)
    throw std::runtime_error("解码器排空时仍请求输入");
}

int MediaDecoder::Receive(AVCodecContext* codec, bool video,
                          MediaBatch& batch) {
  while (!Canceled()) {
    const int result = avcodec_receive_frame(codec, frame_.get());
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return result;
    Check(result, "avcodec_receive_frame");
    if (video)
      ConvertVideo(batch);
    else {
      PrepareAudio();
      ConvertAudio(frame_.get(), batch);
    }
    av_frame_unref(frame_.get());
  }
  return AVERROR_EXIT;
}

void MediaDecoder::Read(quint64 session) {
  if (session != session_ || !video_ || Canceled()) return;
  try {
    BeginIo();
    MediaBatch batch;
    if (!eof_) {
      const int result = packet_pending_ ? 0 : av_read_frame(input_.get(), packet_.get());
      packet_pending_ = false;
      if (result == AVERROR_EOF) {
        // 读完容器不等于播放完：先交出两条轨道及重采样器的尾部数据。
        Send(video_.get(), nullptr, true, batch);
        if (audio_ && decode_audio_) Send(audio_.get(), nullptr, false, batch);
        if (resampler_)
          while (ConvertAudio(nullptr, batch) > 0) {
          }
        eof_ = true;
      } else {
        Check(result, "av_read_frame");
        if (packet_->stream_index == video_stream_->index)
          Send(video_.get(), packet_.get(), true, batch);
        else if (decode_audio_ && audio_stream_ && packet_->stream_index == audio_stream_->index)
          Send(audio_.get(), packet_.get(), false, batch);
      }
      av_packet_unref(packet_.get());
    }
    batch.eof = eof_;
    emit BatchReady(session_, std::move(batch));
  } catch (const std::exception& e) {
    if (!Canceled()) emit Failed(session_, QString::fromUtf8(e.what()));
  }
}

void MediaDecoder::ConvertVideo(MediaBatch& batch) {
  const qint64 pts = frame_->best_effort_timestamp == AV_NOPTS_VALUE
                         ? next_video_us_
                         : av_rescale_q(frame_->best_effort_timestamp,
                                        video_stream_->time_base, kUs) -
                               origin_us_;
  const qint64 duration = std::max<qint64>(
      1, frame_->duration > 0
             ? av_rescale_q(frame_->duration, video_stream_->time_base, kUs)
             : frame_duration_us_);
  next_video_us_ = pts + duration;
  if (next_video_us_ <= target_us_) return;
  av_frame_unref(rgb_.get());
  rgb_->format = AV_PIX_FMT_RGB24;
  rgb_->width = frame_->width;
  rgb_->height = frame_->height;
  rgb_->color_range = AVCOL_RANGE_JPEG;
  rgb_->colorspace = AVCOL_SPC_RGB;
  rgb_->color_primaries = frame_->color_primaries;
  rgb_->color_trc = frame_->color_trc;
  Check(sws_scale_frame(scaler_.get(), rgb_.get(), frame_.get()),
        "sws_scale_frame");
  QImage image(rgb_->data[0], rgb_->width, rgb_->height, rgb_->linesize[0],
               QImage::Format_RGB888);
  // 独立拥有像素，工作线程复用 AVFrame 不会破坏 GUI 尚未显示的画面。
  QImage owned = image.copy();
  if (owned.isNull()) throw std::bad_alloc();
  batch.video.push_back({std::move(owned), std::max(target_us_, pts),
                         next_video_us_ - std::max(target_us_, pts)});
}

void MediaDecoder::PrepareAudio() {
  const auto format = static_cast<AVSampleFormat>(frame_->format);
  if (frame_->sample_rate <= 0 || frame_->nb_samples <= 0 ||
      !av_channel_layout_check(&frame_->ch_layout) ||
      av_get_bytes_per_sample(format) <= 0)
    throw std::runtime_error("音频帧参数无效");
  if (resampler_) {
    if (source_rate_ != frame_->sample_rate || source_format_ != format ||
        av_channel_layout_compare(&source_layout_, &frame_->ch_layout) != 0)
      throw std::runtime_error("本例不支持同一音轨中途改变采样格式");
    return;
  }
  source_rate_ = frame_->sample_rate;
  source_format_ = format;
  Check(av_channel_layout_copy(&source_layout_, &frame_->ch_layout),
        "av_channel_layout_copy");
  const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  SwrContext* raw = nullptr;
  const int result = swr_alloc_set_opts2(
      &raw, &stereo, AV_SAMPLE_FMT_S16, output_rate_, &source_layout_,
      source_format_, source_rate_, 0, nullptr);
  resampler_.reset(raw);
  Check(result, "swr_alloc_set_opts2");
  Check(swr_init(raw), "swr_init");
}

int MediaDecoder::ConvertAudio(const AVFrame* source, MediaBatch& batch) {
  if (source && !audio_started_) {
    audio_origin_us_ = source->best_effort_timestamp == AV_NOPTS_VALUE
                           ? target_us_
                           : av_rescale_q(source->best_effort_timestamp,
                                          audio_stream_->time_base, kUs) -
                                 origin_us_;
    audio_started_ = true;
  }
  const int bound =
      swr_get_out_samples(resampler_.get(), source ? source->nb_samples : 0);
  Check(bound, "swr_get_out_samples");
  if (bound > 2 * output_rate_)
    throw std::runtime_error("单个音频块超过本例缓冲上限");
  QByteArray pcm(std::max(1, bound) * 4, '\0');
  auto* data = reinterpret_cast<uint8_t*>(pcm.data());
  const int produced = swr_convert(resampler_.get(), &data, std::max(1, bound),
                                   source ? source->extended_data : nullptr,
                                   source ? source->nb_samples : 0);
  Check(produced, "swr_convert");
  const qint64 first_sample = audio_samples_;
  audio_samples_ += produced;
  // 按累计采样数换算时间，不反复相加被舍入的单块时长。seek 裁剪精确到采样。
  const qint64 target_sample = av_rescale_rnd(
      target_us_ - audio_origin_us_, output_rate_, AV_TIME_BASE, AV_ROUND_UP);
  const int skip = static_cast<int>(
      std::clamp<qint64>(target_sample - first_sample, 0, produced));
  if (produced > skip) {
    pcm.resize(produced * 4);
    if (skip) pcm.remove(0, skip * 4);
    const qint64 pts =
        audio_origin_us_ +
        av_rescale_q(first_sample + skip, AVRational{1, output_rate_}, kUs);
    batch.audio.push_back({std::move(pcm), pts});
  }
  return produced;
}
