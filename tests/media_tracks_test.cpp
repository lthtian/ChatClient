// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/10/28 14:49
#include "player/media_window.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QLabel>
#include <QFile>
#include <QJsonDocument>
#include <QTest>
#include <QtEndian>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <vector>

namespace {
void Require(bool value, const QString& message) {
  if (!value) throw std::runtime_error(message.toStdString());
}
double Tone(const QByteArray& pcm) {
  int crossings = 0;
  for (int i = 4; i + 3 < pcm.size(); i += 4)
    if (qFromLittleEndian<qint16>(pcm.constData() + i - 4) <= 0 &&
        qFromLittleEndian<qint16>(pcm.constData() + i) > 0) ++crossings;
  return pcm.isEmpty() ? 0 : crossings * 48000.0 / (pcm.size() / 4);
}
struct Decoded {
  MediaTracks tracks;
  QByteArray pcm;
  QString error, audio_error;
  int video = 0;
  qint64 first_audio = -1, first_video = -1;
  bool eof = false;
};
Decoded Decode(MediaSource source, qint64 position = 0) {
  MediaDecoder decoder;
  Decoded result;
  QObject::connect(&decoder, &MediaDecoder::Failed, [&](quint64, QString text) { result.error = text; });
  QObject::connect(&decoder, &MediaDecoder::AudioFailed, [&](quint64, int, QString text) { result.audio_error = text; });
  QObject::connect(&decoder, &MediaDecoder::TracksReady, [&](quint64, MediaTracks tracks) { result.tracks = tracks; });
  QObject::connect(&decoder, &MediaDecoder::BatchReady, [&](quint64, MediaBatch batch) {
    for (const auto& audio : batch.audio) {
      if (result.first_audio < 0) result.first_audio = audio.pts_us;
      result.pcm += audio.pcm;
    }
    for (const auto& video : batch.video) {
      if (result.first_video < 0) result.first_video = video.pts_us;
      ++result.video;
    }
    result.eof = batch.eof;
  });
  decoder.RequestSession(1); decoder.Open(source, 1);
  if (position && result.error.isEmpty()) decoder.Seek(position, 1);
  for (int i = 0; i < 20000 && !result.eof && result.error.isEmpty(); ++i) decoder.Read(1);
  return result;
}
void DefaultAudio(const QString& folder) {
  MediaSource source; source.location = folder + "/dual.mp4";
  auto result = Decode(source);
  Require(result.error.isEmpty() && result.audio_error.isEmpty() && result.eof, "decoder did not drain cleanly");
  const double hz = Tone(result.pcm);
  Require(std::abs(hz - 880) < 5, QString("default second audio expected 880 Hz; decoded %1 Hz").arg(hz));
  Require(result.tracks.audio == 2 && result.tracks.video == 0, "wrong default stream indices");
  Require(result.tracks.items[2].language == "zho" && result.tracks.items[2].is_default, "language/default missing");
  source.audio_stream = 1;
  result = Decode(source, 3000000);
  Require(result.eof && result.error.isEmpty() && std::abs(Tone(result.pcm) - 440) < 5,
          "explicit audio selection did not change PCM");
  Require(result.first_audio >= 3000000 && result.first_audio < 3030000 && result.first_video >= 2950000,
          "selected track did not resume at common timeline position");
  source.audio_stream = -1;
  result = Decode(source);
  Require(result.eof && result.pcm.isEmpty() && result.video == 192 && result.tracks.audio == -1,
          "disabled audio affected video or emitted PCM");
  source.audio_stream = -2;
  source.location = folder + "/first-default.mp4";
  result = Decode(source);
  Require(result.tracks.audio == 1 && std::abs(Tone(result.pcm) - 440) < 5, "first default selection failed");
  source.location = folder + "/dual.mkv";
  result = Decode(source);
  Require(result.tracks.items[2].title == "Chinese 880 Hz" && result.tracks.audio == 2,
          "Matroska title/default missing");
}
void ReopenAudio(const QString& path) {
  MediaDecoder decoder;
  MediaSource source; source.location = path;
  QByteArray pcm;
  QString error;
  quint64 expected = 1;
  bool eof = false;
  QObject::connect(&decoder, &MediaDecoder::Failed, [&](quint64, QString text) { error = text; });
  QObject::connect(&decoder, &MediaDecoder::BatchReady, [&](quint64 session, MediaBatch batch) {
    Require(session == expected, "stale decoder batch escaped cancellation");
    for (const auto& block : batch.audio) {
      if (expected == 2) Require(block.pts_us >= 3000000, "old audio survived reopen/seek");
      pcm += block.pcm;
    }
    eof = batch.eof;
  });
  decoder.RequestSession(1); decoder.Open(source, 1);
  for (int n = 0; n < 2000 && pcm.size() < 192000 && error.isEmpty(); ++n) decoder.Read(1);
  Require(error.isEmpty() && std::abs(Tone(pcm)-880) < 5, "first audio source invalid");
  pcm.clear(); expected = 2;
  source.audio_stream = 1;
  decoder.RequestSession(2); decoder.Open(source, 2); decoder.Seek(3000000, 2);
  decoder.Read(1);
  Require(pcm.isEmpty(), "stale Read delivered audio");
  for (int n = 0; n < 20000 && !eof && error.isEmpty(); ++n) decoder.Read(2);
  Require(error.isEmpty() && eof && std::abs(Tone(pcm)-440) < 5,
          "reused decoder retained old PCM, resampling or EOF state");
}
void Wait(const std::function<bool()>& ready, const QString& message) {
  QElapsedTimer time; time.start();
  while (!ready() && time.elapsed() < 15000) QTest::qWait(10);
  Require(ready(), message);
}
void AudioControls(const QString& path, const QString& url = {}) {
  MediaWindow player;
  int authorizations = 0;
  QObject::connect(&player, &MediaWindow::AuthorizationRequested, [&](quint64 session, QString) {
    ++authorizations;
    MediaSource source; source.kind = MediaSource::Kind::Http; source.location = url;
    source.name = "dual"; source.headers = "X-Lab: media-tracks\r\n";
    source.expires_at_ms = QDateTime::currentMSecsSinceEpoch() + 2500;
    player.SetOnlineSource(session, source, {});
  });
  if (url.isEmpty()) player.OpenFile(path); else player.OpenOnline("dual");
  auto* audio = player.findChild<QComboBox*>("mediaAudioTrack");
  Require(audio != nullptr, "audio track selector is missing");
  auto ready = [&] { return player.state() == MediaWindow::State::Playing || player.state() == MediaWindow::State::Error; };
  Wait(ready, "first playback timeout");
  Require(player.ErrorText().isEmpty() && audio->count() == 3 && audio->currentData().toInt() == 2,
          "audio selector/default mismatch: " + player.ErrorText());
  player.TogglePlayback();
  player.SeekTo(3000000);
  Wait([&] { return player.state() == MediaWindow::State::Paused || player.state() == MediaWindow::State::Error; }, "paused seek timeout");
  if (!url.isEmpty()) QTest::qWait(1700);
  const auto select = [&](int stream, bool play) {
    const qint64 before = player.PositionUs();
    const int index = audio->findData(stream);
    Require(index >= 0, "track not listed");
    audio->setCurrentIndex(index);
    QMetaObject::invokeMethod(audio, "activated", Qt::DirectConnection, Q_ARG(int, index));
    Wait([&] { return player.state() == (play ? MediaWindow::State::Playing : MediaWindow::State::Paused) ||
                     player.state() == MediaWindow::State::Error; }, "track change timeout");
    Require(player.ErrorText().isEmpty(), player.ErrorText());
    Require(audio->currentData().toInt() == stream, "selected stream not restored");
    Require(std::abs(player.PositionUs() - before) < 200000, "track switch reset media position");
    const auto snapshot = player.DiagnosticSnapshot();
    Require(snapshot["audio_stream"].toInt() == stream, "decoder/UI selection mismatch");
  };
  select(1, false);
  QTest::qWait(100);
  Require(player.PositionUs() == 3000000, "paused switch clock advanced");
  if (!url.isEmpty()) Require(authorizations >= 2, "track switch did not exercise credential renewal");
  player.TogglePlayback();
  select(2, true);
  select(-1, true);
  auto* notice = player.findChild<QLabel*>("mediaAudioNotice");
  Require(notice && notice->text().contains(QStringLiteral("已关闭音轨")), "silent choice missing persistent notice");
  select(1, true);
  player.Stop();
  player.TogglePlayback();
  Wait(ready, "restart timeout");
  Require(player.ErrorText().isEmpty() && audio->currentData().toInt() == 1, "restart lost track selection");
  player.OpenFile(path);
  Wait(ready, "reopen timeout");
  Require(player.ErrorText().isEmpty() && audio->currentData().toInt() == 2, "new open retained input-specific preference");
}
void Formats(const QString& folder) {
  for (const QString& name : {"silent.mp4", "two-video.mp4", "av1-10bit.mkv", "vp9-10bit.mkv", "hevc-10bit.mkv", "opus.webm", "surround.mkv"}) {
    MediaSource source; source.location = folder + '/' + name;
    const auto result = Decode(source);
    Require(result.error.isEmpty() && result.audio_error.isEmpty() && result.eof && result.video > 0,
            name + ": " + result.error + result.audio_error);
    if (name.contains("10bit")) Require(result.tracks.items[0].format.contains("10 bit"), name + " bit depth missing");
    if (name == "two-video.mp4") Require(result.tracks.video == 1 && result.video == 24, "default video selection failed");
    if (name == "surround.mkv") {
      Require(result.tracks.items[1].format.contains("96000 Hz"), "source sample rate missing");
      Require(result.pcm.size() >= 48000 * 4, "surround was not converted to 48 kHz stereo S16");
    }
    if (name == "silent.mp4") Require(result.tracks.audio == -1 && result.pcm.isEmpty() &&
        result.tracks.audio_notice.contains(QStringLiteral("没有音轨")), "absent audio misclassified");
  }
}
void EndSeek(const QString& folder) {
  MediaSource source; source.location = folder + "/av1-10bit.mkv";
  const auto result = Decode(source, 999000);
  Require(result.error.isEmpty() && result.eof && result.video == 1 && result.first_video == 958000,
          "seek into rounded container tail lost the last decoded picture");
  Require(Decode(source, 1100000).video == 0, "tail preview concealed a gap larger than timestamp precision");
  MediaWindow player; player.OpenFile(source.location);
  Wait([&] { return player.state() == MediaWindow::State::Playing || player.state() == MediaWindow::State::Error; }, "tail fixture startup timeout");
  Require(player.ErrorText().isEmpty(), player.ErrorText());
  player.TogglePlayback(); player.SeekTo(2000000);
  Wait([&] { return player.state() == MediaWindow::State::Paused || player.state() == MediaWindow::State::Error; }, "tail seek timeout");
  Require(player.state() == MediaWindow::State::Paused && !player.CurrentImage().isNull(), player.ErrorText());
}
void TimestampFreeTail(const QString& folder) {
  AVFormatContext* raw = nullptr;
  Require(avformat_open_input(&raw, (folder + "/av1-10bit.mkv").toUtf8().constData(), nullptr, nullptr) >= 0,
          "timestamp fixture open failed");
  std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> input(raw,
      [](AVFormatContext* p) { avformat_close_input(&p); });
  Require(avformat_find_stream_info(raw, nullptr) >= 0, "timestamp fixture probe failed");
  std::unique_ptr<AVPacket, void(*)(AVPacket*)> packet(av_packet_alloc(),
      [](AVPacket* p) { av_packet_free(&p); });
  Require(bool(packet), "packet allocation failed");
  std::vector<decltype(packet)> packets;
  while (av_read_frame(raw, packet.get()) >= 0) {
    if (packet->stream_index == 0)
      packets.emplace_back(av_packet_clone(packet.get()), packet.get_deleter());
    av_packet_unref(packet.get());
  }
  Require(packets.size() == 24, "timestamp fixture packet count differs");
  for (const auto& item : packets) Require(bool(item), "packet reference allocation failed");
  // 保留定位前缀的时间锚点，仅尾部真实编码包缺失 PTS/DTS。
  packets.back()->pts = packets.back()->dts = AV_NOPTS_VALUE;
  TrackDecoder decoder;
  decoder.Open(*raw->streams[0]->codecpar, raw->streams[0]->time_base, 41667);
  qint64 origin = 0;
  MediaBatch batch;
  const auto decode = [&] {
    for (const auto& item : packets) decoder.Decode(item.get(), origin, batch);
    decoder.Drain(origin, batch);
  };
  decode();
  Require(batch.video.size() == 24, "timestamp-free reference frame count differs");
  const auto last = batch.video.back();
  Require(last.duration_us > 0, "timestamp-free reference duration missing");
  batch = {};
  decoder.Reset(last.pts_us + last.duration_us);
  decode();
  Require(batch.video.size() == 1 && batch.video[0].pts_us == last.pts_us && batch.video[0].duration_us == last.duration_us,
          QString("timestamp-free tail: count=%1 actual=%2/%3 reference=%4/%5")
              .arg(batch.video.size()).arg(batch.video.isEmpty() ? -1 : batch.video.front().pts_us)
              .arg(batch.video.isEmpty() ? -1 : batch.video.front().duration_us).arg(last.pts_us).arg(last.duration_us));
}
void Failures(const QString& folder) {
  for (const QString& name : {"audio-fallback.mkv", "audio-init-failure.mkv", "audio-unavailable.mkv", "video-unavailable.mkv", "audio-damaged.mp4", "rate-change.ts"}) {
    std::cerr << "BEGIN " << name.toStdString() << '\n';
    MediaSource source; source.location = folder + '/' + name;
    auto result = Decode(source);
    if (name == "video-unavailable.mkv") {
      Require(result.error.contains(QStringLiteral("没有对应解码器")) && result.video == 0,
              "missing video decoder not identified");
    } else {
      Require(result.error.isEmpty() && result.eof && result.video == (name == "rate-change.ts" ? 52 : 192),
              name + " stopped video: " + result.error);
      if (name == "rate-change.ts") Require(result.audio_error.contains(QStringLiteral("中途改变采样格式")),
          "dynamic sample format change was not reported");
      if (name == "audio-fallback.mkv")
        Require(result.tracks.audio == 1 && std::abs(Tone(result.pcm)-440) < 5, "unavailable default did not fall back");
      if (name == "audio-init-failure.mkv")
        Require(result.tracks.audio == 1 && result.tracks.items[2].failure == "decoder_init" &&
            result.tracks.items[2].decoder_available, "initialization failure confused with missing decoder");
      if (name == "audio-unavailable.mkv") {
        Require(result.tracks.audio == -1 && result.pcm.isEmpty() &&
            result.tracks.audio_notice.contains(QStringLiteral("无声播放")), "all unavailable audio not reported");
        MediaWindow player; player.OpenFile(source.location);
        Wait([&] { return player.state() == MediaWindow::State::Playing || player.state() == MediaWindow::State::Error; }, "silent playback timeout");
        auto* notice = player.findChild<QLabel*>("mediaAudioNotice");
        Require(player.ErrorText().isEmpty() && notice && notice->text().contains(QStringLiteral("没有对应解码器")),
                "unavailable audio did not continue with persistent reason");
      }
      if (name == "audio-damaged.mp4") {
        Require(result.audio_error.contains(QStringLiteral("解码或音频转换失败")), "midstream audio failure not identified");
        source.excluded_audio = {2};
        result = Decode(source, 2000000);
        Require(result.tracks.audio == 1 && result.audio_error.isEmpty() && std::abs(Tone(result.pcm)-440) < 5,
                "failed track was retried instead of usable fallback");
        MediaWindow player; player.OpenFile(source.location);
        Wait([&] { return player.state() == MediaWindow::State::Playing || player.state() == MediaWindow::State::Error; }, "damaged input startup timeout");
        Require(player.ErrorText().isEmpty(), player.ErrorText());
        // 无设备时也读取真实 AAC；Playback 直接丢弃 PCM，测试不模拟音频设备消费。
        player.AudioEnabledRequested(true);
        Wait([&] { return player.DiagnosticSnapshot()["audio_stream"].toInt() == 1 ||
                         player.state() == MediaWindow::State::Error; }, "runtime audio fallback timeout");
        Wait([&] { return player.state() == MediaWindow::State::Playing || player.state() == MediaWindow::State::Error; }, "runtime fallback resume timeout");
        Require(player.ErrorText().isEmpty() && player.PositionUs() > 1500000,
                "runtime fallback lost playback position or failed video");
        Require(player.DiagnosticSnapshot()["audio_notice"].toString().contains(QStringLiteral("解码或音频转换失败")),
                "runtime failure reason disappeared");
      }
    }
    std::cerr << "END " << name.toStdString() << '\n';
  }
}
}
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    Require(argc == 2 || argc == 3, "usage: media_tracks_test samples-directory [HTTP URL]");
    const QString folder = QString::fromLocal8Bit(argv[1]);
    DefaultAudio(folder);
    ReopenAudio(folder + "/dual.mp4");
    AudioControls(folder + "/dual.mp4", argc == 3 ? QString::fromLocal8Bit(argv[2]) : QString());
    Formats(folder);
    EndSeek(folder);
    TimestampFreeTail(folder);
    Failures(folder);
    std::cout << "PASS track metadata, default/fallback, PCM selection, pause/seek/reopen, formats and failures\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
