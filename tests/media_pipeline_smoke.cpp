#include "media_decoder.h"
#include "media_window.h"
#include "media_codec.h"
#include "playback.h"
#include "track_decoder.h"
#include "video_view.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void Wait(const std::function<bool()>& ready, const char* message, int timeout_ms = 10000) {
  QElapsedTimer timer; timer.start();
  while (!ready() && timer.elapsed() < timeout_ms) QTest::qWait(10);
  Require(ready(), message);
}
QByteArray Pixels(const QImage& image) {
  const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
  QByteArray pixels;
  for (int row = 0; row < rgb.height(); ++row)
    pixels.append(reinterpret_cast<const char*>(rgb.constScanLine(row)), rgb.width() * 3);
  return pixels;
}

// 用真实容器走 MediaDecoder，核对完整帧数和独立 FFmpeg 导出的首帧。
QJsonObject FileDecode(const QString& file, const QString& reference, int expected_frames) {
  MediaDecoder decoder;
  quint64 session = 1;
  decoder.RequestSession(session);
  int video = 0, audio = 0;
  bool eof = false;
  qint64 previous = -1;
  QString failure;
  QImage first;
  QObject::connect(&decoder, &MediaDecoder::Failed, [&](quint64, QString error) { failure = error; });
  QObject::connect(&decoder, &MediaDecoder::BatchReady, [&](quint64, MediaBatch batch) {
    for (const auto& picture : batch.video) {
      Require(picture.pts_us >= previous, "file frame PTS went backwards");
      previous = picture.pts_us;
      if (first.isNull()) first = picture.image;
      ++video;
    }
    audio += batch.audio.size(); eof = batch.eof;
  });
  MediaSource source; source.location = file;
  decoder.Open(source, session);
  decoder.SetOutputRate(48000);
  for (int count = 0; count < 100000 && !eof && failure.isEmpty(); ++count) decoder.Read(session);
  Require(failure.isEmpty(), qPrintable(failure));
  Require(eof && video == expected_frames && audio > 0, "file drain/frame/audio count mismatch");
  QFile reference_file(reference); Require(reference_file.open(QIODevice::ReadOnly), "cannot open RGB reference");
  const auto expected = reference_file.readAll(), actual = Pixels(first);
  Require(actual.size() == expected.size(), "RGB reference dimensions differ");
  int maximum_error = 0;
  for (int index = 0; index < actual.size(); ++index)
    maximum_error = std::max(maximum_error, std::abs(int(quint8(actual[index])) - int(quint8(expected[index]))));
  Require(maximum_error <= 2, "first RGB frame differs from FFmpeg reference");
  return {{"video_frames", video}, {"audio_blocks", audio}, {"rgb_max_error", maximum_error}};
}

// 原播放器实际运行解码线程、设备协商与公共播放组件，验证暂停和定位。
QJsonObject WindowPlayback(const QString& file, const QString& output, const QString& url = {}) {
  MediaWindow window;
  QDir().mkpath(output);
  if (!url.isEmpty()) {
    QObject::connect(&window, &MediaWindow::AuthorizationRequested, [&](quint64 session, const QString&) {
      MediaSource source;
      source.kind = MediaSource::Kind::Http;
      source.location = url;
      source.headers = "X-Lab: media-samples\r\n";
      source.expires_at_ms = QDateTime::currentMSecsSinceEpoch() + 60000;
      window.SetOnlineSource(session, source, {});
    });
  }
  const auto open = [&] { if (url.isEmpty()) window.OpenFile(file); else window.OpenOnline(file); };
  auto* playback = window.findChild<Playback*>();
  Require(playback, "window has no shared Playback");
  playback->Audio().SetVolume(0, true);
  int frames = 0;
  QObject::connect(&window, &MediaWindow::FramePresented, [&](qint64) { ++frames; });
  open();
  Wait([&] { return window.state() == MediaWindow::State::Error || frames >= 4; }, "file did not start");
  Require(window.state() != MediaWindow::State::Error, qPrintable(window.ErrorText()));
  window.TogglePlayback();
  const qint64 paused = window.PositionUs();
  QTest::qWait(100);
  Require(window.state() == MediaWindow::State::Paused && window.PositionUs() == paused, "pause clock advanced");
  window.SeekTo(2300000);
  Wait([&] { return window.state() == MediaWindow::State::Error || window.state() == MediaWindow::State::Paused; }, "paused seek timed out");
  Require(window.state() == MediaWindow::State::Paused && window.PositionUs() == 2300000, "paused seek position incorrect");
  const int preview_frames = frames;
  window.TogglePlayback();
  Wait([&] { return frames > preview_frames && window.PositionUs() > 2400000; }, "seek resume did not advance");
  Require(window.grab().save(output + "/file-player.png"), "cannot save player render");
  open(); open();
  Wait([&] { return window.state() == MediaWindow::State::Error || window.state() == MediaWindow::State::Playing; }, "reopen timed out");
  Require(window.state() == MediaWindow::State::Playing, qPrintable(window.ErrorText()));
  window.SeekTo(window.DurationUs() - 200000);
  Wait([&] { return window.state() == MediaWindow::State::Error || window.state() == MediaWindow::State::Ended; }, "playback tail did not drain");
  Require(window.state() == MediaWindow::State::Ended, qPrintable(window.ErrorText()));
  return {{"presented_frames", frames}, {"audio_device", window.HasAudioOutput()}, {"final_position_us", double(window.PositionUs())}};
}

// 使用真实 H.264/Opus 编码器生成访问单元，验证 RTC 输入交接和公共直播调度。
QJsonObject RtcMedia(const QString& output) {
  TrackDecoder video, audio;
  std::unique_ptr<AVCodecParameters, void (*)(AVCodecParameters*)> parameters(
      avcodec_parameters_alloc(), [](AVCodecParameters* p) { avcodec_parameters_free(&p); });
  Require(bool(parameters), "codec parameters allocation failed");
  parameters->codec_type = AVMEDIA_TYPE_VIDEO; parameters->codec_id = AV_CODEC_ID_H264;
  video.Open(*parameters, AVRational{1, 1000000}, 33333);
  parameters->codec_type = AVMEDIA_TYPE_AUDIO; parameters->codec_id = AV_CODEC_ID_OPUS;
  parameters->sample_rate = 48000; parameters->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
  audio.Open(*parameters, AVRational{1, 1000000});
  qint64 origin = AV_NOPTS_VALUE;
  constexpr qint64 epoch = 4000000000000000LL;
  MediaBatch decoded;
  CameraEncoder encoder([&](bool is_video, QByteArray bytes, qint64 pts) {
    (is_video ? video : audio).DecodeUnit(bytes, epoch + pts, epoch + pts, origin, decoded);
  });
  QImage image(1280, 720, QImage::Format_RGB32); image.fill(Qt::darkGreen);
  encoder.Video(image, 0, true);
  Require(!decoded.video.isEmpty(), "RTC first video was not decoded");
  const auto held = decoded.video.front().image;
  const auto held_hash = QCryptographicHash::hash(Pixels(held), QCryptographicHash::Sha256);
  qint64 sample = 0;
  for (int frame = 1; frame < 30; ++frame) {
    const qint64 time = frame * 1000000LL / 30;
    while (sample * 1000000 / 48000 <= time) {
      float pcm[960 * 2];
      for (int index = 0; index < 960; ++index)
        pcm[index * 2] = pcm[index * 2 + 1] = 0.1f * std::sin((sample + index) * 440.0 * 6.283185307179586 / 48000);
      encoder.Audio(pcm, sample); sample += 960;
    }
    encoder.Video(image, time, false);
  }
  Require(decoded.video.size() == 30 && decoded.audio.size() >= 45, "RTC access units did not decode completely");
  Require(origin == epoch && decoded.video.front().pts_us == 0, "RTC common origin was not normalized");
  Require(QCryptographicHash::hash(Pixels(held), QCryptographicHash::Sha256) == held_hash, "decoded image borrowed reusable memory");
  const auto color = held.pixelColor(held.width() / 2, held.height() / 2);
  Require(color.green() > color.red() + 50 && color.green() > color.blue() + 50, "RTC decoded color incorrect");
  qint64 previous_audio = -1;
  for (const auto& block : decoded.audio) {
    Require(block.pts_us >= previous_audio && block.pcm.size() % 4 == 0, "RTC PCM layout or time invalid");
    previous_audio = block.pts_us;
  }
  const int video_count = decoded.video.size(), audio_count = decoded.audio.size();
  const qint64 tail = decoded.video.back().pts_us;
  VideoView view; view.resize(640, 360);
  Playback playback(&view); playback.Configure(true, true);
  const int rate = playback.Audio().Open();
  // 输入 PCM 为 48 kHz；设备不支持该格式时只验证真实无声播放路径。
  if (rate != 48000) playback.Audio().Close();
  playback.Audio().SetVolume(0, true);
  qint64 presented = -1;
  QObject::connect(&playback, &Playback::FramePresented, [&](qint64 pts) { presented = pts; });
  playback.Append(std::move(decoded));
  Require(playback.PositionUs() >= tail - 200000, "live initial buffer did not join near current media");
  Require(playback.TryStart(true), "live initial buffer not ready");
  Wait([&] { playback.Tick(); return presented >= tail - 150000; }, "live frame not presented");
  // 参考链重置后要求编码器产生恢复点，音视频仍使用原来的公共起点。
  video.Reset();
  encoder.Video(image, 2500000, true);
  float silence[960 * 2]{}; encoder.Audio(silence, 120000);
  playback.Append(std::move(decoded));
  Require(playback.PositionUs() >= 2300000, "live backlog did not reanchor audio and video together");
  Wait([&] { playback.TryStart(true); playback.Tick(); return presented >= 2500000; }, "live recovery frame not shown");
  Wait([&] { return playback.Tick() == Playback::Result::Buffering; }, "live underrun did not freeze playback");
  encoder.Video(image, 4000000, true);
  encoder.Audio(silence, 192000);
  playback.Append(std::move(decoded));
  Require(playback.PositionUs() >= 3800000, "live refill stayed on the frozen clock");
  Wait([&] { playback.TryStart(true); playback.Tick(); return presented >= 4000000; }, "live refill frame not shown");
  Require(view.grab().save(output + "/rtc-player.png"), "cannot save RTC render");
  return {{"video_frames", video_count}, {"audio_blocks", audio_count}, {"audio_device", rate == 48000},
      {"recovered_pts_us", double(presented)}};
}
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  av_log_set_level(AV_LOG_ERROR);
  if (argc != 5 && argc != 6) return 2;
  try {
    const QString file = QString::fromLocal8Bit(argv[1]), reference = QString::fromLocal8Bit(argv[2]);
    const QString output = QString::fromLocal8Bit(argv[4]); QDir().mkpath(output);
    QJsonObject result;
    result["file_decode"] = FileDecode(file, reference, QString::fromLocal8Bit(argv[3]).toInt());
    result["window"] = WindowPlayback(file, output);
    if (argc == 6) result["http_window"] = WindowPlayback(file, output + "/http", QString::fromLocal8Bit(argv[5]));
    result["rtc"] = RtcMedia(output);
    QFile report(output + "/result.json"); Require(report.open(QIODevice::WriteOnly), "cannot write report");
    report.write(QJsonDocument(result).toJson());
    std::cout << "PASS " << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << std::endl;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << std::endl; return 1;
  }
}
