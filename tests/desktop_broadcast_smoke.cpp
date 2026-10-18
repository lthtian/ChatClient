#include <QApplication>
#include <QAudioDeviceInfo>
#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QScreen>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTest>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include "mainwindow.h"
#include "media_transfer.h"
#include "broadcast/desktop_broadcast.h"
#include "broadcast/desktop_publisher.h"
#include "player/media_window.h"

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& predicate, const char* message, int timeout = 30000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) QTest::qWait(10);
    Require(predicate(), message);
}
QJsonObject Login(MyTcpClient& client, const QJsonObject& fixture, const QString& name) {
    client.connectToHost(fixture["host"].toString(), fixture["port"].toInt());
    Wait([&] { return client.getSocket()->state() == QAbstractSocket::ConnectedState; }, "chat connection failed");
    client.sendJson({{"msgid", 1}, {"username", name}, {"password", fixture["password"]}});
    QJsonObject response;
    Wait([&] {
        for (auto bytes = client.read(); !bytes.isEmpty(); bytes = client.read()) {
            const auto json = QJsonDocument::fromJson(bytes).object();
            if (json["msgid"].toInt() == 2) response = json;
        }
        return !response.isEmpty();
    }, "login timeout");
    Require(response["errno"].toInt() == 0, "login rejected");
    return response;
}

// 真实输出端点播放连续正弦，验证 loopback 捕获后的网络音频，而非模拟采集接口。
class Tone : public QIODevice {
public:
    Tone() { open(ReadOnly); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 192000 + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char* data, qint64 size) override {
        const qint64 count = size / 4;
        for (qint64 i = 0; i < count; ++i) {
            const qint16 sample = qint16(std::sin(double(position_++) * 2 * 3.141592653589793 * 737 / 48000) * 7000);
            std::memcpy(data + i * 4, &sample, 2); std::memcpy(data + i * 4 + 2, &sample, 2);
        }
        return count * 4;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    qint64 position_ = 0; // 每声道已产生的真实播放采样数。
};

double RecordRms(const MediaSource& source) {
    QProcess process;
    process.setProgram("D:/chat/_deps/ffmpeg-8.1.1-full_build-shared/bin/ffmpeg.exe");
    process.setArguments({"-v", "error", "-rw_timeout", "8000000", "-headers", QString::fromUtf8(source.headers),
        "-i", source.location, "-t", "2", "-vn", "-ac", "2", "-ar", "48000", "-f", "f32le", "pipe:1"});
    process.start();
    Wait([&] { return process.state() == QProcess::NotRunning; }, "audio receiver timeout");
    const auto error = process.readAllStandardError();
    Require(process.exitCode() == 0 && error.isEmpty(), "audio receiver reported an error");
    const auto pcm = process.readAllStandardOutput();
    Require(pcm.size() > 48000 * 4, "received too little PCM");
    double energy = 0;
    for (int offset = 0; offset + 4 <= pcm.size(); offset += 4) {
        float sample; std::memcpy(&sample, pcm.constData() + offset, 4);
        energy += double(sample) * sample;
    }
    return std::sqrt(energy / (pcm.size() / 4));
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& text) {
        std::cerr << "Qt: " << text.toUtf8().constData() << '\n';
    });
    try {
        Require(argc == 2, "usage: desktop_broadcast_smoke fixture.json");
        QFile file(QString::fromLocal8Bit(argv[1]));
        Require(file.open(QIODevice::ReadOnly), "fixture unavailable");
        const auto fixture = QJsonDocument::fromJson(file.readAll()).object();
        MyTcpClient owner, viewer;
        const auto login = Login(owner, fixture, fixture["owner"].toString());
        Login(viewer, fixture, fixture["viewer"].toString());
        MainWindow chat(nullptr, &owner);
        chat.setUser(login["id"].toInt(), fixture["owner"].toString());
        QObject::connect(owner.getSocket(), &QTcpSocket::readyRead, &chat, &MainWindow::recvHandler);
        QObject::connect(chat.statusBar(), &QStatusBar::messageChanged, &app, [](const QString& text) {
            if (!text.isEmpty()) std::cerr << "chat: " << text.toUtf8().constData() << '\n';
        });
        chat.initByTCP(); chat.show();
        auto* contacts = chat.findChild<MyListWidget*>("contactList");
        Wait([&] { return contacts && contacts->count() > 0; }, "contacts unavailable");
        for (int i = 0; i < contacts->count(); ++i) {
            const auto found = chat._list.find(contacts->item(i)->text());
            if (found != chat._list.end() && found->second.first == fixture["viewer_id"].toInt() && !found->second.second)
                contacts->setCurrentRow(i);
        }
        Require(contacts->currentItem(), "test conversation unavailable");
        chat.findChild<QPushButton*>("createLiveButton")->click();
        auto* dialog = chat.findChild<DesktopBroadcast*>();
        Require(dialog, "desktop control did not open");
        auto* screens = dialog->findChild<QComboBox*>("broadcastScreen");
        Require(screens->currentIndex() == QGuiApplication::screens().indexOf(QGuiApplication::primaryScreen()), "primary screen not selected");
        dialog->findChild<QLineEdit*>("broadcastTitle")->setText("Desktop integration verification");
        auto* system = dialog->findChild<QCheckBox*>("broadcastSystemAudio");
        auto* mic = dialog->findChild<QCheckBox*>("broadcastMicrophone");
        auto* publisher = dialog->findChild<DesktopPublisher*>();
        QSignalSpy errors(publisher, &DesktopPublisher::Failed), unavailable(publisher, &DesktopPublisher::AudioUnavailable);
        QObject::connect(publisher, &DesktopPublisher::AudioUnavailable, &app, [](bool system, const QString& error) {
            std::cerr << "audio endpoint unavailable (" << (system ? "system" : "microphone") << "): " << error.toUtf8().constData() << '\n';
        });
        QSignalSpy publishing(publisher, &DesktopPublisher::Publishing), stats(publisher, &DesktopPublisher::Statistics);
        dialog->findChild<QPushButton*>("broadcastStart")->click();
        Wait([&] { return !publishing.isEmpty() || !errors.isEmpty(); }, "desktop publisher startup timeout");
        if (!errors.isEmpty()) throw std::runtime_error(errors.first().first().toString().toStdString());
        const QString liveId = dialog->LiveId();
        Require(liveId.size() == 32, "live creation did not bind the publisher");
        MediaTransfer transfer(&viewer);
        QObject::connect(viewer.getSocket(), &QTcpSocket::readyRead, &app, [&] {
            for (auto bytes = viewer.read(); !bytes.isEmpty(); bytes = viewer.read()) transfer.handleResponse(QJsonDocument::fromJson(bytes).object());
        });
        const QJsonObject conversation{{"is_group", false}, {"target", login["id"]}};
        const QJsonObject live{{"live_id", liveId}};
        auto source = [&](const QString& format) {
            bool done = false; MediaSource result; QString error;
            transfer.requestLiveSource(conversation, live, format, [&](MediaSource value, const QString& message) {
                result = std::move(value); error = message; done = true;
            });
            Wait([&] { return done; }, "view authorization timeout");
            Require(error.isEmpty(), "view authorization rejected"); return result;
        };
        for (const auto& format : {QString("flv"), QString("hls")}) {
            if (format == "hls") QTest::qWait(4500);
            MediaWindow player;
            QSignalSpy frames(&player, &MediaWindow::FramePresented);
            QObject::connect(&player, &MediaWindow::AuthorizationRequested, &app, [&](quint64 session, const QString&) {
                player.SetOnlineSource(session, source(format), {});
            });
            player.OpenLive("desktop-test", format == "hls");
            Wait([&] { return frames.count() >= 10 || player.state() == MediaWindow::State::Error; }, "desktop viewer timeout");
            if (player.state() == MediaWindow::State::Error) throw std::runtime_error(player.ErrorText().toStdString());
            Require(player.CurrentImage().size() == QSize(1280, 720), "encoded dimensions incorrect");
            player.SetLiveState("ended");
            std::cout << "PASS desktop -> RTMP -> " << format.toStdString() << " -> Qt: " << frames.count() << " frames\n";
        }
        Tone tone;
        QAudioFormat pcm;
        pcm.setCodec("audio/pcm"); pcm.setSampleRate(48000); pcm.setChannelCount(2);
        pcm.setSampleSize(16); pcm.setSampleType(QAudioFormat::SignedInt); pcm.setByteOrder(QAudioFormat::LittleEndian);
        Require(QAudioDeviceInfo::defaultOutputDevice().isFormatSupported(pcm), "test tone format unsupported");
        QAudioOutput audio(pcm); audio.setVolume(0.35); audio.start(&tone);
        QTest::qWait(500);
        const auto input = source("flv");
        const double audible = RecordRms(input);
        Require(audible > 0.001, "system loopback did not capture played sound");
        system->setChecked(false);
        QTest::qWait(2500);
        const double silent = RecordRms(input);
        Require(silent < 0.0001, "system audio switch did not mute published audio");
        audio.stop();
        mic->setChecked(true); QTest::qWait(800); mic->setChecked(false);
        Require(errors.isEmpty(), "publisher failed while toggling audio");
        Require(!stats.isEmpty(), "publisher statistics unavailable");
        dialog->grab().save(QString::fromLocal8Bit(argv[1]) + ".desktop.png");
        std::cout << "PASS audio: system RMS=" << audible << ", muted RMS=" << silent
                  << ", unavailable endpoints=" << unavailable.count() << '\n';
        dialog->findChild<QPushButton*>("broadcastEnd")->click();
        Wait([&] { return dialog->LiveId().isEmpty() && !publisher->isRunning(); }, "end did not release publisher");
        Require(dialog->findChild<QPushButton*>("broadcastStart")->isEnabled(), "control did not return to idle");
        dialog->findChild<QPushButton*>("broadcastStart")->click();
        Wait([&] { return publishing.count() >= 2 || !errors.isEmpty(); }, "second broadcast did not start");
        Require(dialog->LiveId() != liveId && errors.isEmpty(), "second broadcast reused an ended session");
        QPointer<DesktopBroadcast> closed(dialog);
        dialog->close();
        Wait([&] { return closed.isNull(); }, "closing publisher did not end the service session");
        std::cout << "PASS lifecycle: end -> fresh create -> close -> service end\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
