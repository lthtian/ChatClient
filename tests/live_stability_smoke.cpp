#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QUuid>
#include <functional>
#include <iostream>
#include <stdexcept>
#include "media_transfer.h"
#include "tcpclient.h"
#include "broadcast/desktop_broadcast.h"
#include "broadcast/desktop_publisher.h"
#include "player/media_window.h"

namespace {
void Require(bool ok, const QString& message) { if (!ok) throw std::runtime_error(message.toStdString()); }
void Wait(const std::function<bool()>& ready, const QString& message) {
    QElapsedTimer clock; clock.start();
    while (!ready() && clock.elapsed() < 30000) QTest::qWait(10);
    Require(ready(), message);
}
QJsonObject ReadJson(const QString& path) {
    QFile file(path); Require(file.open(QIODevice::ReadOnly), "fixture unavailable");
    return QJsonDocument::fromJson(file.readAll()).object();
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& text) {
        std::cerr << "Qt: " << text.toUtf8().constData() << std::endl;
    });
    try {
        Require(argc >= 3, "usage: live_stability_smoke fixture.json publish|watch [flv|hls] [milliseconds]");
        const QString path = QString::fromLocal8Bit(argv[1]);
        const auto fixture = ReadJson(path);
        const bool publish = QString::fromLocal8Bit(argv[2]) == "publish";
        const int duration = argc > 4 ? QString::fromLocal8Bit(argv[4]).toInt() : 90000;
        MyTcpClient client;
        client.connectToHost(fixture["host"].toString(), fixture["port"].toInt());
        Wait([&] { return client.getSocket()->state() == QAbstractSocket::ConnectedState; }, "chat connection timeout");
        client.sendJson({{"msgid", 1}, {"username", fixture[publish ? "owner" : "viewer"]}, {"password", fixture["password"]}});
        QJsonObject login;
        Wait([&] {
            for (auto bytes = client.read(); !bytes.isEmpty(); bytes = client.read()) {
                const auto result = QJsonDocument::fromJson(bytes).object();
                if (result["msgid"].toInt() == 2) login = result;
            }
            return !login.isEmpty();
        }, "login timeout");
        Require(login["errno"].toInt() == 0, "login failed");
        MediaTransfer transfer(&client);
        QObject::connect(client.getSocket(), &QTcpSocket::readyRead, &app, [&] {
            for (auto bytes = client.read(); !bytes.isEmpty(); bytes = client.read())
                transfer.handleResponse(QJsonDocument::fromJson(bytes).object());
        });
        if (publish) {
            DesktopBroadcast dialog;
            dialog.show();
            bool ended = false;
            QString end_error;
            QObject::connect(&dialog, &DesktopBroadcast::EndRequested, &app, [&] {
                transfer.request("live_end", {{"conversation", QJsonObject{{"is_group", false}, {"target", fixture["viewer_id"]}}},
                    {"live_id", dialog.LiveId()}}, [&](const QJsonObject&, const QString& error) {
                        end_error = error; dialog.SetEnded(error); ended = true;
                    });
            });
            dialog.findChild<QCheckBox*>("broadcastSystemAudio")->setChecked(false);
            const QJsonObject conversation{{"is_group", false}, {"target", fixture["viewer_id"]}};
            QObject::connect(&dialog, &DesktopBroadcast::StartRequested, &app, [&](const QString& title) {
                transfer.request("live_create", {{"conversation", conversation}, {"title", title},
                    {"client_msg_id", QUuid::createUuid().toString(QUuid::WithoutBraces)}},
                    [&](const QJsonObject& data, const QString& error) {
                        dialog.SetAuthorization(conversation, data["live"].toObject(), data["publish"].toObject(), error);
                    });
            });
            auto* publisher = dialog.findChild<DesktopPublisher*>();
            QSignalSpy started(publisher, &DesktopPublisher::Publishing), errors(publisher, &DesktopPublisher::Failed);
            dialog.findChild<QPushButton*>("broadcastStart")->click();
            Wait([&] { return !started.isEmpty() || !errors.isEmpty(); }, "publisher startup timeout");
            Require(errors.isEmpty(), errors.isEmpty() ? QString() : errors.first().first().toString());
            QFile marker(path + ".live"); Require(marker.open(QIODevice::WriteOnly), "marker unavailable");
            marker.write(QJsonDocument(QJsonObject{{"live_id", dialog.LiveId()}}).toJson()); marker.close();
            std::cout << "PUBLISH_READY" << std::endl;
            QElapsedTimer clock; clock.start();
            while (clock.elapsed() < duration && !QFileInfo::exists(path + ".stop") && errors.isEmpty()) QTest::qWait(10);
            QFile closing(path + ".closing"); Require(closing.open(QIODevice::WriteOnly), "close marker unavailable"); closing.close();
            QElapsedTimer close_clock; close_clock.start();
            dialog.close(); QTest::qWait(100);
            const bool hidden = !dialog.isVisible();
            std::cout << "CLOSE: visible_after_100ms=" << dialog.isVisible() << std::endl;
            Wait([&] { return ended && !publisher->isRunning(); }, "end timeout");
            std::cout << "CLOSE: cleanup_ms=" << close_clock.elapsed() << std::endl;
            Require(end_error.isEmpty() && errors.isEmpty(), "publisher or end failed");
            Require(hidden, "control window waited for service response before disappearing");
            std::cout << "PASS publisher stopped and service ended" << std::endl;
        } else {
            const auto live = ReadJson(path + ".live");
            const auto conversation = QJsonObject{{"is_group", false}, {"target", fixture["owner_id"].toInt(29)}};
            const QString format = argc > 3 ? QString::fromLocal8Bit(argv[3]) : "flv";
            MediaWindow player;
            player.show();
            int authorizations = 0;
            QObject::connect(&player, &MediaWindow::AuthorizationRequested, &app, [&](quint64 session, const QString&) {
                ++authorizations;
                transfer.requestLiveSource(conversation, live, format, [&, session](MediaSource source, const QString& error) {
                    player.SetOnlineSource(session, std::move(source), error);
                });
            });
            QSignalSpy frames(&player, &MediaWindow::FramePresented);
            player.OpenLive("live stability", format == "hls");
            QElapsedTimer clock; clock.start();
            qint64 playing_ms = 0, previous_ms = 0, last_frame_ms = 0;
            QObject::connect(&player, &MediaWindow::FramePresented, &app, [&](qint64) { last_frame_ms = clock.elapsed(); });
            QString last_error;
            while (clock.elapsed() < duration && authorizations == 1 && player.state() != MediaWindow::State::Error) {
                QTest::qWait(10);
                const qint64 now = clock.elapsed();
                if (player.state() == MediaWindow::State::Playing) playing_ms += now - previous_ms;
                previous_ms = now;
                if (!player.ErrorText().isEmpty()) { last_error = player.ErrorText(); break; }
            }
            std::cout << "WATCH: ms=" << clock.elapsed() << " frames=" << frames.count()
                      << " authorizations=" << authorizations << " state=" << int(player.state())
                      << " playing_ms=" << playing_ms << " last_frame_age_ms=" << clock.elapsed() - last_frame_ms
                      << " error=" << last_error.toUtf8().constData() << std::endl;
            Require(last_error.isEmpty() && authorizations == 1 && frames.count() > duration / 100 &&
                    playing_ms >= duration * 3 / 4 && clock.elapsed() - last_frame_ms < 3000,
                    "continuous playback interrupted: " + last_error);
            player.SetLiveState("ended");
            std::cout << "PASS continuous " << format.toStdString() << std::endl;
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << std::endl; return 1; }
}
