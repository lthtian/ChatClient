#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTest>
#include <QTimer>
#include <functional>
#include <iostream>
#include <stdexcept>
#include "media_transfer.h"
#include "tcpclient.h"
#include "player/media_window.h"
#include "mainwindow.h"

namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& ready, const char* message) {
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < 30000) QTest::qWait(10);
  Require(ready(), message);
}
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
    std::cerr << "Qt: " << message.toUtf8().constData() << '\n';
  });
  QApplication::setQuitOnLastWindowClosed(false);
  try {
    Require(argc == 2, "usage: live_playback_smoke fixture.json");
    QFile file(QString::fromLocal8Bit(argv[1]));
    Require(file.open(QIODevice::ReadOnly), "fixture unavailable");
    const auto fixture = QJsonDocument::fromJson(file.readAll()).object();
    MyTcpClient client;
    client.connectToHost(fixture["host"].toString("127.0.0.1"), fixture["port"].toInt());
    Wait([&] { return client.getSocket()->state() == QAbstractSocket::ConnectedState; }, "chat connect timeout");
    client.sendJson({{"msgid", 1}, {"username", fixture["username"]}, {"password", fixture["password"]}});
    QJsonObject login;
    Wait([&] {
      for (auto bytes = client.read(); !bytes.isEmpty(); bytes = client.read()) {
        const auto value = QJsonDocument::fromJson(bytes).object();
        if (value["msgid"].toInt() == 2) login = value;
      }
      return !login.isEmpty();
    }, "login timeout");
    Require(login["errno"].toInt() == 0, "login failed");
    MediaTransfer transfer(&client);
    MediaWindow* current = nullptr;
    QTimer dispatch;
    QObject::connect(&dispatch, &QTimer::timeout, &app, [&] {
      for (auto bytes = client.read(); !bytes.isEmpty(); bytes = client.read()) {
        const auto response = QJsonDocument::fromJson(bytes).object();
        if (response["msgid"].toInt() == 29 && current)
          current->SetLiveState(response["live"].toObject()["state"].toString());
        else transfer.handleResponse(response);
      }
    });
    dispatch.start(5);
    const QStringList formats = fixture["cards_only"].toBool() ? QStringList{} : QStringList{"flv", "hls"};
    for (const auto& format : formats) {
      MediaWindow player;
      current = &player;
      QSignalSpy frames(&player, &MediaWindow::FramePresented);
      QSignalSpy seeks(&player, &MediaWindow::SeekRequested);
      int authorizations = 0;
      QObject::connect(&player, &MediaWindow::AuthorizationRequested, &app,
          [&](quint64 session, const QString&) {
        ++authorizations;
        transfer.requestLiveSource(fixture["conversation"].toObject(), fixture["live"].toObject(), format,
            [&, session](MediaSource source, const QString& error) {
          // 只缩短客户端认定的期限，观察真实业务续签和重新加入，不修改服务端时钟。
          if (authorizations == 1) source.expires_at_ms = QDateTime::currentMSecsSinceEpoch() + 6000;
          player.SetOnlineSource(session, std::move(source), error);
        });
      });
      player.show();
      player.OpenLive("stage9", format == "hls");
      Wait([&] { return frames.count() >= 3 || player.state() == MediaWindow::State::Error; }, "live frame timeout");
      if (player.state() == MediaWindow::State::Error) throw std::runtime_error(player.ErrorText().toStdString());
      Require(player.DurationUs() == 0, "live acquired finite duration");
      player.TogglePlayback();
      Require(player.state() == MediaWindow::State::Paused, "live pause failed");
      const int count = frames.count(), requests = authorizations;
      QTest::qWait(700);
      Require(frames.count() == count, "paused live still presents frames");
      player.TogglePlayback();
      Wait([&] { return authorizations > requests && frames.count() > count + 3; }, "live resume did not rejoin");
      Require(seeks.count() == 0, "live unexpectedly invoked seek");
      if (format == "flv" && !fixture["control_dir"].toString().isEmpty()) {
        const int before = authorizations;
        QFile request(fixture["control_dir"].toString() + "/reconnect-request");
        Require(request.open(QIODevice::WriteOnly), "reconnect marker unavailable");
        request.write("ready"); request.close();
        Wait([&] { return QFileInfo::exists(fixture["control_dir"].toString() + "/reconnect-done"); }, "publisher restart timeout");
        const int previous = frames.count();
        Wait([&] { return authorizations > before && frames.count() > previous + 3; }, "live failed to recover after republish");
      }
      player.SetLiveState("ended");
      Require(player.state() == MediaWindow::State::Ended, "end state not applied");
      player.grab().save(QString::fromLocal8Bit(argv[1]) + "." + format + ".png");
      std::cout << "PASS Qt " << format.toStdString() << ": frames=" << frames.count()
                << " authorizations=" << authorizations << " seeks=" << seeks.count() << '\n';
      current = nullptr;
    }
    if (fixture["check_cards"].toBool()) {
      dispatch.stop();
      MainWindow chat(nullptr, &client);
      QObject::connect(chat.statusBar(), &QStatusBar::messageChanged, &app, [](const QString& text) {
        if (!text.isEmpty()) std::cerr << "chat status: " << text.toUtf8().constData() << '\n';
      });
      chat.setUser(login["id"].toInt(), fixture["username"].toString());
      // 此处已登录，显式连接读取槽；正常入口在登录前创建窗口并绑定 connected。
      QObject::connect(client.getSocket(), &QTcpSocket::readyRead, &chat, &MainWindow::recvHandler);
      chat.initByTCP();
      auto* contacts = chat.findChild<MyListWidget*>("contactList");
      Wait([&] { return contacts->count() > 0; }, "chat contacts unavailable");
      contacts->setCurrentRow(0);
      chat.show();
      QMetaObject::invokeMethod(&chat, "reloadMsgList", Qt::DirectConnection);
      QFrame* card = nullptr;
      try { Wait([&] {
        for (auto* value : chat.findChildren<QFrame*>("liveCard"))
          if (value->property("live_id").toString() == fixture["live"].toObject()["live_id"].toString()) card = value;
        return card != nullptr;
      }, "live history card unavailable"); }
      catch (...) { chat.grab().save(QString::fromLocal8Bit(argv[1]) + ".failed.png"); throw; }
      chat.grab().save(QString::fromLocal8Bit(argv[1]) + ".chat.png");
      auto buttons = card->findChildren<QPushButton*>();
      for (auto* button : buttons)
        if (button->text() == QStringLiteral("观看直播")) button->click();
      Wait([&] {
        auto* player = chat.findChild<MediaWindow*>();
        return player && !player->CurrentImage().isNull();
      }, "live card did not open player");
      std::cout << "PASS Qt chat: history card -> watch button -> live player\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
