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
      packet_(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); }) {}

MediaDecoder::~MediaDecoder() = default;

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
  live_ = source.IsLive();
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
    if (source.IsHls()) {
      // HLS 解封装器把 headers 继续传给子列表、初始化文件和分片。
      av_dict_set(&options, "http_persistent", "0", 0);
      av_dict_set(&options, "http_multiple", "0", 0);
      if (live_) av_dict_set(&options, "live_start_index", "-1", 0);
    }
    av_dict_set(&options, "probesize", "1048576", 0);
    av_dict_set(&options, "analyzeduration", "2000000", 0);
  }
  BeginIo();
  const int result = avformat_open_input(&raw, source.location.toUtf8().constData(),
      online_ ? av_find_input_format(source.IsHls() ? "hls" :
          source.kind == MediaSource::Kind::LiveFlv ? "flv" : "mov") : nullptr, &options);
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
  video_.Close();
  audio_.Close();
  input_.reset();
  video_stream_ = audio_stream_ = nullptr;
  decode_audio_ = true;
  try {
    if (!packet_) throw std::bad_alloc();
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
    video_.Open(*video_stream_->codecpar, video_stream_->time_base, frame_duration_us_);
    if (audio_stream_) {
      audio_.Open(*audio_stream_->codecpar, audio_stream_->time_base);
      audio_.SetOutputRate(output_rate_);
    }
    qint64 duration = live_ || input_->duration == AV_NOPTS_VALUE ? 0 : input_->duration;
    qint64 audio_end = -1;
    if (!live_ && audio_stream_ && audio_stream_->duration != AV_NOPTS_VALUE) {
      const qint64 start = audio_stream_->start_time == AV_NOPTS_VALUE
                               ? 0
                               : av_rescale_q(audio_stream_->start_time,
                                              audio_stream_->time_base, kUs) -
                                     origin_us_;
      audio_end = start + av_rescale_q(audio_stream_->duration,
                                       audio_stream_->time_base, kUs);
    }
    // 直播可能从非零时刻的 GOP 加入。第一份解码输出建立两轨共同起点，不能等待过去的时间。
    if (live_) origin_us_ = AV_NOPTS_VALUE;
    emit Opened(session_, std::max<qint64>(0, duration), audio_.IsOpen(),
                audio_end);
  } catch (const std::exception& e) {
    if (!Canceled()) emit Failed(session_, QString::fromUtf8(e.what()));
  }
}

void MediaDecoder::Close(quint64 session) {
  if (session != requested_session_.load(std::memory_order_relaxed)) return;
  session_ = session;
  av_packet_unref(packet_.get());
  video_.Close();
  audio_.Close();
  input_.reset();
  video_stream_ = audio_stream_ = nullptr;
}

void MediaDecoder::SetAudioEnabled(bool enabled) { decode_audio_ = enabled; }

void MediaDecoder::SetOutputRate(int rate) { output_rate_ = rate; audio_.SetOutputRate(rate); }

void MediaDecoder::ResetDecodeState(qint64 target_us) {
  av_packet_unref(packet_.get());
  packet_pending_ = false;
  video_.Reset(target_us);
  audio_.Reset(target_us);
  eof_ = false;
}

void MediaDecoder::Seek(qint64 position_us, quint64 session) {
  if (session != requested_session_.load(std::memory_order_relaxed)) return;
  session_ = session;
  BeginIo();
  try {
    if (!input_ || !video_.IsOpen()) throw std::runtime_error("文件尚未打开");
    if (live_) throw std::runtime_error("直播不支持点播定位");
    const int64_t timestamp =
        av_rescale_q(position_us + seek_origin_us_, kUs, video_stream_->time_base);
    // 向前找关键帧，再顺序解码到目标位置；直接跳到任意编码包会缺少参考图像。
    Check(av_seek_frame(input_.get(), video_stream_->index, timestamp,
                        AVSEEK_FLAG_BACKWARD),
          "av_seek_frame");
    ResetDecodeState(position_us);
    emit Seeked(session_);
  } catch (const std::exception& e) {
    if (!Canceled()) emit Failed(session_, QString::fromUtf8(e.what()));
  }
}

void MediaDecoder::Read(quint64 session) {
  if (session != session_ || !video_.IsOpen() || Canceled()) return;
  try {
    BeginIo();
    MediaBatch batch;
    const auto canceled = [this] { return Canceled(); };
    if (!eof_) {
      const int result = packet_pending_ ? 0 : av_read_frame(input_.get(), packet_.get());
      packet_pending_ = false;
      if (result == AVERROR_EOF) {
        // 读完容器不等于播放完：先交出两条轨道及重采样器的尾部数据。
        video_.Drain(origin_us_, batch, canceled);
        if (audio_.IsOpen() && decode_audio_) audio_.Drain(origin_us_, batch, canceled);
        eof_ = true;
      } else {
        Check(result, "av_read_frame");
        if (packet_->stream_index == video_stream_->index)
          video_.Decode(packet_.get(), origin_us_, batch, canceled);
        else if (decode_audio_ && audio_stream_ && packet_->stream_index == audio_stream_->index)
          audio_.Decode(packet_.get(), origin_us_, batch, canceled);
      }
      av_packet_unref(packet_.get());
    }
    batch.eof = eof_;
    if (!Canceled()) emit BatchReady(session_, std::move(batch));
  } catch (const std::exception& e) {
    if (!Canceled()) emit Failed(session_, QString::fromUtf8(e.what()));
  }
}
