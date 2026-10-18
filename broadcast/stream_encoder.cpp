#include "stream_encoder.h"
#include "audio_capture.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {
void Check(int result, const char* operation) {
    if (result >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, text, sizeof(text));
    throw std::runtime_error(std::string(operation) + ": " + text);
}
}

struct StreamEncoder::Data {
    const std::atomic_bool& stopping;
    AVFormatContext* output = nullptr;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    AVStream* video_stream = nullptr;
    AVStream* audio_stream = nullptr;
    AVFrame* picture = nullptr;
    AVFrame* pcm = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;
    qint64 deadline = 0;
    explicit Data(const std::atomic_bool& stop) : stopping(stop) {}
    ~Data() {
        // 取消发布后只排空编码器的内部尾帧并归还内存，不再向已关闭的直播发送。
        for (auto* context : {video, audio}) {
            if (context && packet && avcodec_is_open(context) && avcodec_send_frame(context, nullptr) >= 0)
                while (avcodec_receive_packet(context, packet) >= 0) av_packet_unref(packet);
        }
        if (output && output->pb) avio_closep(&output->pb);
        avformat_free_context(output);
        avcodec_free_context(&video); avcodec_free_context(&audio);
        av_frame_free(&picture); av_frame_free(&pcm); av_packet_free(&packet);
        sws_freeContext(scaler);
    }
    static int Interrupted(void* opaque) {
        const auto& d = *static_cast<Data*>(opaque);
        return d.stopping || (d.deadline && CaptureClockUs() >= d.deadline);
    }
    void BeginIo() { deadline = CaptureClockUs() + 8000000; }
    void Encode(AVCodecContext* context, AVStream* stream, AVFrame* frame) {
        Check(avcodec_send_frame(context, frame), "avcodec_send_frame");
        while (true) {
            const int result = avcodec_receive_packet(context, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            Check(result, "avcodec_receive_packet");
            av_packet_rescale_ts(packet, context->time_base, stream->time_base);
            packet->stream_index = stream->index;
            BeginIo();
            // FLV 封装器写入 AVIO，RTMP 协议层将其组织为消息及 chunk。
            const int written = av_interleaved_write_frame(output, packet);
            av_packet_unref(packet);
            Check(written, "RTMP 写入失败");
        }
    }
};

StreamEncoder::StreamEncoder(const QString& url, const std::atomic_bool& stopping) : data_(new Data(stopping)) {
    auto& d = *data_;
    Check(avformat_network_init(), "avformat_network_init");
    Check(avformat_alloc_output_context2(&d.output, nullptr, "flv", nullptr), "创建 FLV 输出");
    d.output->interrupt_callback = {Data::Interrupted, &d};
    d.output->max_interleave_delta = 100000;
    const AVCodec* video_codec = avcodec_find_encoder_by_name("libx264");
    const AVCodec* audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!video_codec || !audio_codec) throw std::runtime_error("FFmpeg 缺少 libx264 或 AAC 编码器");
    d.video = avcodec_alloc_context3(video_codec);
    d.audio = avcodec_alloc_context3(audio_codec);
    d.picture = av_frame_alloc(); d.pcm = av_frame_alloc(); d.packet = av_packet_alloc();
    if (!d.video || !d.audio || !d.picture || !d.pcm || !d.packet) throw std::bad_alloc();
    d.video->width = 1280; d.video->height = 720;
    d.video->pix_fmt = AV_PIX_FMT_YUV420P;
    d.video->time_base = {1, 30}; d.video->framerate = {30, 1};
    d.video->bit_rate = 1800000; d.video->rc_max_rate = 2100000; d.video->rc_buffer_size = 3600000;
    d.video->gop_size = 60; d.video->max_b_frames = 0; d.video->thread_count = 2;
    d.video->color_range = AVCOL_RANGE_MPEG; d.video->colorspace = AVCOL_SPC_BT709;
    d.video->color_primaries = AVCOL_PRI_BT709; d.video->color_trc = AVCOL_TRC_BT709;
    d.video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    Check(av_opt_set(d.video->priv_data, "preset", "veryfast", 0), "x264 preset");
    Check(av_opt_set(d.video->priv_data, "tune", "zerolatency", 0), "x264 tune");
    Check(av_opt_set(d.video->priv_data, "x264-params", "keyint=60:min-keyint=60:scenecut=0", 0), "x264 GOP");
    Check(avcodec_open2(d.video, video_codec, nullptr), "打开 H.264 编码器");
    d.audio->sample_rate = 48000; d.audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
    d.audio->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    d.audio->time_base = {1, 48000}; d.audio->bit_rate = 128000;
    d.audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    Check(avcodec_open2(d.audio, audio_codec, nullptr), "打开 AAC 编码器");
    if (d.audio->frame_size != 1024) throw std::runtime_error("AAC 编码器帧长不是 1024");
    d.video_stream = avformat_new_stream(d.output, nullptr);
    d.audio_stream = avformat_new_stream(d.output, nullptr);
    if (!d.video_stream || !d.audio_stream) throw std::bad_alloc();
    d.video_stream->time_base = d.video->time_base;
    d.audio_stream->time_base = d.audio->time_base;
    Check(avcodec_parameters_from_context(d.video_stream->codecpar, d.video), "复制视频编码参数");
    Check(avcodec_parameters_from_context(d.audio_stream->codecpar, d.audio), "复制音频编码参数");
    d.picture->format = d.video->pix_fmt;
    d.picture->width = 1280; d.picture->height = 720;
    d.picture->color_range = d.video->color_range; d.picture->colorspace = d.video->colorspace;
    d.picture->color_primaries = d.video->color_primaries; d.picture->color_trc = d.video->color_trc;
    Check(av_frame_get_buffer(d.picture, 32), "分配视频帧");
    d.pcm->format = d.audio->sample_fmt; d.pcm->sample_rate = 48000;
    d.pcm->nb_samples = 1024;
    Check(av_channel_layout_copy(&d.pcm->ch_layout, &d.audio->ch_layout), "复制声道布局");
    Check(av_frame_get_buffer(d.pcm, 0), "分配音频帧");
    d.scaler = sws_getContext(1280, 720, AV_PIX_FMT_RGB24, 1280, 720, AV_PIX_FMT_YUV420P,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!d.scaler) throw std::runtime_error("创建像素转换器失败");
    const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
    Check(sws_setColorspaceDetails(d.scaler, coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16), "设置 RGB/YUV 色彩转换");
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rw_timeout", "8000000", 0);
    d.BeginIo();
    const auto location = url.toUtf8();
    const int opened = avio_open2(&d.output->pb, location.constData(), AVIO_FLAG_WRITE, &d.output->interrupt_callback, &options);
    av_dict_free(&options);
    Check(opened, "连接直播服务器失败");
    av_dict_set(&options, "flvflags", "no_duration_filesize", 0);
    d.BeginIo();
    const int header = avformat_write_header(d.output, &options);
    av_dict_free(&options);
    Check(header, "发送直播编码配置失败");
}
StreamEncoder::~StreamEncoder() = default;

void StreamEncoder::Video(const QImage& image, qint64 index) {
    auto& d = *data_;
    if (image.size() != QSize(1280, 720) || image.format() != QImage::Format_RGB888)
        throw std::runtime_error("桌面图像格式不正确");
    Check(av_frame_make_writable(d.picture), "复用视频帧");
    const std::uint8_t* pixels[] = {image.constBits(), nullptr, nullptr, nullptr};
    const int strides[] = {image.bytesPerLine(), 0, 0, 0};
    if (sws_scale(d.scaler, pixels, strides, 0, 720, d.picture->data, d.picture->linesize) != 720)
        throw std::runtime_error("RGB 转 YUV 失败");
    d.picture->pts = index;
    d.Encode(d.video, d.video_stream, d.picture);
}

void StreamEncoder::Audio(const float* stereo, qint64 position) {
    auto& d = *data_;
    Check(av_frame_make_writable(d.pcm), "复用音频帧");
    // 采集和混音用交错 float，AAC 编码器接收两个平面的 float。
    for (int channel = 0; channel < 2; ++channel) {
        auto* target = reinterpret_cast<float*>(d.pcm->data[channel]);
        for (int sample = 0; sample < 1024; ++sample) target[sample] = std::clamp(stereo[sample * 2 + channel], -1.0f, 1.0f);
    }
    d.pcm->pts = position;
    d.Encode(d.audio, d.audio_stream, d.pcm);
}

qint64 StreamEncoder::BytesWritten() const { return data_->output->pb->bytes_written; }
