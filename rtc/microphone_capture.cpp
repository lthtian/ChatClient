#include "microphone_capture.h"
#include "audio_capture.h"
#include <windows.h>
#include <memory>
#include <stdexcept>

MicrophoneCapture::MicrophoneCapture(qint64 origin_us) : origin_us_(origin_us) {}
MicrophoneCapture::~MicrophoneCapture() { Stop(); wait(); }
void MicrophoneCapture::SetEnabled(bool enabled) { enabled_ = enabled; }
void MicrophoneCapture::Stop() { stopping_ = true; }
void MicrophoneCapture::Mix(float* stereo, int samples, qint64 position) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_ && active_) active_->Mix(stereo, samples, position, 1.0f);
}

void MicrophoneCapture::run() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_ptr<AudioCapture> capture;
    // 先撤销借用指针，再在设备线程释放；编码端不会等待 Stop/Release。
    auto close = [&] {
        { std::lock_guard<std::mutex> lock(mutex_); active_ = nullptr; }
        capture.reset();
    };
    bool attempted = false;
    while (!stopping_) {
        if (!enabled_) {
            close(); attempted = false;
        } else {
            try {
                if (!attempted) {
                    attempted = true;
                    emit Status(QStringLiteral("正在打开麦克风，画面继续直播"));
                    if (FAILED(com)) throw std::runtime_error("COM 音频初始化失败");
                    // Activate/Initialize/Start 可能耗时数百毫秒，期间不持有 PCM 锁。
                    capture = std::make_unique<AudioCapture>(false);
                    if (!enabled_ || stopping_) { close(); continue; }
                    { std::lock_guard<std::mutex> lock(mutex_); active_ = capture.get(); }
                    emit Status(QStringLiteral("麦克风已开启"));
                }
                // Poll 只读取已到达的采样；与编码端 Mix 互斥，防止并发修改采样队列。
                std::lock_guard<std::mutex> lock(mutex_);
                if (capture) capture->Poll(origin_us_);
            } catch (const std::exception& error) {
                close();
                emit Status(QString::fromUtf8(error.what()) + QStringLiteral("；继续无声直播，可关闭再开启麦克风重试"));
            }
        }
        msleep(2);
    }
    close();
    if (SUCCEEDED(com)) CoUninitialize();
}
