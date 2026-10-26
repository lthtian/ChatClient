#include "media_codec.h"
#include <QPainter>
#include <algorithm>
#include <cstring>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace {
constexpr AVRational kUs{1, 1000000};
void Check(int result, const char* name) {
    if (result >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, error, sizeof(error));
    throw std::runtime_error(std::string(name) + ": " + error);
}
// 容器只拥有一组编解码资源，构造中途失败也能完整释放。
struct CodecData {
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVFrame* pcm = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    SwsContext* scaler = nullptr;
    SwrContext* resampler = nullptr;
    ~CodecData() {
        avcodec_free_context(&video); avcodec_free_context(&audio);
        av_frame_free(&frame); av_frame_free(&pcm); av_packet_free(&packet);
        sws_freeContext(scaler); swr_free(&resampler);
    }
    void Allocate(const AVCodec* v, const AVCodec* a) {
        if (!v || !a) throw std::runtime_error("FFmpeg 缺少 H.264 或 Opus 编解码器");
        video = avcodec_alloc_context3(v); audio = avcodec_alloc_context3(a);
        if (!video || !audio || !frame || !pcm || !packet) throw std::bad_alloc();
    }
};
}

struct CameraEncoder::Data : CodecData {
    Output output; // 编码结果直接交给 RTC 轨道，不保存无限增长的编码包队列。
    explicit Data(Output value) : output(std::move(value)) {}
    ~Data() {
        // 直播结束时排空编码器内部延迟，只释放尾包，不再向已关闭的轨道发送。
        for (auto* codec : {video, audio}) {
            if (!codec || !packet || !avcodec_is_open(codec)) continue;
            if (avcodec_send_frame(codec, nullptr) < 0) continue;
            while (avcodec_receive_packet(codec, packet) >= 0) av_packet_unref(packet);
        }
    }
    void Encode(AVCodecContext* codec, AVFrame* input, bool is_video) {
        Check(avcodec_send_frame(codec, input), "提交采集帧");
        for (;;) {
            const int result = avcodec_receive_packet(codec, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            Check(result, "取编码包");
            // 原生 RTP 不携带 OpusHead/pre-skip；用本帧采样起点标记发送时刻。
            const qint64 pts = is_video ? packet->pts : packet->pts + codec->initial_padding;
            output(is_video, QByteArray(reinterpret_cast<char*>(packet->data), packet->size),
                   av_rescale_q(pts, codec->time_base, kUs));
            av_packet_unref(packet);
        }
    }
};

CameraEncoder::CameraEncoder(Output output) : data_(new Data(std::move(output))) {
    auto& d = *data_;
    const auto* v = avcodec_find_encoder_by_name("libx264");
    const auto* a = avcodec_find_encoder_by_name("libopus");
    d.Allocate(v, a);
    d.video->width = 1280; d.video->height = 720;
    d.video->pix_fmt = AV_PIX_FMT_YUV420P;
    d.video->time_base = kUs; d.video->framerate = {30, 1};
    d.video->bit_rate = 1500000; d.video->rc_max_rate = 1800000; d.video->rc_buffer_size = 1500000;
    d.video->gop_size = 60; d.video->max_b_frames = 0; d.video->thread_count = 2;
    d.video->color_range = AVCOL_RANGE_MPEG; d.video->colorspace = AVCOL_SPC_BT709;
    d.video->color_primaries = AVCOL_PRI_BT709; d.video->color_trc = AVCOL_TRC_BT709;
    Check(av_opt_set(d.video->priv_data, "preset", "veryfast", 0), "x264 preset");
    Check(av_opt_set(d.video->priv_data, "tune", "zerolatency", 0), "x264 tune");
    Check(av_opt_set(d.video->priv_data, "profile", "baseline", 0), "x264 profile");
    // IDR 附带 SPS/PPS，后来加入的观众也能独立建立解码器。
    Check(av_opt_set(d.video->priv_data, "x264-params", "annexb=1:repeat-headers=1:keyint=60:scenecut=0:level=3.1", 0), "x264 参数");
    Check(avcodec_open2(d.video, v, nullptr), "打开 H.264 编码器");
    d.frame->format = d.video->pix_fmt; d.frame->width = 1280; d.frame->height = 720;
    Check(av_frame_get_buffer(d.frame, 32), "分配 YUV 帧");
    d.audio->sample_rate = 48000; d.audio->sample_fmt = AV_SAMPLE_FMT_FLT;
    d.audio->ch_layout = AV_CHANNEL_LAYOUT_STEREO; d.audio->time_base = {1, 48000};
    d.audio->bit_rate = 64000;
    Check(av_opt_set(d.audio->priv_data, "application", "lowdelay", 0), "Opus 模式");
    Check(av_opt_set(d.audio->priv_data, "frame_duration", "20", 0), "Opus 帧长");
    Check(avcodec_open2(d.audio, a, nullptr), "打开 Opus 编码器");
    if (d.audio->frame_size != 960) throw std::runtime_error("Opus 帧长必须为 960");
    d.pcm->format = AV_SAMPLE_FMT_FLT; d.pcm->sample_rate = 48000; d.pcm->nb_samples = 960;
    Check(av_channel_layout_copy(&d.pcm->ch_layout, &d.audio->ch_layout), "设置声道");
    Check(av_frame_get_buffer(d.pcm, 0), "分配 PCM 帧");
    d.scaler = sws_getContext(1280, 720, AV_PIX_FMT_RGB24, 1280, 720, AV_PIX_FMT_YUV420P,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!d.scaler) throw std::bad_alloc();
    const auto* colors = sws_getCoefficients(SWS_CS_ITU709);
    Check(sws_setColorspaceDetails(d.scaler, colors, 1, colors, 0, 0, 1 << 16, 1 << 16), "设置色彩转换");
}
CameraEncoder::~CameraEncoder() = default;

void CameraEncoder::Video(const QImage& source, qint64 time_us, bool keyframe) {
    auto& d = *data_;
    QImage image(1280, 720, QImage::Format_RGB888);
    image.fill(Qt::black);
    const QSize size = source.size().scaled(image.size(), Qt::KeepAspectRatio);
    { QPainter painter(&image); painter.drawImage(QRect(QPoint((1280-size.width())/2, (720-size.height())/2), size), source); }
    Check(av_frame_make_writable(d.frame), "复用 YUV 帧");
    const uint8_t* pixels[] = {image.constBits(), nullptr, nullptr, nullptr};
    const int strides[] = {image.bytesPerLine(), 0, 0, 0};
    Check(sws_scale(d.scaler, pixels, strides, 0, 720, d.frame->data, d.frame->linesize), "RGB 转 YUV");
    d.frame->pts = time_us;
    d.frame->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    d.Encode(d.video, d.frame, true);
}
void CameraEncoder::Audio(const float* stereo, qint64 position) {
    auto& d = *data_;
    Check(av_frame_make_writable(d.pcm), "复用 PCM 帧");
    std::memcpy(d.pcm->data[0], stereo, 960 * 2 * sizeof(float));
    d.pcm->pts = position;
    d.Encode(d.audio, d.pcm, false);
}
