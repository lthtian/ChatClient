#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "audio_capture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <QString>
#include <algorithm>
#include <deque>
#include <cstdint>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

namespace {
void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(
        QString("%1 (HRESULT 0x%2)").arg(operation).arg(quint32(result), 8, 16, QChar('0')).toStdString());
}
template<class T> void Release(T*& value) { if (value) { value->Release(); value = nullptr; } }
struct Block { qint64 position; std::vector<float> pcm; };
}

qint64 CaptureClockUs() {
    static const qint64 frequency = [] { LARGE_INTEGER value; QueryPerformanceFrequency(&value); return value.QuadPart; }();
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart / frequency * 1000000 + value.QuadPart % frequency * 1000000 / frequency;
}

struct AudioCapture::Data {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* format = nullptr;
    SwrContext* resampler = nullptr;
    std::deque<Block> blocks;
    bool started = false;
    ~Data() {
        if (started) client->Stop();
        Release(capture); Release(client); Release(device); Release(enumerator);
        CoTaskMemFree(format);
        swr_free(&resampler);
    }
};

AudioCapture::AudioCapture(bool loopback) : data_(new Data) {
    auto& d = *data_;
    Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&d.enumerator)), "创建音频设备枚举器");
    Check(d.enumerator->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &d.device), "查找默认音频设备");
    Check(d.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&d.client)), "打开音频端点");
    Check(d.client->GetMixFormat(&d.format), "查询设备 PCM 格式");
    const auto* format = d.format;
    WORD tag = format->wFormatTag;
    AVChannelLayout source{};
    if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
        if (extended->dwChannelMask) av_channel_layout_from_mask(&source, extended->dwChannelMask);
    }
    if (source.nb_channels != format->nChannels) {
        av_channel_layout_uninit(&source);
        av_channel_layout_default(&source, format->nChannels);
    }
    const AVSampleFormat sample_format = tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32 ? AV_SAMPLE_FMT_FLT
        : tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16 ? AV_SAMPLE_FMT_S16
        : tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 32 ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_NONE;
    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    const int result = sample_format == AV_SAMPLE_FMT_NONE ? AVERROR(EINVAL)
        : swr_alloc_set_opts2(&d.resampler, &stereo, AV_SAMPLE_FMT_FLT, 48000,
                             &source, sample_format, format->nSamplesPerSec, 0, nullptr);
    av_channel_layout_uninit(&source);
    if (result < 0 || swr_init(d.resampler) < 0) throw std::runtime_error("设备 PCM 格式不受支持");
    // Loopback 从播放端点取得系统混音；麦克风使用录音端点，不需要虚拟声卡。
    Check(d.client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                               2000000, 0, d.format, nullptr), "初始化音频采集");
    Check(d.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&d.capture)), "取得 PCM 读取接口");
    Check(d.client->Start(), "启动音频采集");
    d.started = true;
}
AudioCapture::~AudioCapture() = default;

void AudioCapture::Poll(qint64 origin_us) {
    auto& d = *data_;
    UINT32 available = 0;
    Check(d.capture->GetNextPacketSize(&available), "检查音频采集包");
    while (available) {
        BYTE* bytes = nullptr;
        UINT32 samples = 0;
        DWORD flags = 0;
        UINT64 qpc = 0;
        Check(d.capture->GetBuffer(&bytes, &samples, &flags, nullptr, &qpc), "读取音频采集包");
        // GetBuffer 的内存只借用到 ReleaseBuffer；转换必须在归还前完成。
        try {
            const qint64 delay = swr_get_delay(d.resampler, d.format->nSamplesPerSec);
            const int capacity = int(av_rescale_rnd(delay + samples, 48000, d.format->nSamplesPerSec, AV_ROUND_UP));
            std::vector<std::uint8_t> silence;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                silence.resize(std::size_t(samples) * d.format->nBlockAlign);
                bytes = silence.data();
            }
            Block block;
            const qint64 timestamp = flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR
                ? CaptureClockUs() - qint64(samples) * 1000000 / d.format->nSamplesPerSec : qint64(qpc / 10);
            block.position = av_rescale_q(timestamp - origin_us, AVRational{1, 1000000}, AVRational{1, 48000})
                - av_rescale_q(delay, AVRational{1, int(d.format->nSamplesPerSec)}, AVRational{1, 48000});
            block.pcm.resize(std::size_t(capacity) * 2);
            auto* output = reinterpret_cast<std::uint8_t*>(block.pcm.data());
            const auto* input = reinterpret_cast<const std::uint8_t*>(bytes);
            const int count = swr_convert(d.resampler, &output, capacity, &input, samples);
            if (count < 0) throw std::runtime_error("转换采集 PCM 失败");
            block.pcm.resize(std::size_t(count) * 2);
            if (count) d.blocks.push_back(std::move(block));
        } catch (...) { d.capture->ReleaseBuffer(samples); throw; }
        Check(d.capture->ReleaseBuffer(samples), "归还音频采集包");
        if (!d.blocks.empty() && d.blocks.back().position - d.blocks.front().position > 48000)
            throw std::runtime_error("音频采集积压超过一秒，无法保持实时推流");
        Check(d.capture->GetNextPacketSize(&available), "检查音频采集包");
    }
}

void AudioCapture::Mix(float* stereo, int samples, qint64 position, float gain) {
    auto& blocks = data_->blocks;
    while (!blocks.empty() && blocks.front().position + qint64(blocks.front().pcm.size() / 2) <= position)
        blocks.pop_front();
    for (const auto& block : blocks) {
        if (block.position >= position + samples) break;
        const qint64 first = std::max(position, block.position);
        const qint64 last = std::min(position + samples, block.position + qint64(block.pcm.size() / 2));
        for (qint64 sample = first; sample < last; ++sample)
            for (int channel = 0; channel < 2; ++channel)
                stereo[(sample - position) * 2 + channel] += block.pcm[(sample - block.position) * 2 + channel] * gain;
    }
}
