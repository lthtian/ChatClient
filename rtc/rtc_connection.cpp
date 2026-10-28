#include "rtc_connection.h"
#include "media_codec.h"
#include "track_decoder.h"
#include "audio_capture.h"
#include "microphone_capture.h"
#include "rtp.h"
#include <rtc/rtc.hpp>
#include <QDateTime>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>
#include <atomic>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace {
// SR 的 NTP/RTP 必须对应同一个采集时刻，不能把排队后的发送时间配给旧采样。
class CaptureReport : public rtc::MediaHandler {
public:
    CaptureReport(std::shared_ptr<rtc::RtpPacketizationConfig> config, const quint64& origin)
        : reporter_(config), config_(std::move(config)), origin_(origin) {}
    void outgoing(rtc::message_vector& messages, const rtc::message_callback& send) override {
        reporter_.outgoing(messages, [&](rtc::message_ptr report) {
            if (report->size() >= rtc::RtcpSr::Size(0)) {
                auto* sr = reinterpret_cast<rtc::RtcpSr*>(report->data());
                const quint32 ticks = sr->rtpTimestamp() - config_->startTimestamp;
                sr->setNtpTimestamp(origin_ + (quint64(ticks) << 32) / config_->clockRate);
            }
            send(std::move(report));
        });
    }
private:
    rtc::RtcpSrReporter reporter_; // 复用库的报文计数和 SR/SDES 生成。
    std::shared_ptr<rtc::RtpPacketizationConfig> config_; // 本条轨道的 RTP 起点和单位。
    const quint64& origin_; // 发布线程建立的公共 NTP 原点，发布期间保持不变。
};
qint64 TimeUs(rtc::RtcpReceivingSession::SyncTimestamps sync, quint32 timestamp, int rate) {
    const qint64 ntp_us = qint64(sync.ntpTimestamp >> 32) * 1000000 +
        qint64((sync.ntpTimestamp & 0xffffffffULL) * 1000000 >> 32);
    return ntp_us + qint64(qint32(timestamp - quint32(sync.rtpTimestamp))) * 1000000 / rate;
}
struct HttpResult {
    int status; // HTTP 响应码，只有 201 代表成功建立会话。
    QByteArray body; // SDP Answer；DELETE 的响应体为空。
    QByteArray location; // 服务端返回的相对会话资源地址。
};
// 局部事件循环只运行在 RTC 工作线程；取消与超时会中止当前 HTTP 等待。
HttpResult Exchange(QNetworkAccessManager& network, const QUrl& url, const QByteArray& token,
                    const QByteArray& body, const std::atomic_bool& stop, bool removing) {
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", token);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/sdp");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto* reply = removing ? network.deleteResource(request) : network.post(request, body);
    QEventLoop loop;
    QTimer timer;
    timer.setInterval(20);
    const qint64 deadline = CaptureClockUs() + (removing ? 1500000 : 10000000);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if ((!removing && stop) || CaptureClockUs() >= deadline) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        if (reply->bytesAvailable() > 65536) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start();
    if (!reply->isFinished()) loop.exec();
    const HttpResult result{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                            reply->readAll(), reply->rawHeader("Location")};
    const auto error = reply->error();
    delete reply;
    if (result.body.size() > 65536 || (!result.status && error != QNetworkReply::NoError))
        throw std::runtime_error("RTC 信令连接失败、超时或已取消");
    return result;
}
}

struct RtcConnection::Data {
    QJsonObject descriptor; // 本次业务授权，票据只发送到配置允许的信令入口。
    bool publishing; // true 编码发送；false 重组接收并解码。
    int rate; // 观看设备的 PCM 采样率。
    std::atomic_bool stop{false}, microphone{false}, keyframe{true}; // GUI/网络回调交给工作线程的命令。
    std::mutex mutex; // 保护最新画面、待处理 RTP 和已解码批次。
    QImage picture; // 最新摄像头图像，之后的图像覆盖它。
    quint64 picture_number = 0; // 防止重复编码同一份采集画面。
    std::deque<std::pair<bool, QByteArray>> incoming; // 有界 RTP 邮箱；回调只复制，不解码。
    bool overflow = false; // 邮箱溢出后终止会话，防止无界延迟。
    MediaBatch media; // GUI 定时取走的独立图像和 PCM。
    quint64 ntp_origin = 0; // SR 使用的共同采集时钟原点。
};

RtcConnection::RtcConnection(QJsonObject descriptor, bool publishing, int rate, QObject* parent)
    : QThread(parent), data_(std::make_shared<Data>()) {
    data_->descriptor = std::move(descriptor); data_->publishing = publishing; data_->rate = rate;
}
RtcConnection::~RtcConnection() { Stop(); wait(); }
void RtcConnection::Stop() { data_->stop = true; }
void RtcConnection::SetMicrophone(bool enabled) { data_->microphone = enabled; }
void RtcConnection::SetPicture(QImage image) {
    std::lock_guard<std::mutex> lock(data_->mutex);
    data_->picture = std::move(image); ++data_->picture_number;
}
MediaBatch RtcConnection::TakeMedia() {
    std::lock_guard<std::mutex> lock(data_->mutex);
    MediaBatch result = std::move(data_->media); data_->media = {};
    return result;
}

void RtcConnection::run() {
    auto d = data_;
    std::shared_ptr<rtc::PeerConnection> peer;
    QNetworkAccessManager network;
    QUrl resource;
    QByteArray token;
    QString failure;
    try {
        const QUrl base(qEnvironmentVariable("CHAT_LIVE_RTC_URL", "http://39.105.18.142/rtc"));
        const QUrl url(d->descriptor["url"].toString());
        token = d->descriptor["headers"].toObject()["Authorization"].toString().toLatin1();
        const QString path = base.path() + (d->publishing ? "/publish/" : "/play/") + "[0-9a-f]{32}";
        if ((url.scheme() != "http" && url.scheme() != "https") || url.scheme() != base.scheme() ||
            url.host() != base.host() || url.port() != base.port() || !url.userInfo().isEmpty() ||
            url.hasQuery() || url.hasFragment() || !QRegularExpression("^" + path + "$").match(url.path()).hasMatch() ||
            !QRegularExpression("^Bearer [0-9a-f]{64}$").match(QString::fromLatin1(token)).hasMatch() ||
            d->descriptor["expires_at"].toVariant().toLongLong() <= QDateTime::currentMSecsSinceEpoch())
            throw std::runtime_error("RTC 授权地址或票据无效");
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        peer = std::make_shared<rtc::PeerConnection>(config);
        const auto direction = d->publishing ? rtc::Description::Direction::SendOnly : rtc::Description::Direction::RecvOnly;
        rtc::Description::Video video("video", direction);
        video.addH264Codec(96, "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1");
        rtc::Description::Audio audio("audio", direction);
        audio.addOpusCodec(111, "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1");
        const auto vs = QRandomGenerator::global()->generate(), as = QRandomGenerator::global()->generate();
        if (d->publishing) {
            video.addSSRC(vs, "camera", "camera", "video");
            audio.addSSRC(as, "camera", "camera", "audio");
        }
        const auto vt = peer->addTrack(video), at = peer->addTrack(audio);
        std::shared_ptr<rtc::RtcpReceivingSession> vr, ar;
        const std::weak_ptr<Data> weak = d;
        if (d->publishing) {
            const auto vc = std::make_shared<rtc::RtpPacketizationConfig>(vs, "camera", 96, 90000);
            const auto ac = std::make_shared<rtc::RtpPacketizationConfig>(as, "camera", 111, 48000);
            auto vp = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, vc, 1100);
            auto ap = std::make_shared<rtc::OpusRtpPacketizer>(ac);
            vp->addToChain(std::make_shared<CaptureReport>(vc, d->ntp_origin));
            vp->addToChain(std::make_shared<rtc::RtcpNackResponder>(512));
            vp->addToChain(std::make_shared<rtc::PliHandler>([weak] { if (auto p = weak.lock()) p->keyframe = true; }));
            ap->addToChain(std::make_shared<CaptureReport>(ac, d->ntp_origin));
            ap->addToChain(std::make_shared<rtc::RtcpNackResponder>(128));
            vt->setMediaHandler(vp); at->setMediaHandler(ap);
        } else {
            vr = std::make_shared<rtc::RtcpReceivingSession>();
            ar = std::make_shared<rtc::RtcpReceivingSession>();
            vt->setMediaHandler(vr); at->setMediaHandler(ar);
            for (auto item : {std::make_pair(vt, true), std::make_pair(at, false)}) {
                item.first->onMessage([weak, is_video = item.second](rtc::message_variant value) {
                    auto p = weak.lock();
                    const auto* bytes = std::get_if<rtc::binary>(&value);
                    if (!p || !bytes || p->stop) return;
                    std::lock_guard<std::mutex> lock(p->mutex);
                    // UDP 数据报上限也受检查；控制报文已由 RtcpReceivingSession 消费。
                    if (bytes->size() > 2048 || p->incoming.size() >= 512) { p->overflow = true; return; }
                    p->incoming.emplace_back(is_video, QByteArray(reinterpret_cast<const char*>(bytes->data()), int(bytes->size())));
                });
            }
        }
        emit Status(QStringLiteral("正在协商 WebRTC"));
        peer->setLocalDescription(rtc::Description::Type::Offer);
        const auto gathering_deadline = CaptureClockUs() + 8000000;
        while (!d->stop && peer->gatheringState() != rtc::PeerConnection::GatheringState::Complete) {
            if (CaptureClockUs() > gathering_deadline) throw std::runtime_error("ICE 地址收集超时");
            msleep(10);
        }
        if (d->stop) throw std::runtime_error("已取消");
        const auto offer = std::string(*peer->localDescription());
        const auto response = Exchange(network, url, token, QByteArray::fromStdString(offer), d->stop, false);
        if (response.status != 201 || !response.body.startsWith("v=0"))
            throw std::runtime_error("RTC 协商被拒绝，HTTP " + std::to_string(response.status));
        if (!QRegularExpression("^/rtc/session/[0-9a-f]{32}$").match(QString::fromLatin1(response.location)).hasMatch())
            throw std::runtime_error("RTC 会话资源地址无效");
        resource = url.resolved(QUrl(QString::fromLatin1(response.location)));
        peer->setRemoteDescription(rtc::Description(response.body.toStdString(), "answer"));
        const auto connected_deadline = CaptureClockUs() + 15000000;
        while (!d->stop && !(vt->isOpen() && at->isOpen())) {
            if (peer->state() == rtc::PeerConnection::State::Failed || CaptureClockUs() > connected_deadline)
                throw std::runtime_error("RTC 媒体连接失败，请检查服务器 UDP 8000 是否可达");
            msleep(10);
        }
        if (d->stop) throw std::runtime_error("已取消");
        emit Connected();
        const qint64 origin = CaptureClockUs();
        const quint64 epoch_ms = quint64(QDateTime::currentMSecsSinceEpoch()) + 2208988800000ULL;
        d->ntp_origin = ((epoch_ms / 1000) << 32) + ((epoch_ms % 1000) << 32) / 1000;
        if (d->publishing) {
            CameraEncoder encoder([&](bool is_video, QByteArray bytes, qint64 time) {
                const auto& track = is_video ? vt : at;
                track->sendFrame(reinterpret_cast<const rtc::byte*>(bytes.constData()), std::size_t(bytes.size()),
                                 rtc::FrameInfo(std::chrono::duration<double>(time / 1000000.0)));
            });
            MicrophoneCapture microphone(origin);
            connect(&microphone, &MicrophoneCapture::Status, this, &RtcConnection::Status);
            microphone.SetEnabled(d->microphone);
            microphone.start();
            qint64 sample = 0, last_video = -33333;
            quint64 image_number = 0;
            while (!d->stop) {
                if (peer->state() != rtc::PeerConnection::State::Connected) throw std::runtime_error("RTC 连接已断开");
                const qint64 elapsed = CaptureClockUs() - origin;
                microphone.SetEnabled(d->microphone);
                if (elapsed - sample * 1000000 / 48000 > 500000) throw std::runtime_error("编码落后超过半秒，请降低本机负载后重试");
                // 留出设备采集的短暂延迟，按相同采样时间轴发送，缺失位置为静音。
                while (sample * 1000000 / 48000 + 60000 <= elapsed) {
                    float pcm[960 * 2]{};
                    microphone.Mix(pcm, 960, sample);
                    encoder.Audio(pcm, sample); sample += 960;
                }
                if (elapsed - last_video >= 33333) {
                    QImage image;
                    { std::lock_guard<std::mutex> lock(d->mutex);
                      if (image_number != d->picture_number) { image = d->picture; image_number = d->picture_number; } }
                    if (!image.isNull()) { encoder.Video(image, elapsed, d->keyframe.exchange(false)); last_video = elapsed; }
                }
                msleep(2);
            }
        } else {
            TrackDecoder video_decoder, audio_decoder;
            std::unique_ptr<AVCodecParameters, void (*)(AVCodecParameters*)> parameters(
                avcodec_parameters_alloc(), [](AVCodecParameters* p) { avcodec_parameters_free(&p); });
            if (!parameters) throw std::bad_alloc();
            parameters->codec_type = AVMEDIA_TYPE_VIDEO; parameters->codec_id = AV_CODEC_ID_H264;
            video_decoder.Open(*parameters, AVRational{1, 1000000}, 33333);
            video_decoder.SetVideoLimit(QSize(1920, 1080));
            if (d->rate) {
                parameters->codec_type = AVMEDIA_TYPE_AUDIO; parameters->codec_id = AV_CODEC_ID_OPUS;
                parameters->sample_rate = 48000; parameters->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
                audio_decoder.Open(*parameters, AVRational{1, 1000000});
                audio_decoder.SetOutputRate(d->rate);
            }
            qint64 media_origin = AV_NOPTS_VALUE; // 两轨 SR 映射后的共同起点，不分别归零。
            camera::ReorderBuffer vq, aq;
            camera::H264Assembler assembler;
            struct Unit { bool video; QByteArray bytes; quint32 timestamp; };
            std::deque<Unit> pending;
            qint64 pending_bytes = 0, last_pli = 0, last_media = origin;
            auto enqueue = [&](bool is_video, QByteArray bytes, quint32 timestamp) {
                pending_bytes += bytes.size(); pending.push_back({is_video, std::move(bytes), timestamp});
                if (pending.size() > 250 || pending_bytes > 8 * 1024 * 1024) throw std::runtime_error("RTC 等待时钟的缓存达到上限");
            };
            while (!d->stop) {
                const qint64 now = CaptureClockUs();
                if (peer->state() != rtc::PeerConnection::State::Connected) throw std::runtime_error("RTC 连接已断开");
                std::deque<std::pair<bool, QByteArray>> incoming;
                { std::lock_guard<std::mutex> lock(d->mutex);
                  if (d->overflow) throw std::runtime_error("RTC 接收队列积压，已停止本次观看");
                  incoming.swap(d->incoming); }
                for (const auto& item : incoming) {
                    camera::RtpPacket packet;
                    if (!camera::ParseRtp(item.second, packet)) continue;
                    (item.first ? vq : aq).Insert(std::move(packet), now);
                    last_media = now;
                }
                auto pli = [&] { if (now - last_pli >= 500000) { vt->requestKeyframe(); last_pli = now; } };
                vq.Drain(now, [&](const camera::RtpPacket& packet) {
                    assembler.Push(packet, [&](QByteArray bytes, quint32 timestamp) { enqueue(true, std::move(bytes), timestamp); });
                }, [&](qint64) { assembler.Lost(); video_decoder.Reset(); pli(); });
                aq.Drain(now, [&](const camera::RtpPacket& packet) { enqueue(false, packet.payload, packet.timestamp); }, [](qint64) {});
                const auto vsync = vr->getSyncTimestamps(), async = ar->getSyncTimestamps();
                // 两条 SR 将独立的 RTP 起点映射到公共 NTP；不能把音视频分别归零。
                if (vsync.ntpTimestamp && async.ntpTimestamp) {
                    MediaBatch batch;
                    while (!pending.empty()) {
                        auto item = std::move(pending.front()); pending.pop_front(); pending_bytes -= item.bytes.size();
                        const qint64 time = TimeUs(item.video ? vsync : async, item.timestamp, item.video ? 90000 : 48000);
                        // 当前 RTC 编码无 B 帧，访问单元的解码和显示时刻一致。
                        if (item.video || d->rate)
                            (item.video ? video_decoder : audio_decoder).DecodeUnit(item.bytes, time, time, media_origin, batch);
                        // 首份 SR 到达前可能已经积累一个 GOP；解码参考链，播放只保留最近约 200ms。
                        while (batch.video.size() > 6) batch.video.removeFirst();
                        while (batch.audio.size() > 12) batch.audio.removeFirst();
                    }
                    std::lock_guard<std::mutex> lock(d->mutex);
                    d->media.video += std::move(batch.video); d->media.audio += std::move(batch.audio);
                    if (d->media.video.size() > 16 || d->media.audio.size() > 50)
                        throw std::runtime_error("界面消费媒体过慢，已停止本次观看");
                } else if (now - origin > 8000000) throw std::runtime_error("未收到完整的 RTCP 音视频时钟");
                if (now - origin < 2000000) pli();
                if (now - last_media > 10000000) throw std::runtime_error("十秒内未收到直播媒体");
                msleep(2);
            }
        }
    } catch (const std::exception& error) { if (!d->stop) failure = QString::fromUtf8(error.what()); }
    // DELETE 和 PeerConnection 的释放在工作线程完成；关闭窗口不等待网络超时。
    if (peer) peer->close();
    if (!resource.isEmpty()) {
        try { Exchange(network, resource, token, {}, d->stop, true); }
        catch (const std::exception&) { if (failure.isEmpty() && !d->stop) failure = QStringLiteral("RTC 会话释放失败"); }
    }
    peer.reset();
    if (!failure.isEmpty()) emit Failed(failure);
}
