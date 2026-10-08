#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QTest>
#include <QTimer>
#include <functional>
#include <iostream>
#include <stdexcept>
#include "media_transfer.h"
#include "tcpclient.h"
#include "player/media_window.h"

namespace {
void Require(bool ok, const char* error) { if (!ok) throw std::runtime_error(error); }
void Wait(const std::function<bool()>& ready, const char* error) {
  QElapsedTimer time; time.start();
  while (!ready() && time.elapsed() < 30000) QTest::qWait(10);
  Require(ready(), error);
}
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    Require(argc == 2, "usage: hls_playback_smoke fixture.json");
    QFile file(QString::fromLocal8Bit(argv[1]));
    Require(file.open(QIODevice::ReadOnly), "cannot read fixture");
    const auto fixture = QJsonDocument::fromJson(file.readAll()).object();
    MyTcpClient client;
    client.connectToHost("39.105.18.142", 7000);
    Wait([&] { return client.getSocket()->state() == QAbstractSocket::ConnectedState; }, "connect timeout");
    client.sendJson({{"msgid", 1}, {"username", fixture["username"]}, {"password", fixture["password"]}});
    QJsonObject login;
    Wait([&] {
      for (auto bytes = client.read(); !bytes.isEmpty(); bytes = client.read()) {
        const auto value = QJsonDocument::fromJson(bytes).object();
        if (value["msgid"].toInt() == 2) login = value;
      }
      return !login.isEmpty();
    }, "login timeout");
    Require(login["errno"].toInt() == 0, "login rejected");
    MediaTransfer transfer(&client);
    QTimer dispatch;
    QObject::connect(&dispatch, &QTimer::timeout, &app, [&] {
      for (auto bytes = client.read(); !bytes.isEmpty(); bytes = client.read())
        transfer.handleResponse(QJsonDocument::fromJson(bytes).object());
    });
    dispatch.start(5);
    for (const auto& value : fixture["videos"].toArray()) {
      const auto video = value.toObject();
      MediaWindow player;
      int authorizations = 0;
      QObject::connect(&player, &MediaWindow::AuthorizationRequested, &app,
          [&](quint64 session, const QString& rendition) {
        ++authorizations;
        transfer.requestPlaybackSource(fixture["conversation"].toObject(), video, rendition,
            [&, session](MediaSource source, const QString& error) {
          // 缩短本次测试客户端认定的期限，走真实重新授权；不篡改服务端时钟。
          if (authorizations == 1) source.expires_at_ms = QDateTime::currentMSecsSinceEpoch() + 4500;
          player.SetOnlineSource(session, std::move(source), error);
        });
      });
      player.show();
      player.OpenOnline(video["name"].toString());
      const auto ready = [&] { return !player.CurrentImage().isNull() || player.state() == MediaWindow::State::Error; };
      Wait(ready, "first HLS frame timeout");
      if (player.state() == MediaWindow::State::Error) throw std::runtime_error(player.ErrorText().toStdString());
      auto* quality = player.findChild<QComboBox*>("mediaQuality");
      Require(quality && quality->count() == video["variants"].toInt(), "dynamic quality list mismatch");
      player.TogglePlayback();
      player.SeekTo(2300000);
      Wait([&] { return player.state() == MediaWindow::State::Paused || player.state() == MediaWindow::State::Error; }, "paused seek timeout");
      Require(player.state() == MediaWindow::State::Paused, "paused seek failed");
      Require(qAbs(player.PositionUs() - 2300000) < 150000, "seek position mismatch");
      quality->setCurrentIndex(quality->count() - 1);
      QMetaObject::invokeMethod(quality, "activated", Qt::DirectConnection, Q_ARG(int, quality->count() - 1));
      Wait([&] { return player.state() == MediaWindow::State::Paused || player.state() == MediaWindow::State::Error; }, "quality switch timeout");
      Require(player.state() == MediaWindow::State::Paused && qAbs(player.PositionUs() - 2300000) < 150000,
              "switch did not preserve paused position");
      Require(player.CurrentImage().height() == 360, "selected rendition dimensions mismatch");
      // 暂停期间让授权跨过到期点，再恢复；必须保留 360p 和目标位置。
      QTest::qWait(4500);
      player.TogglePlayback();
      Wait([&] { return authorizations >= 2 && player.PositionUs() > 2500000; }, "authorization renewal timeout");
      Require(quality->currentText() == "360p", "renewal lost rendition selection");
      Require(player.state() != MediaWindow::State::Error, "playback failed after renewal");
      std::cout << "HLS passed: variants=" << quality->count() << " image=" << player.CurrentImage().width()
                << 'x' << player.CurrentImage().height() << " authorizations=" << authorizations << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
