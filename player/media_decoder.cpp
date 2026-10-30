#include "media_decoder.h"

#include <QFileInfo>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace {
constexpr AVRational kUs{1, AV_TIME_BASE};
void Check(int result, const char* api) {
  if (result >= 0) return;
  char message[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(result, message, sizeof(message));
  throw std::runtime_error(std::string(api) + ": " + message);
}
QString Metadata(const AVStream* stream, const char* key) {
  const auto* value = av_dict_get(stream->metadata, key, nullptr, 0);
  return value ? QString::fromUtf8(value->value) : QString();
}
QString TrackError(const MediaTrack& track, const QString& phase, const QString& detail) {
  return QStringLiteral("%1轨道 #%2（%3%4）%5：%6")
      .arg(track.type == "audio" ? QStringLiteral("音频") : QStringLiteral("视频"))
      .arg(track.index).arg(track.codec)
      .arg(track.profile.isEmpty() ? QString() : " / " + track.profile).arg(phase, detail);
}
}  // namespace

MediaDecoder::MediaDecoder()
    : input_(nullptr, [](AVFormatContext* p) { avformat_close_input(&p); }),
      packet_(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); }) {
  audio_.audio_discontinuity = [this](qint64 actual, qint64 expected) {
    emit AudioDiscontinuity(session_, actual, expected);
  };
}

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
         std::chrono::steady_clock::now() >= self->io_deadline_;
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
  if (!source.IsHls() && !source.IsLive()) {
    av_dict_set(&options, "format_whitelist", "mov,matroska,webm,avi,mpegts,flv,ogg", 0);
    av_dict_set(&options, "enable_drefs", "0", 0);
    av_dict_set(&options, "max_streams", "64", 0);
  }
  if (!online_) av_dict_set(&options, "protocol_whitelist", "file", 0);
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
  const AVInputFormat* format = source.IsHls() ? av_find_input_format("hls") :
      source.kind == MediaSource::Kind::LiveFlv ? av_find_input_format("flv") : nullptr;
  const int result = avformat_open_input(&raw, source.location.toUtf8().constData(), format, &options);
  av_dict_free(&options);
  input_.reset(raw);
  Check(result, "avformat_open_input");
  if (!source.IsHls()) {
    // 单文件与 FLV 输入不需要打开额外资源；HLS 子请求使用独立的业务授权边界。
    input_->io_open = [](AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
      return AVERROR(EACCES);
    };
  }
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
    SelectTracks(source);
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
    if (!live_) emit TimelineReady(session_, origin_us_, seek_origin_us_);
    emit TracksReady(session_, tracks_);
    emit Opened(session_, std::max<qint64>(0, duration), audio_.IsOpen(),
                audio_end);
  } catch (const std::exception& e) {
    if (!Canceled()) emit Failed(session_, QString::fromUtf8(e.what()));
  }
}

void MediaDecoder::SelectTracks(const MediaSource& source) {
  tracks_ = {};
  for (unsigned i = 0; i < input_->nb_streams; ++i) {
    const auto* stream = input_->streams[i];
    const auto* p = stream->codecpar;
    MediaTrack track;
    track.index = stream->index;
    track.type = QString::fromUtf8(av_get_media_type_string(p->codec_type));
    track.codec = QString::fromUtf8(avcodec_get_name(p->codec_id));
    track.profile = QString::fromUtf8(avcodec_profile_name(p->codec_id, p->profile));
    track.language = Metadata(stream, "language");
    track.title = Metadata(stream, "title");
    if (track.title.isEmpty()) track.title = Metadata(stream, "handler_name");
    track.is_default = stream->disposition & AV_DISPOSITION_DEFAULT;
    track.attached_picture = stream->disposition & AV_DISPOSITION_ATTACHED_PIC;
    track.decoder_available = avcodec_find_decoder(p->codec_id) != nullptr;
    if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
      const auto* pixel = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(p->format));
      const QString fps = stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0 ?
          QStringLiteral("%1/%2").arg(stream->avg_frame_rate.num).arg(stream->avg_frame_rate.den) : QStringLiteral("未知");
      track.format = QStringLiteral("%1×%2 · %3 · %4 bit · 平均 %5 fps")
          .arg(p->width).arg(p->height).arg(pixel ? QString::fromUtf8(pixel->name) : "?")
          .arg(pixel && pixel->nb_components > 0 ? QString::number(pixel->comp[0].depth) : "?").arg(fps);
    } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
      char layout[128]{};
      av_channel_layout_describe(&p->ch_layout, layout, sizeof(layout));
      const char* sample = av_get_sample_fmt_name(static_cast<AVSampleFormat>(p->format));
      track.format = QStringLiteral("%1 Hz · %2 · %3").arg(p->sample_rate).arg(QString::fromUtf8(layout))
          .arg(sample ? QString::fromUtf8(sample) : QStringLiteral("采样格式未知"));
    }
    if ((track.type == "audio" || track.type == "video") && !track.decoder_available) {
      track.failure = "decoder_missing";
      track.error = TrackError(track, QStringLiteral("不可用"), QStringLiteral("本机 FFmpeg 没有对应解码器"));
    }
    if (track.type == "audio" && source.excluded_audio.contains(track.index)) {
      track.failure = "decode_failed";
      track.error = TrackError(track, QStringLiteral("不可用"), QStringLiteral("本次播放中解码失败"));
    }
    tracks_.items.push_back(std::move(track));
  }
  for (const QString& type : {QStringLiteral("video"), QStringLiteral("audio")}) {
    QVector<int> candidates;
    for (int i = 0; i < tracks_.items.size(); ++i)
      if (tracks_.items[i].type == type && !tracks_.items[i].attached_picture) candidates.push_back(i);
    std::stable_sort(candidates.begin(), candidates.end(), [&](int a, int b) {
      const auto rank = [&](int i) {
        const auto& track = tracks_.items[i];
        return type == "audio" && track.index == source.audio_stream ? 2 : track.is_default ? 1 : 0;
      };
      return rank(a) > rank(b);
    });
    QStringList failures;
    for (int index : candidates) {
      auto& track = tracks_.items[index];
      if (!track.error.isEmpty()) { failures.push_back(track.error); continue; }
      if (type == "audio" && source.audio_stream == -1) continue;
      const auto* stream = input_->streams[track.index];
      try {
        if (type == "video") {
          const AVRational rate = av_guess_frame_rate(input_.get(), input_->streams[track.index], nullptr);
          frame_duration_us_ = rate.num > 0 && rate.den > 0 ? av_rescale_q(1, av_inv_q(rate), kUs) : 40000;
          video_.Open(*stream->codecpar, stream->time_base, frame_duration_us_, stream->sample_aspect_ratio);
          video_stream_ = stream;
          tracks_.video = track.index;
        } else {
          audio_.Open(*stream->codecpar, stream->time_base);
          audio_.SetOutputRate(output_rate_);
          audio_stream_ = stream;
          tracks_.audio = track.index;
        }
        break;
      } catch (const std::exception& e) {
        if (Canceled()) throw;
        (type == "video" ? video_ : audio_).Close();
        track.failure = "decoder_init";
        track.error = TrackError(track, QStringLiteral("初始化失败"), QString::fromUtf8(e.what()));
        failures.push_back(track.error);
      }
    }
    if (type == "video" && !video_stream_)
      throw std::runtime_error((candidates.isEmpty() ? QStringLiteral("文件中没有非封面视频轨道") :
          failures.join("；")).toStdString());
    if (type == "audio") {
      if (source.audio_stream == -1) tracks_.audio_notice = QStringLiteral("无声播放（已关闭音轨）");
      else if (candidates.isEmpty()) tracks_.audio_notice = QStringLiteral("无声播放（文件没有音轨）");
      else if (!audio_stream_) tracks_.audio_notice = QStringLiteral("无声播放（%1 条音轨均不可用）：%2")
          .arg(candidates.size()).arg(failures.value(0));
      else if (!failures.isEmpty()) tracks_.audio_notice = QStringLiteral("已选用可用音轨；") + failures.front();
    }
  }
}

void MediaDecoder::DecodeVideo(MediaBatch& batch, bool drain) {
  try {
    const auto canceled = [this] { return Canceled(); };
    if (drain) video_.Drain(origin_us_, batch, canceled);
    else video_.Decode(packet_.get(), origin_us_, batch, canceled);
  } catch (const std::exception& e) {
    throw std::runtime_error(TrackError(tracks_.items[video_stream_->index],
        QStringLiteral("解码或图像转换失败"), QString::fromUtf8(e.what())).toStdString());
  }
}

void MediaDecoder::DecodeAudio(MediaBatch& batch, bool drain) {
  if (!audio_.IsOpen() || !decode_audio_) return;
  try {
    const auto canceled = [this] { return Canceled(); };
    if (drain) audio_.Drain(origin_us_, batch, canceled);
    else audio_.Decode(packet_.get(), origin_us_, batch, canceled);
  } catch (const std::exception& e) {
    if (Canceled()) return;
    const auto& track = tracks_.items[audio_stream_->index];
    audio_.Close();
    batch.audio.clear();
    emit AudioFailed(session_, track.index,
        TrackError(track, QStringLiteral("解码或音频转换失败"), QString::fromUtf8(e.what())));
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
    const bool awaiting_origin = origin_us_ == AV_NOPTS_VALUE;
    if (!eof_) {
      const int result = packet_pending_ ? 0 : av_read_frame(input_.get(), packet_.get());
      packet_pending_ = false;
      if (result == AVERROR_EOF) {
        // 读完容器不等于播放完：先交出两条轨道及重采样器的尾部数据。
        DecodeVideo(batch, true);
        DecodeAudio(batch, true);
        eof_ = true;
      } else {
        Check(result, "av_read_frame");
        if (packet_->stream_index == video_stream_->index)
          DecodeVideo(batch, false);
        else if (decode_audio_ && audio_stream_ && packet_->stream_index == audio_stream_->index)
          DecodeAudio(batch, false);
      }
      av_packet_unref(packet_.get());
    }
    if (awaiting_origin && origin_us_ != AV_NOPTS_VALUE)
      emit TimelineReady(session_, origin_us_, seek_origin_us_);
    batch.eof = eof_;
    if (!Canceled()) emit BatchReady(session_, std::move(batch));
  } catch (const std::exception& e) {
    if (!Canceled()) emit Failed(session_, QString::fromUtf8(e.what()));
  }
}
