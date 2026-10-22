#include "rtc_connection.h"
#include "camera_window.h"
#include "video_view.h"
#include <QApplication>
#include <QAudioDeviceInfo>
#include <QCameraInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <iostream>

// 两个独立进程经过真实业务服务与 SRS 收发；样本画面进入同一编码器，不模拟网络。
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QStringList args = app.arguments();
    if (args.size() == 2 && args[1] == "devices") {
        QJsonArray cameras;
        for (const auto& camera : QCameraInfo::availableCameras()) cameras.append(camera.description());
        std::cout << QJsonDocument(QJsonObject{{"cameras", cameras},
            {"audio_outputs", QAudioDeviceInfo::availableDevices(QAudio::AudioOutput).size()}}).toJson().toStdString();
        return 0;
    }
    if (args.size() < 3) return 2;
    QFile file(args[2]);
    if (!file.open(QIODevice::ReadOnly)) return 2;
    const auto input = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const bool publishing = args[1] == "publish";
    bool failed = false;
    int video = 0, audio = 0;
    qint64 previous_video = -1, previous_audio = -1;
    const int duration = input["duration_ms"].toInt(20000);
    if (args[1] == "window") {
        auto* window = new CameraWindow(false);
        window->SetExisting(input["live"].toObject());
        QObject::connect(window, &CameraWindow::FramePresented, &app, [&](qint64 pts) {
            if (pts < previous_video) failed = true;
            previous_video = pts; ++video;
        });
        QObject::connect(window, &QObject::destroyed, &app, [&] { app.exit(!failed && video >= 100 ? 0 : 1); });
        window->show(); window->SetAuthorization(input["live"].toObject(), input["descriptor"].toObject(), {});
        QObject::connect(window->findChild<RtcConnection*>(), &RtcConnection::Failed, &app, [&](const QString& error) {
            failed = true; std::cerr << "FAIL " << error.toUtf8().constData() << std::endl;
        });
        QTimer::singleShot(duration, window, [window] { window->grab().save("rtc-window.png"); window->close(); });
        QTimer::singleShot(duration + 5000, &app, [&] { app.exit(1); });
        const int result = app.exec();
        std::cout << "window frames=" << video << " result=" << result << std::endl;
        return result;
    }
    RtcConnection connection(input["descriptor"].toObject(), publishing, 48000);
    QObject::connect(&connection, &RtcConnection::Status, &app, [](const QString& message) {
        std::cout << message.toUtf8().constData() << std::endl;
    });
    QObject::connect(&connection, &RtcConnection::Failed, &app, [&](const QString& error) {
        failed = true; std::cerr << "FAIL " << error.toUtf8().constData() << std::endl;
    });
    QObject::connect(&connection, &RtcConnection::Connected, &app, [] { std::cout << "CONNECTED" << std::endl; });
    QObject::connect(&connection, &QThread::finished, &app, &QCoreApplication::quit);
    QImage image(input["image"].toString());
    if (publishing && image.isNull()) return 2;
    QTimer timer;
    timer.setInterval(publishing ? 33 : 10);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (QFile::exists(args[2] + ".stop")) { connection.Stop(); return; }
        if (publishing) connection.SetPicture(image);
        else {
            auto batch = connection.TakeMedia();
            for (const auto& picture : batch.video) {
                if (picture.pts_us < previous_video || picture.image.isNull()) failed = true;
                previous_video = picture.pts_us; ++video;
            }
            for (const auto& block : batch.audio) {
                if (block.pts_us < previous_audio || block.pcm.isEmpty()) failed = true;
                previous_audio = block.pts_us; ++audio;
            }
        }
    });
    timer.start();
    QTimer::singleShot(duration, &connection, &RtcConnection::Stop);
    connection.start();
    app.exec(); connection.wait();
    std::cout << "video=" << video << " audio=" << audio << " av_delta_us=" << (previous_video - previous_audio) << std::endl;
    return !failed && (publishing || (video >= 100 && audio >= 100 && std::abs(previous_video - previous_audio) < 250000)) ? 0 : 1;
}
