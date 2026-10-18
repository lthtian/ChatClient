#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "desktop_publisher.h"
#include "audio_capture.h"
#include "stream_encoder.h"

#include <windows.h>
#include <QMutexLocker>
#include <array>
#include <algorithm>
#include <memory>
#include <stdexcept>

DesktopPublisher::DesktopPublisher(QObject* parent) : QThread(parent) {}
DesktopPublisher::~DesktopPublisher() { Stop(); wait(); }
void DesktopPublisher::Configure(const QString& url, bool system_audio, bool microphone) {
    Q_ASSERT(!isRunning());
    url_ = url; stopping_ = false; system_audio_ = system_audio; microphone_ = microphone;
}
void DesktopPublisher::SubmitImage(QImage image) { QMutexLocker lock(&image_mutex_); image_ = std::move(image); }
void DesktopPublisher::SetSystemAudio(bool enabled) { system_audio_ = enabled; }
void DesktopPublisher::SetMicrophone(bool enabled) { microphone_ = enabled; }
void DesktopPublisher::Stop() { stopping_ = true; }

void DesktopPublisher::run() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { emit Failed(QStringLiteral("初始化音频 COM 环境失败")); return; }
    // 在 COM 退出之前释放所有设备引用；每次重推建立独立的编码与时间线。
    {
        try {
            StreamEncoder encoder(url_, stopping_);
            std::unique_ptr<AudioCapture> system, microphone;
            auto update = [this](std::unique_ptr<AudioCapture>& capture, std::atomic_bool& enabled, bool loopback) {
                if (!enabled) { capture.reset(); return; }
                if (capture) return;
                try { capture.reset(new AudioCapture(loopback)); }
                catch (const std::exception& error) {
                    enabled = false;
                    emit AudioUnavailable(loopback, QString::fromUtf8(error.what()));
                }
            };
            const qint64 origin = CaptureClockUs();
            qint64 video_index = -1, audio_position = 0, report_time = origin, report_bytes = 0;
            bool announced = false;
            while (!stopping_) {
                update(system, system_audio_, true);
                update(microphone, microphone_, false);
                auto poll = [this, origin](std::unique_ptr<AudioCapture>& capture, std::atomic_bool& enabled, bool loopback) {
                    if (!capture) return;
                    try { capture->Poll(origin); }
                    catch (const std::exception& error) {
                        capture.reset(); enabled = false;
                        emit AudioUnavailable(loopback, QString::fromUtf8(error.what()));
                    }
                };
                poll(system, system_audio_, true); poll(microphone, microphone_, false);
                const qint64 elapsed = CaptureClockUs() - origin;
                const qint64 next_video = elapsed * 30 / 1000000;
                if (next_video > video_index) {
                    QImage image;
                    { QMutexLocker lock(&image_mutex_); image = image_; }
                    if (image.isNull()) throw std::runtime_error("没有可采集的桌面画面");
                    // 编码落后时直接使用当前画面和当前时间，避免追赶陈旧画面产生延迟。
                    encoder.Video(image, next_video);
                    video_index = next_video;
                }
                // 留 60ms 接收设备 PCM；音视频 PTS 仍使用同一个 QPC 原点。
                const qint64 ready_samples = std::max<qint64>(0, elapsed - 60000) * 48000 / 1000000;
                if (ready_samples - audio_position > 24000)
                    throw std::runtime_error("网络或编码积压超过 500ms，请检查上行网络或 CPU 占用");
                while (!stopping_ && audio_position + 1024 <= ready_samples) {
                    std::array<float, 2048> mixed{};
                    const float gain = system && microphone ? 0.5f : 1.0f;
                    if (system) system->Mix(mixed.data(), 1024, audio_position, gain);
                    if (microphone) microphone->Mix(mixed.data(), 1024, audio_position, gain);
                    // 无音源时仍编码静音，使运行中切换声音来源不改变媒体轨道。
                    encoder.Audio(mixed.data(), audio_position);
                    audio_position += 1024;
                }
                if (!announced && audio_position > 0) { announced = true; emit Publishing(); }
                const qint64 now = CaptureClockUs();
                if (now - report_time >= 1000000) {
                    const qint64 bytes = encoder.BytesWritten();
                    emit Statistics((now - origin) / 1000, int((bytes - report_bytes) * 8000 / (now - report_time)));
                    report_time = now; report_bytes = bytes;
                }
                msleep(3);
            }
        } catch (const std::exception& error) {
            if (!stopping_) emit Failed(QString::fromUtf8(error.what()));
        }
    }
    url_.clear();
    CoUninitialize();
}
