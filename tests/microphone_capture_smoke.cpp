#include "microphone_capture.h"
#include "audio_capture.h"
#include "media_codec.h"
#include "track_decoder.h"
#include <QCoreApplication>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

// 真实麦克风和编解码器在本机内存中运行；不建立网络连接，不保存 PCM。
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        TrackDecoder decoder;
        AVCodecParameters* parameters = avcodec_parameters_alloc();
        if (!parameters) throw std::bad_alloc();
        parameters->codec_type = AVMEDIA_TYPE_AUDIO; parameters->codec_id = AV_CODEC_ID_OPUS;
        parameters->sample_rate = 48000; parameters->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        try { decoder.Open(*parameters, AVRational{1, 1000000}); }
        catch (...) { avcodec_parameters_free(&parameters); throw; }
        avcodec_parameters_free(&parameters);
        qint64 media_origin = 0;
        int encoded_audio = 0, decoded_audio = 0, ready = 0;
        qint64 last_audio = -1;
        CameraEncoder encoder([&](bool video, QByteArray bytes, qint64 pts) {
            if (video) return;
            if (pts <= last_audio) throw std::runtime_error("audio timestamp did not advance");
            last_audio = pts; ++encoded_audio;
            MediaBatch batch; decoder.DecodeUnit(bytes, pts, pts, media_origin, batch);
            decoded_audio += batch.audio.size();
        });
        const qint64 origin = CaptureClockUs();
        MicrophoneCapture microphone(origin);
        bool unavailable = false;
        QObject::connect(&microphone, &MicrophoneCapture::Status, &app, [&](const QString& text) {
            if (text == QStringLiteral("麦克风已开启")) ++ready;
            if (text.contains(QStringLiteral("继续无声直播"))) unavailable = true;
            std::cout << "status_us=" << CaptureClockUs() - origin << ' ' << text.toUtf8().constData() << std::endl;
        });
        microphone.SetEnabled(true); microphone.start();
        QImage image(1280, 720, QImage::Format_RGB32); image.fill(Qt::darkGreen);
        qint64 sample = 0, last_video = -33333, max_gap = 0, max_mix = 0, max_lag = 0;
        double energy = 0; qint64 values = 0;
        while (CaptureClockUs() - origin < 8500000) {
            QCoreApplication::processEvents();
            const qint64 elapsed = CaptureClockUs() - origin;
            // 覆盖起播已开启、直播中两次关闭再开启，保持同一音视频时间线。
            const bool enabled = elapsed < 2000000 || (elapsed >= 3000000 && elapsed < 5000000) || elapsed >= 6000000;
            microphone.SetEnabled(enabled);
            max_lag = std::max(max_lag, elapsed - sample * 1000000 / 48000);
            while (sample * 1000000 / 48000 + 60000 <= elapsed) {
                float pcm[960 * 2]{};
                const qint64 begin = CaptureClockUs();
                microphone.Mix(pcm, 960, sample);
                max_mix = std::max(max_mix, CaptureClockUs() - begin);
                for (const float value : pcm) {
                    if (!std::isfinite(value) || (!enabled && value != 0)) throw std::runtime_error("invalid or unmuted PCM");
                    energy += double(value) * value; ++values;
                }
                encoder.Audio(pcm, sample); sample += 960;
            }
            if (elapsed - last_video >= 33333) {
                if (last_video >= 0) max_gap = std::max(max_gap, elapsed - last_video);
                encoder.Video(image, elapsed, last_video < 0); last_video = elapsed;
            }
            QThread::msleep(2);
        }
        microphone.Stop(); microphone.wait(); QCoreApplication::processEvents();
        if (unavailable || ready != 3 || max_lag >= 200000 || max_gap >= 200000 || decoded_audio < 400)
            throw std::runtime_error("microphone startup/toggle interrupted the media timeline");
        // 关闭发生于设备仍在启动时，也必须由该设备线程完成释放。
        const qint64 close = CaptureClockUs();
        { MicrophoneCapture opening(close); opening.SetEnabled(true); opening.start();
          QThread::msleep(40); opening.Stop(); opening.wait(); }
        std::cout << "PASS ready=" << ready << " encoded_audio=" << encoded_audio << " decoded_audio=" << decoded_audio
                  << " max_lag_us=" << max_lag << " max_video_gap_us=" << max_gap << " max_mix_us=" << max_mix
                  << " pcm_rms=" << std::sqrt(energy / values) << " close_during_open_us=" << CaptureClockUs() - close << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << std::endl; return 1;
    }
}
