#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProgressDialog>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QUuid>
#include <functional>
#include <iostream>
#include <stdexcept>

#include "mainwindow.h"
#include "media_transfer.h"
#include "media_window.h"

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// 等待真实网络与界面事件；没有替换服务器、设备或传输模块。
void waitFor(const std::function<bool()>& ready, const char* message, int timeout = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeout) QTest::qWait(10);
    require(ready(), message);
}

QJsonObject exchange(MyTcpClient& client, const QJsonObject& request, int expected)
{
    QJsonObject response;
    client.sendJson(request);
    waitFor(
        [&]
        {
            for (QByteArray bytes = client.read(); !bytes.isEmpty(); bytes = client.read())
            {
                auto value = QJsonDocument::fromJson(bytes).object();
                if (value["msgid"].toInt() == expected) response = value;
            }
            return !response.isEmpty();
        },
        "control response timeout");
    require(response["errno"].toInt() == 0, "account operation rejected");
    return response;
}

QByteArray digest(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "cannot open verification file");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), "cannot hash verification file");
    return hash.result();
}
}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);  // 验证只读写专用缓存，不改动用户已下载的文件。
    QApplication::setQuitOnLastWindowClosed(false);
    try
    {
        require(argc == 2 || argc == 3, "usage: video_chat_smoke sample.mp4 [--expiry|--no-audio-device]");
        const bool checkExpiry = argc == 3 && QString::fromLocal8Bit(argv[2]) == "--expiry";
        const bool noAudio = argc == 3 && QString::fromLocal8Bit(argv[2]) == "--no-audio-device";
        // 平台插件已加载；限定本进程后续插件搜索路径，使真实 Qt 音频后端不可用。
        // 不禁用 Windows 设备，也不替换 QAudioDeviceInfo 的返回值。
        if (noAudio) QCoreApplication::setLibraryPaths({});
        const QString sample = QString::fromLocal8Bit(argv[1]);
        const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        const QString names[] = {"av6_" + suffix + "_a", "av6_" + suffix + "_b"};
        const QString password = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QImage avatar(32, 32, QImage::Format_RGB32);
        avatar.fill(Qt::darkCyan);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        avatar.save(&buffer, "PNG");
        MyTcpClient clients[2];
        MainWindow sender(nullptr, &clients[0]), receiver(nullptr, &clients[1]);
        int ids[2]{};
        for (int i = 0; i < 2; ++i)
        {
            const int port = qEnvironmentVariableIntValue("CHAT_PORT");
            clients[i].connectToHost(qEnvironmentVariable("CHAT_HOST", "39.105.18.142"),
                                     port > 0 && port <= 65535 ? port : 7000);
            waitFor([&] { return clients[i].getSocket()->state() == QAbstractSocket::ConnectedState; },
                    "chat endpoint unavailable");
            ids[i] = exchange(clients[i],
                              {{"msgid", 3},
                               {"username", names[i]},
                               {"password", password},
                               {"avatar", QString::fromLatin1(png.toBase64())}},
                              4)["id"]
                         .toInt();
            require(ids[i] > 0, "invalid test account id");
            exchange(clients[i], {{"msgid", 1}, {"username", names[i]}, {"password", password}}, 2);
        }
        exchange(clients[0], {{"msgid", 6}, {"id", ids[0]}, {"friendname", names[1]}}, 13);
        // 只记录本次测试创建的数据编号，不保存口令或短期授权凭证。
        QFile record("stage6-test-data.json");
        require(record.open(QIODevice::WriteOnly), "cannot record test data ids");
        record.write(QJsonDocument(QJsonObject{{"sender", ids[0]},
                                               {"receiver", ids[1]},
                                               {"sender_name", names[0]},
                                               {"receiver_name", names[1]}})
                         .toJson());
        record.close();
        sender.setUser(ids[0], names[0]);
        receiver.setUser(ids[1], names[1]);
        sender.initByTCP();
        receiver.initByTCP();
        auto* left = sender.findChild<MyListWidget*>("contactList");
        auto* right = receiver.findChild<MyListWidget*>("contactList");
        waitFor([&] { return left->count() == 1 && right->count() == 1; }, "friend list missing");
        left->setCurrentRow(0);
        right->setCurrentRow(0);
        sender.show();
        receiver.show();
        auto* sending = sender.findChild<MediaTransfer*>();
        auto* receiving = receiver.findChild<MediaTransfer*>();
        QSignalSpy errorsA(sending, &MediaTransfer::failed), errorsB(receiving, &MediaTransfer::failed);
        QSignalSpy published(sending, &MediaTransfer::messagePublished);
        QSignalSpy downloaded(receiving, &MediaTransfer::fileReady);
        int downloads = 0;
        QObject::connect(receiving, &MediaTransfer::phaseChanged, &app,
                         [&](const QString& text, bool)
                         {
                             if (text == QStringLiteral("下载视频，完成后播放")) ++downloads;
                         });
        // 直接给传输模块选择好的路径；文件选择框不属于协议联调的验证目标。
        sending->sendVideo({{"is_group", false}, {"target", ids[1]}}, sample);
        waitFor([&] { return !published.isEmpty() || !errorsA.isEmpty(); }, "upload timed out", 120000);
        if (!errorsA.isEmpty()) throw std::runtime_error(errorsA.first().first().toString().toStdString());
        QTest::qWait(50);
        require(!sender.findChild<QProgressDialog*>()->isVisible(), "upload dialog stayed open");
        waitFor([&] { return receiver.findChild<QPushButton*>("playVideoButton") != nullptr; },
                "live video message missing");
        // 在线入口经过真实聊天鉴权，FFmpeg 直接读取同一个服务器上的视频。
        const auto videoMedia = published.first()[1].toJsonObject()["media"].toObject();
        const QString previousCache = receiving->cachedVideoPath(videoMedia);
        const auto cacheTime = QFileInfo(previousCache).lastModified();
        QElapsedTimer onlineElapsed;
        onlineElapsed.start();
        receiver.findChild<QPushButton*>("onlineVideoButton")->click();
        auto* online = receiver.findChild<MediaWindow*>();
        require(online && online->IsOnline(), "online input was not selected");
        QSignalSpy authorizations(online, &MediaWindow::AuthorizationRequested);
        waitFor([&] { return online->PositionUs() > 300000 || online->state() == MediaWindow::State::Error; },
                "online first picture timed out", 30000);
        require(online->state() != MediaWindow::State::Error, qPrintable(online->ErrorText()));
        require(!online->CurrentImage().isNull(), "online decoded picture missing");
        if (noAudio) require(!online->HasAudioOutput(), "audio fallback branch was not reached");
        std::cout << "Online startup_ms=" << onlineElapsed.elapsed()
                  << " audio_output=" << online->HasAudioOutput() << std::endl;
        online->TogglePlayback();
        const qint64 pausedAt = online->PositionUs();
        QTest::qWait(150);
        require(online->PositionUs() == pausedAt, "online pause clock moved");
        const qint64 seekAt = std::min<qint64>(20000000, online->DurationUs() / 2);
        online->SeekTo(seekAt);
        waitFor([&] { return online->state() == MediaWindow::State::Paused ||
                            online->state() == MediaWindow::State::Error; }, "online seek timed out", 30000);
        require(online->state() == MediaWindow::State::Paused, qPrintable(online->ErrorText()));
        require(online->PositionUs() == seekAt && !online->CurrentImage().isNull(), "online preview failed");
        require(downloaded.isEmpty() && receiving->cachedVideoPath(videoMedia) == previousCache &&
                    QFileInfo(previousCache).lastModified() == cacheTime, "online playback changed disk cache");
        online->grab().save("stage7-online-player.png");
        if (checkExpiry)
        {
            std::cout << "Waiting for real five-minute credential expiry" << std::endl;
            waitFor([&] { return onlineElapsed.elapsed() >= 302000; }, "expiry wait failed", 305000);
            online->TogglePlayback();
            waitFor([&] { return online->PositionUs() > seekAt + 100000 ||
                                online->state() == MediaWindow::State::Error; }, "credential renewal timed out", 30000);
            require(!authorizations.isEmpty() && online->state() != MediaWindow::State::Error,
                    qPrintable(online->ErrorText()));
            std::cout << "PASS: expired credential renewed at preserved position" << std::endl;
        }
        delete online;
        const QString relayUrl = qEnvironmentVariable("CHAT_TEST_RELAY");
        const QString gatePath = qEnvironmentVariable("CHAT_TEST_GATE");
        if (!relayUrl.isEmpty() && !gatePath.isEmpty())
        {
            MediaWindow streaming;
            QObject::connect(&streaming, &MediaWindow::AuthorizationRequested, &app,
                [&](quint64 session)
                {
                    receiving->requestPlaybackSource({{"is_group", false}, {"target", ids[0]}}, videoMedia, {},
                        [target = QPointer<MediaWindow>(&streaming), relayUrl, session]
                        (MediaSource source, const QString& error)
                        {
                            // 中间节点只转发真实服务响应，暂时停止交付字节来制造断粮。
                            source.location = relayUrl;
                            if (target) target->SetOnlineSource(session, std::move(source), error);
                        });
                });
            streaming.OpenOnline(videoMedia["name"].toString());
            waitFor([&] { return streaming.PositionUs() > 300000 || streaming.state() == MediaWindow::State::Error; },
                    "relay playback startup timed out", 30000);
            require(streaming.state() != MediaWindow::State::Error, qPrintable(streaming.ErrorText()));
            std::cout << "Relay first_frame_ms=" << QDateTime::currentMSecsSinceEpoch() << std::endl;
            QFile gate(gatePath);
            require(gate.open(QIODevice::WriteOnly), "cannot pause byte delivery");
            gate.close();
            waitFor([&] { return streaming.state() == MediaWindow::State::Buffering ||
                                streaming.state() == MediaWindow::State::Error; }, "network did not rebuffer", 8500);
            require(streaming.state() == MediaWindow::State::Buffering, qPrintable(streaming.ErrorText()));
            const qint64 frozen = streaming.PositionUs();
            QTest::qWait(200);
            require(streaming.PositionUs() == frozen, "clock advanced during network starvation");
            require(gate.remove(), "cannot resume byte delivery");
            waitFor([&] { return streaming.state() == MediaWindow::State::Playing ||
                                streaming.state() == MediaWindow::State::Error; }, "network recovery timed out", 15000);
            require(streaming.state() == MediaWindow::State::Playing, qPrintable(streaming.ErrorText()));
            require(gate.open(QIODevice::WriteOnly), "cannot pause byte delivery for seek");
            gate.close();
            streaming.SeekTo(streaming.DurationUs() / 2);
            QTest::qWait(100);
            // 在网络定位尚未完成时切换本地输入，原子代次必须先中断旧读取。
            streaming.OpenFile(sample);
            waitFor([&] { return streaming.PositionUs() > 100000 || streaming.state() == MediaWindow::State::Error; },
                    "switching away from stalled network input blocked", 2500);
            require(streaming.state() != MediaWindow::State::Error, qPrintable(streaming.ErrorText()));
            require(gate.remove(), "cannot clear network gate");
            std::cout << "PASS: real HTTP relay starvation, frozen clock, recovery, canceled seek to local input" << std::endl;
        }
        // 正在打开网络输入时关闭，旧授权回调和阻塞读取不能阻止窗口退出。
        receiver.findChild<QPushButton*>("onlineVideoButton")->click();
        QTest::qWait(30);
        QElapsedTimer closeElapsed;
        closeElapsed.start();
        delete receiver.findChild<MediaWindow*>();
        require(closeElapsed.elapsed() < 2000, "closing online input blocked GUI");
        receiver.findChild<QPushButton*>("playVideoButton")->click();
        waitFor([&] { return !downloaded.isEmpty() || !errorsB.isEmpty(); }, "download timed out", 120000);
        if (!errorsB.isEmpty()) throw std::runtime_error(errorsB.first().first().toString().toStdString());
        require(digest(sample) == digest(downloaded.first().first().toString()), "download bytes differ");
        QTest::qWait(50);
        require(!receiver.findChild<QProgressDialog*>()->isVisible(), "download dialog stayed open");
        const QString cachePath = downloaded.first().first().toString();
        const QDateTime cacheModified = QFileInfo(cachePath).lastModified();
        const int initialDownloads = downloads;
        require(receiver.findChild<QPushButton*>("playVideoButton")->text() == QStringLiteral("播放"),
                "cached action label missing");
        auto* player = receiver.findChild<MediaWindow*>();
        require(player != nullptr && !player->IsOnline(), "local player input missing");
        waitFor([&] { return player->PositionUs() > 300000 || player->state() == MediaWindow::State::Error; },
                "player clock did not advance");
        require(player->state() != MediaWindow::State::Error, qPrintable(player->ErrorText()));
        require(!player->CurrentImage().isNull(), "decoded picture missing");
        player->TogglePlayback();
        player->SeekTo(2000000);
        waitFor([&] { return player->state() == MediaWindow::State::Paused; }, "paused seek failed");
        player->grab().save("stage6-player.png");
        delete player;
        receiver.findChild<QPushButton*>("playVideoButton")->click();
        waitFor([&] { return downloaded.size() == 2 || !errorsB.isEmpty(); }, "cache reuse timed out");
        require(downloaded.size() == 2 && downloaded.last().first().toString() == cachePath &&
                    downloads == initialDownloads && QFileInfo(cachePath).lastModified() == cacheModified,
                "cache was downloaded again");
        delete receiver.findChild<MediaWindow*>();
        {
            MediaTransfer reopened(&clients[1]);
            const QJsonObject media = published.first()[1].toJsonObject()["media"].toObject();
            require(reopened.cachedVideoPath(media) == cachePath, "cache needs in-memory state");
            // 保持长度不变，只破坏内容，验证摘要检查而非仅检查文件存在。
            QFile corrupt(cachePath);
            require(corrupt.open(QIODevice::ReadWrite), "cannot open test cache");
            QByteArray byte = corrupt.read(1);
            require(byte.size() == 1 && corrupt.seek(0), "cannot read test cache");
            byte[0] = static_cast<char>(byte[0] ^ 0xff);
            require(corrupt.write(byte) == 1, "cannot modify test cache");
        }
        receiver.findChild<QPushButton*>("playVideoButton")->click();
        waitFor([&] { return downloaded.size() == 3 || !errorsB.isEmpty(); }, "cache repair timed out",
                120000);
        require(downloaded.size() == 3 && downloads == initialDownloads + 1 &&
                    digest(cachePath) == digest(sample),
                "damaged cache was reused");
        delete receiver.findChild<MediaWindow*>();
        QTest::qWait(50);
        require(!receiver.findChild<QProgressDialog*>()->isVisible(), "cache dialog stayed open");
        receiver.grab().save("stage7-chat.png");
        sender.grab().save("stage6-sender.png");
        receiver.resize(800, 600);
        QTest::qWait(50);
        receiver.grab().save("stage6-chat-small.png");
        // 重新加载会话，验证正式文件消息能从历史恢复且没有重复卡片。
        QMetaObject::invokeMethod(&receiver, "reloadMsgList", Qt::DirectConnection);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        waitFor([&] { return receiver.findChildren<QPushButton*>("playVideoButton").size() == 1; },
                "video history missing or duplicated");
        const int previous = receiver.findChild<QListWidget*>("chatHistory")->count();
        sender.findChild<MyTextEdit*>()->setPlainText("stage6 text check");
        sender.sendMessage();
        waitFor([&] { return receiver.findChild<QListWidget*>("chatHistory")->count() > previous; },
                "text delivery failed");
        require(errorsA.isEmpty() && errorsB.isEmpty(), "unexpected media failure");
        sending->sendVideo({{"is_group", false}, {"target", ids[1]}}, sample);
        sending->cancel();
        QTest::qWait(50);
        require(!sending->busy() && !sender.findChild<QProgressDialog*>()->isVisible(),
                "cancel did not close dialog");
        std::cout << "CachePath: " << cachePath.toStdString() << '\n';
        receiver.close();
        sender.close();
        std::cout << "PASS: real MP4 upload, push, download SHA256, playback, paused seek, history, text, "
                     "dialogs, cache reuse, damaged cache repair, cancel, online playback/seek/no disk cache\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
