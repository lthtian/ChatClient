#include "player/media_window.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSet>
#include <QFileInfo>
#include <algorithm>
#include <QTest>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const QString& message) {
  if (!value) throw std::runtime_error(message.toStdString());
}
void Wait(const std::function<bool()>& ready, const QString& message, int ms = 15000) {
  QElapsedTimer timer; timer.start();
  while (!ready() && timer.elapsed() < ms) QTest::qWait(10);
  Require(ready(), message);
}
void Ready(MediaWindow& player) {
  Wait([&] { return !player.CurrentImage().isNull() || player.state() == MediaWindow::State::Error; },
       "first frame timeout");
  Require(player.state() != MediaWindow::State::Error, player.ErrorText());
}
QJsonObject Decode(const QString& path, VideoPicture* first = nullptr) {
  MediaDecoder decoder;
  QString error;
  int video = 0, audio = 0, gaps = 0;
  qint64 previous_video = -1, previous_audio = -1, first_audio = -1;
  bool eof = false;
  QSet<qint64> steps;
  QObject::connect(&decoder, &MediaDecoder::Failed, [&](quint64, QString text) { error = text; });
  QObject::connect(&decoder, &MediaDecoder::AudioDiscontinuity,
      [&](quint64, qint64, qint64) { ++gaps; });
  QObject::connect(&decoder, &MediaDecoder::BatchReady, [&](quint64, MediaBatch batch) {
    for (const auto& picture : batch.video) {
      if (video == 0 && first) *first = picture;
      if (video > 0) {
        Require(picture.pts_us >= previous_video, "decoded video PTS decreased");
        steps.insert(picture.pts_us - previous_video);
      }
      previous_video = picture.pts_us; ++video;
    }
    for (const auto& block : batch.audio) {
      if (!audio) first_audio = block.pts_us;
      Require(block.pts_us >= previous_audio, "decoded audio PTS decreased");
      previous_audio = block.pts_us; ++audio;
    }
    eof = batch.eof;
  });
  MediaSource source; source.location = path;
  decoder.RequestSession(1); decoder.Open(source, 1);
  for (int packet = 0; packet < 10000 && !eof && error.isEmpty(); ++packet) decoder.Read(1);
  QJsonArray intervals;
  auto sorted = steps.values(); std::sort(sorted.begin(), sorted.end());
  for (auto interval : sorted) intervals.append(double(interval));
  return {{"name", QFileInfo(path).fileName()}, {"error", error}, {"eof", eof},
      {"video_frames", video}, {"audio_blocks", audio}, {"audio_first_pts_us", double(first_audio)},
      {"audio_discontinuities", gaps}, {"video_intervals_us", intervals}};
}
QRect ColoredBounds(const QImage& image) {
  int left = image.width(), right = -1, top = image.height(), bottom = -1;
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x) {
      const QColor color(image.pixel(x, y));
      if (color.red() + color.green() + color.blue() < 80) continue;
      left = std::min(left, x); right = std::max(right, x);
      top = std::min(top, y); bottom = std::max(bottom, y);
    }
  return QRect(QPoint(left, top), QPoint(right, bottom));
}
void Geometry(const QString& samples, const QString& output, QJsonArray& records) {
  struct Case { const char* name; double sar; bool swap; const char* corners; };
  const Case cases[]{
      {"base.mp4", 1, false, "RGBY"}, {"base.mkv", 1, false, "RGBY"},
      {"sar_4_3.mp4", 4.0 / 3, false, "RGBY"},
      {"rotate_90.mp4", 1, true, "GYRB"}, {"rotate_180.mp4", 1, false, "YBGR"},
      {"rotate_270.mp4", 1, true, "BRYG"}, {"rotate_90_sar.mp4", 4.0 / 3, true, "GYRB"},
      {"mirror.mp4", 1, false, "GRYB"}};
  VideoView view; view.resize(800, 600); view.show();
  for (const auto& sample : cases) {
    VideoPicture picture;
    auto record = Decode(samples + '/' + sample.name, &picture);
    Require(record["error"].toString().isEmpty() && record["eof"].toBool(), record["error"].toString());
    view.SetImage(picture.image, picture.geometry); QTest::qWait(20);
    const QImage image = view.grab().toImage();
    Require(image.save(output + '/' + sample.name + ".png"), "cannot save display evidence");
    const QRect bounds = ColoredBounds(image);
    double width = picture.image.width() * sample.sar, height = picture.image.height();
    if (sample.swap) std::swap(width, height);
    const double scale = std::min(800 / width, 600 / height);
    Require(std::abs(bounds.width() - width * scale) < 3 &&
            std::abs(bounds.height() - height * scale) < 3, QString(sample.name) + " display aspect mismatch");
    const QPoint corners[]{bounds.topLeft() + QPoint(8, 8), bounds.topRight() + QPoint(-8, 8),
        bounds.bottomLeft() + QPoint(8, -8), bounds.bottomRight() + QPoint(-8, -8)};
    for (int i = 0; i < 4; ++i) {
      const QColor color(image.pixel(corners[i]));
      const bool r = color.red() > 150, g = color.green() > 150, b = color.blue() > 150;
      const char actual = r && g && !b ? 'Y' : r && !g && !b ? 'R' : !r && g && !b ? 'G' : !r && !g && b ? 'B' : '?';
      Require(actual == sample.corners[i], QString(sample.name) + " corner orientation mismatch");
    }
    record["display_width"] = bounds.width(); record["display_height"] = bounds.height();
    records.append(record);
  }
  const auto unsupported = Decode(samples + "/rotate_45.mp4");
  Require(unsupported["error"].toString().contains("0/90/180/270"), "unsupported rotation was not rejected");
  records.append(unsupported);
  // RTC 交付的是已经定向的原始画面；三字段聚合和默认画布都采用单位几何。
  QImage image(320, 240, QImage::Format_RGB888); image.fill(Qt::red);
  VideoPicture rtc_picture{image, 100000, 33333};
  view.SetImage(rtc_picture.image); QTest::qWait(20);
  Require(ColoredBounds(view.grab().toImage()).size() == QSize(800, 600), "RTC default geometry mismatch");
}
void Playback(const QString& samples, const QString& output, QJsonArray& records) {
  qputenv("CHAT_MEDIA_TRACE", (output + "/timeline.jsonl").toLocal8Bit());
  MediaWindow player; player.show();
  player.OpenFile(samples + "/base.mp4"); Ready(player);
  Wait([&] { return player.PositionUs() > 300000; }, "play clock did not advance");
  player.TogglePlayback(); const qint64 paused = player.PositionUs(); QTest::qWait(600);
  Require(player.PositionUs() == paused, "paused clock advanced");
  player.SeekTo(2300000);
  Wait([&] { return player.state() == MediaWindow::State::Paused || player.state() == MediaWindow::State::Error; }, "paused seek timeout");
  Require(player.state() == MediaWindow::State::Paused && player.PositionUs() == 2300000, player.ErrorText());
  player.TogglePlayback(); QTest::qWait(400);
  player.OpenFile(samples + "/rotate_90.mp4");
  player.OpenFile(samples + "/base.mkv"); Ready(player);
  records.append(QJsonObject{{"playback", "pause_seek_reopen"}, {"snapshot", player.DiagnosticSnapshot()}});
  for (const auto& name : {"audio_late.mp4", "audio_gap.mkv", "vfr_bframes.mp4"}) {
    auto decoded = Decode(samples + '/' + name);
    Require(decoded["error"].toString().isEmpty() && decoded["eof"].toBool(), decoded["error"].toString());
    if (QString(name) == "audio_late.mp4") Require(decoded["audio_first_pts_us"].toDouble() > 600000, "late audio offset erased");
    if (QString(name) == "audio_gap.mkv") Require(decoded["audio_discontinuities"].toInt() > 0, "audio gap not detected");
    if (QString(name) == "vfr_bframes.mp4") Require(decoded["video_intervals_us"].toArray().size() > 2, "VFR intervals collapsed");
    records.append(decoded);
    player.OpenFile(samples + '/' + name); Ready(player);
    Wait([&] { return player.state() == MediaWindow::State::Ended || player.state() == MediaWindow::State::Error; }, "playback did not drain");
    Require(player.state() == MediaWindow::State::Ended, player.ErrorText());
    records.append(QJsonObject{{"playback", name}, {"snapshot", player.DiagnosticSnapshot()}});
  }
}
void Hls(const QString& url, const QString& output) {
  QDir().mkpath(output);
  qputenv("CHAT_MEDIA_TRACE", (output + "/hls.jsonl").toLocal8Bit());
  MediaWindow player; player.show();
  QObject::connect(&player, &MediaWindow::AuthorizationRequested,
      [&](quint64 session, const QString&) {
    MediaSource source; source.kind = MediaSource::Kind::Hls;
    source.location = url; source.name = "local-hls-lab";
    source.headers = "X-Lab: local\r\n";
    source.expires_at_ms = QDateTime::currentMSecsSinceEpoch() + 60000;
    player.SetOnlineSource(session, source, {});
  });
  player.OpenOnline("local-hls-lab"); Ready(player);
  player.TogglePlayback(); player.SeekTo(2300000);
  Wait([&] { return player.state() == MediaWindow::State::Paused || player.state() == MediaWindow::State::Error; }, "HLS seek timeout");
  Require(player.state() == MediaWindow::State::Paused && player.PositionUs() == 2300000, player.ErrorText());
  player.TogglePlayback();
  Wait([&] { return player.state() == MediaWindow::State::Ended || player.state() == MediaWindow::State::Error; }, "HLS drain timeout");
  Require(player.state() == MediaWindow::State::Ended, player.ErrorText());
  QFile result(output + "/result.json"); Require(result.open(QIODevice::WriteOnly), "cannot write result");
  result.write(QJsonDocument(player.DiagnosticSnapshot()).toJson());
}
void Network(const QString& url, const QString& gate_path, const QString& output) {
  QDir().mkpath(output);
  qputenv("CHAT_MEDIA_TRACE", (output + "/network.jsonl").toLocal8Bit());
  MediaWindow player; player.show();
  QObject::connect(&player, &MediaWindow::AuthorizationRequested,
      [&](quint64 session, const QString&) {
    MediaSource source;
    source.kind = MediaSource::Kind::Http;
    source.location = url; source.name = "local-network-lab";
    source.headers = "X-Lab: local\r\n";
    source.expires_at_ms = QDateTime::currentMSecsSinceEpoch() + 60000;
    player.SetOnlineSource(session, source, {});
  });
  player.OpenOnline("local-network-lab"); Ready(player);
  QFile gate(gate_path);
  Require(gate.open(QIODevice::WriteOnly), "cannot pause relay"); gate.close();
  Wait([&] { return player.state() == MediaWindow::State::Buffering || player.state() == MediaWindow::State::Error; },
       "network did not run out of buffered media", 12000);
  Require(player.state() == MediaWindow::State::Buffering, player.ErrorText());
  const qint64 frozen = player.PositionUs(); QTest::qWait(600);
  Require(player.PositionUs() == frozen, "starved clock advanced");
  Require(QFile::remove(gate_path), "cannot resume relay");
  Wait([&] { return player.state() == MediaWindow::State::Playing || player.state() == MediaWindow::State::Error; }, "network recovery timeout");
  Require(player.state() == MediaWindow::State::Playing, player.ErrorText());
  Require(gate.open(QIODevice::WriteOnly), "cannot pause relay"); gate.close();
  player.SeekTo(1000000);
  QTest::qWait(100);
  Require(player.DiagnosticSnapshot()["in_flight"].toBool(), "expected a blocked read before cancellation");
  // 阻塞请求被下一代取代；新打开仍然读取同一个真实 HTTP 源。
  player.OpenOnline("local-network-lab");
  Require(QFile::remove(gate_path), "cannot release relay");
  Ready(player);
  QFile result(output + "/result.json"); Require(result.open(QIODevice::WriteOnly), "cannot write result");
  result.write(QJsonDocument(player.DiagnosticSnapshot()).toJson());
}
}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  try {
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "hls") {
      Hls(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
      std::cout << "PASS HTTP HLS playback, paused seek and drain\n";
      return 0;
    }
    if (argc == 5 && QString::fromLocal8Bit(argv[1]) == "network") {
      Network(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]), QString::fromLocal8Bit(argv[4]));
      std::cout << "PASS network starvation, frozen clock, recovery and cancellation\n";
      return 0;
    }
    Require(argc == 3, "usage: media_reinforcement <sample-directory> <result-directory>");
    const QString samples = QString::fromLocal8Bit(argv[1]), output = QString::fromLocal8Bit(argv[2]);
    Require(QDir().mkpath(output), "cannot create result directory");
    QJsonArray records;
    Geometry(samples, output, records); Playback(samples, output, records);
    QFile result(output + "/result.json"); Require(result.open(QIODevice::WriteOnly), "cannot write result");
    result.write(QJsonDocument(records).toJson());
    std::cout << "PASS geometry, actual decoding, pause, seek, reopen, audio gaps and VFR\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
