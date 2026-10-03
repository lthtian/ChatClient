#include "media_transfer.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>
#include <stdexcept>

#include "public.h"
#include "tcpclient.h"

namespace
{
constexpr qint64 kLimit = 100 * 1024 * 1024;
constexpr qint64 kChunk = 64 * 1024;

QString identifier() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

bool isMp4(const QString& name)
{
    return QFileInfo(name).suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) == 0;
}
}  // namespace

MediaTransfer::MediaTransfer(MyTcpClient* client, QObject* parent) : QObject(parent), client_(client)
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (!root.isEmpty()) directory_ = QDir(root).filePath("ChatClient/videos");
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] { finish(QStringLiteral("文件传输超时")); });
    connect(client_, &MyTcpClient::disconnected, this, &MediaTransfer::connectionLost);
}

MediaTransfer::~MediaTransfer()
{
    if (reply_)
    {
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
    }
}

void MediaTransfer::request(const QString& operation, QJsonObject fields, Completion complete)
{
    if (!client_->getSocket() || client_->getSocket()->state() != QAbstractSocket::ConnectedState)
    {
        complete({}, QStringLiteral("聊天连接已断开"));
        return;
    }
    const QString id = identifier();
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    pending_.insert(id, {std::move(complete), timer});
    connect(timer, &QTimer::timeout, this,
            [this, id]
            {
                if (!pending_.contains(id)) return;
                auto pending = pending_.take(id);
                pending.timer->deleteLater();
                pending.complete({}, QStringLiteral("服务端请求超时"));
            });
    fields["msgid"] = MediaRequest;
    fields["request_id"] = id;
    fields["op"] = operation;
    timer->start(15000);
    client_->sendJson(fields);
}

void MediaTransfer::handleResponse(const QJsonObject& response)
{
    const QString id = response["request_id"].toString();
    if (!pending_.contains(id)) return;
    auto pending = pending_.take(id);
    pending.timer->stop();
    pending.timer->deleteLater();
    const QString error =
        response["ok"].toBool() ? QString() : response["error"].toString(QStringLiteral("服务端拒绝请求"));
    pending.complete(response["data"].toObject(), error);
}

QNetworkRequest MediaTransfer::makeRequest(const QJsonObject& descriptor, const QString& method) const
{
    const QUrl url(descriptor["url"].toString());
    const bool loopback = url.host() == "127.0.0.1" || url.host() == "localhost" || url.host() == "::1";
    // 学习环境只对明确配置的媒体地址允许公网 HTTP，其他地址仍要求 HTTPS。
    const QUrl mediaUrl(qEnvironmentVariable("CHAT_MEDIA_URL", "http://39.105.18.142/media"));
    const bool allowedHttp = url.scheme() == "http" && (loopback || url == mediaUrl);
    const QString authorization = descriptor["headers"].toObject()["Authorization"].toString();
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment() ||
        !(url.scheme() == "https" || allowedHttp) || descriptor["method"].toString() != method ||
        !authorization.startsWith("Bearer ") || authorization.contains('\r') || authorization.contains('\n'))
    {
        throw std::runtime_error("invalid media transfer descriptor");
    }
    QNetworkRequest request(url);
    // 授权头只发送到描述中的地址；不跟随重定向泄露短期凭证。
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", authorization.toLatin1());
    request.setTransferTimeout(30000);
    return request;
}

void MediaTransfer::sendVideo(const QJsonObject& conversation, const QString& path)
{
    if (busy_)
    {
        emit failed(QStringLiteral("请先完成或取消当前文件任务"));
        return;
    }
    source_ = std::make_unique<QFile>(path);
    if (!isMp4(path) || !source_->open(QIODevice::ReadOnly) || source_->size() <= 0 ||
        source_->size() > kLimit)
    {
        source_.reset();
        emit failed(QStringLiteral("请选择可读取的 MP4，大小必须为 1 字节至 100 MiB"));
        return;
    }
    busy_ = true;
    conversation_ = conversation;
    clientMessageId_ = identifier();
    path_ = path;
    total_ = source_->size();
    transferred_ = 0;
    hash_.reset();
    const quint64 generation = ++generation_;
    deadline_.start(30 * 60 * 1000);
    emit phaseChanged(QStringLiteral("计算视频摘要"), true);
    hashNext(generation);
}

void MediaTransfer::hashNext(quint64 generation)
{
    if (generation != generation_ || !busy_) return;
    const QByteArray bytes = source_->read(kChunk);
    if (source_->error() != QFileDevice::NoError)
    {
        finish(source_->errorString());
        return;
    }
    hash_.addData(bytes);
    transferred_ += bytes.size();
    emit progress(transferred_, total_);
    if (transferred_ > total_)
    {
        finish(QStringLiteral("源文件在读取期间发生变化"));
        return;
    }
    if (!source_->atEnd())
    {
        QTimer::singleShot(0, this, [this, generation] { hashNext(generation); });
        return;
    }
    if (transferred_ != total_ || !source_->seek(0))
    {
        finish(QStringLiteral("源文件长度改变或无法重新读取"));
        return;
    }
    expectedHash_ = QString::fromLatin1(hash_.result().toHex());
    beginUpload(generation);
}

void MediaTransfer::beginUpload(quint64 generation)
{
    emit phaseChanged(QStringLiteral("申请视频上传"), true);
    request("begin_file",
            {{"conversation", conversation_},
             {"client_msg_id", clientMessageId_},
             {"bytes", total_},
             {"sha256", expectedHash_},
             {"name", QFileInfo(path_).fileName()}},
            [this, generation](const QJsonObject& data, const QString& error)
            {
                if (generation != generation_)
                {
                    // 申请返回前已取消：回收刚分配的资源，避免继续上传。
                    if (error.isEmpty() && !data["media_id"].toString().isEmpty())
                        request("cancel", {{"media_id", data["media_id"]}},
                                [](const QJsonObject&, const QString&) {});
                    return;
                }
                if (!error.isEmpty())
                {
                    finish(error);
                    return;
                }
                mediaId_ = data["media_id"].toString();
                if (data["state"].toString() == "ready")
                {
                    publish(generation);
                    return;
                }
                try
                {
                    upload(data["upload"].toObject(), generation);
                }
                catch (const std::exception& e)
                {
                    finish(QString::fromUtf8(e.what()));
                }
            });
}

void MediaTransfer::upload(const QJsonObject& descriptor, quint64 generation)
{
    QNetworkRequest request = makeRequest(descriptor, "PUT");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    request.setHeader(QNetworkRequest::ContentLengthHeader, total_);
    request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
    emit phaseChanged(QStringLiteral("上传视频"), true);
    auto* reply = network_.put(request, source_.get());
    reply_ = reply;
    connect(reply, &QNetworkReply::uploadProgress, this,
            [this, generation](qint64 done, qint64 total)
            {
                if (generation == generation_) emit progress(done, total);
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation]
            {
                reply->deleteLater();
                if (generation != generation_) return;
                reply_ = nullptr;
                const QByteArray body = reply->readAll();
                const auto result = QJsonDocument::fromJson(body).object();
                if (reply->error() != QNetworkReply::NoError ||
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
                    !result["ok"].toBool())
                {
                    finish(result["error"].toString(reply->errorString()));
                    return;
                }
                // PUT 成功后仍检查资源状态；只有 ready 才能发布为聊天消息。
                emit phaseChanged(QStringLiteral("确认上传结果"), true);
                this->request("status", {{"media_id", mediaId_}},
                              [this, generation](const QJsonObject& data, const QString& error)
                              {
                                  if (generation != generation_) return;
                                  if (!error.isEmpty())
                                  {
                                      finish(error);
                                      return;
                                  }
                                  if (data["state"].toString() != "ready")
                                  {
                                      finish(QStringLiteral("服务端尚未确认文件可用"));
                                      return;
                                  }
                                  publish(generation);
                              });
            });
}

void MediaTransfer::publish(quint64 generation)
{
    publishing_ = true;
    emit phaseChanged(QStringLiteral("发布视频消息"), false);
    request("publish",
            {{"conversation", conversation_}, {"client_msg_id", clientMessageId_}, {"media_id", mediaId_}},
            [this, generation](const QJsonObject& data, const QString& error)
            {
                if (generation != generation_) return;
                if (!error.isEmpty())
                {
                    finish(error + QStringLiteral("；发布结果可重新打开会话核对"));
                    return;
                }
                const QJsonObject conversation = conversation_;
                const QJsonObject message = data["message"].toObject();
                finish();
                emit messagePublished(conversation, message);
            });
}

QString MediaTransfer::cachedVideoPath(const QJsonObject& media) const
{
    const QString hash = media["sha256"].toString();
    if (directory_.isEmpty() || !QRegularExpression("^[0-9a-f]{64}$").match(hash).hasMatch()) return {};
    const QFileInfo file(QDir(directory_).filePath(hash + ".mp4"));
    return file.isFile() && file.size() > 0 && file.size() == media["bytes"].toVariant().toLongLong()
               ? file.absoluteFilePath()
               : QString();
}

void MediaTransfer::requestPlaybackSource(const QJsonObject& conversation, const QJsonObject& media,
                                          SourceCompletion complete)
{
    const QString id = media["media_id"].toString();
    if (!isMp4(media["name"].toString()) || !QRegularExpression("^[0-9a-f]{32}$").match(id).hasMatch())
    {
        complete({}, QStringLiteral("请选择聊天中的 MP4 视频"));
        return;
    }
    request("read", {{"conversation", conversation}, {"media_id", id}, {"variant", "original"}},
            [this, name = media["name"].toString(), complete = std::move(complete)]
            (const QJsonObject& data, const QString& error)
            {
                if (!error.isEmpty()) { complete({}, error); return; }
                try
                {
                    const auto descriptor = data["download"].toObject();
                    const auto request = makeRequest(descriptor, "GET");
                    MediaSource source;
                    source.kind = MediaSource::Kind::Http;
                    source.name = name;
                    source.location = QString::fromUtf8(request.url().toEncoded());
                    source.headers = "Authorization: " + request.rawHeader("Authorization") + "\r\n";
                    source.expires_at_ms = descriptor["expires_at"].toVariant().toLongLong();
                    if (source.expires_at_ms <= QDateTime::currentMSecsSinceEpoch() + 1000)
                        throw std::runtime_error("playback credential expired");
                    complete(std::move(source), {});
                }
                catch (const std::exception& e) { complete({}, QString::fromUtf8(e.what())); }
            });
}

void MediaTransfer::downloadVideo(const QJsonObject& conversation, const QJsonObject& media)
{
    if (busy_)
    {
        emit failed(QStringLiteral("请先完成或取消当前文件任务"));
        return;
    }
    const QString id = media["media_id"].toString();
    if (!isMp4(media["name"].toString()) || !QRegularExpression("^[0-9a-f]{32}$").match(id).hasMatch())
    {
        emit failed(QStringLiteral("本阶段只支持打开 MP4 文件"));
        return;
    }
    if (directory_.isEmpty() || !QDir().mkpath(directory_))
    {
        emit failed(QStringLiteral("无法创建视频缓存目录"));
        return;
    }
    busy_ = true;
    conversation_ = conversation;
    mediaId_ = id;
    const quint64 generation = ++generation_;
    deadline_.start(30 * 60 * 1000);
    emit phaseChanged(QStringLiteral("申请视频读取权限"), true);
    request("read", {{"conversation", conversation}, {"media_id", id}, {"variant", "original"}},
            [this, generation](const QJsonObject& data, const QString& error)
            {
                if (generation != generation_) return;
                if (!error.isEmpty())
                {
                    finish(error);
                    return;
                }
                try
                {
                    total_ = data["bytes"].toVariant().toLongLong();
                    expectedHash_ = data["sha256"].toString();
                    if (total_ <= 0 || total_ > kLimit ||
                        !QRegularExpression("^[0-9a-f]{64}$").match(expectedHash_).hasMatch())
                        throw std::runtime_error("invalid media length or checksum");
                    // 授权成功后才使用缓存；内容摘要作为文件名，不依赖进程内的路径记录。
                    path_ = QDir(directory_).filePath(expectedHash_ + ".mp4");
                    cached_ = std::make_unique<QFile>(path_);
                    if (cached_->open(QIODevice::ReadOnly) && cached_->size() == total_)
                    {
                        transferred_ = 0;
                        hash_.reset();
                        emit phaseChanged(QStringLiteral("校验本地视频"), true);
                        checkCache(data["download"].toObject(), generation);
                        return;
                    }
                    cached_.reset();
                    startDownload(data["download"].toObject(), generation);
                }
                catch (const std::exception& e)
                {
                    finish(QString::fromUtf8(e.what()));
                }
            });
}

void MediaTransfer::checkCache(const QJsonObject& descriptor, quint64 generation)
{
    if (generation != generation_ || !cached_) return;
    const QByteArray bytes = cached_->read(kChunk);
    hash_.addData(bytes);
    transferred_ += bytes.size();
    emit progress(transferred_, total_);
    if (cached_->error() == QFileDevice::NoError && transferred_ <= total_ && !cached_->atEnd())
    {
        QTimer::singleShot(0, this, [this, descriptor, generation] { checkCache(descriptor, generation); });
        return;
    }
    const bool valid = cached_->error() == QFileDevice::NoError && transferred_ == total_ &&
                       QString::fromLatin1(hash_.result().toHex()) == expectedHash_;
    cached_.reset();
    if (valid)
    {
        const QString path = path_;
        finish();
        emit fileReady(path);
        return;
    }
    startDownload(descriptor, generation);
}

void MediaTransfer::startDownload(const QJsonObject& descriptor, quint64 generation)
{
    try
    {
        destination_ = std::make_unique<QSaveFile>(path_);
        if (!destination_->open(QIODevice::WriteOnly))
        {
            finish(destination_->errorString());
            return;
        }
        transferred_ = 0;
        hash_.reset();
        const QNetworkRequest request = makeRequest(descriptor, "GET");
        emit phaseChanged(QStringLiteral("下载视频，完成后播放"), true);
        auto* reply = network_.get(request);
        reply_ = reply;
        reply->setReadBufferSize(kChunk);
        connect(reply, &QIODevice::readyRead, this,
                [this, reply, generation] { readDownload(reply, generation); });
        connect(
            reply, &QNetworkReply::finished, this,
            [this, reply, generation]
            {
                reply->deleteLater();
                if (generation != generation_) return;
                readDownload(reply, generation);
                if (generation != generation_) return;
                reply_ = nullptr;
                if (reply->error() != QNetworkReply::NoError ||
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
                {
                    finish(reply->errorString());
                    return;
                }
                if (transferred_ != total_ || QString::fromLatin1(hash_.result().toHex()) != expectedHash_)
                {
                    finish(QStringLiteral("下载文件长度或 SHA-256 校验失败"));
                    return;
                }
                if (!destination_->commit())
                {
                    finish(destination_->errorString());
                    return;
                }
                const QString path = path_;
                finish();
                emit fileReady(path);
            });
    }
    catch (const std::exception& e)
    {
        finish(QString::fromUtf8(e.what()));
    }
}

void MediaTransfer::readDownload(QNetworkReply* reply, quint64 generation)
{
    if (generation != generation_ || !destination_) return;
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
    {
        finish(QStringLiteral("媒体下载被拒绝，HTTP %1")
                   .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
        return;
    }
    if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() != total_)
    {
        finish(QStringLiteral("下载响应的文件长度不匹配"));
        return;
    }
    while (reply->bytesAvailable() > 0)
    {
        const QByteArray bytes = reply->read(kChunk);
        if (bytes.isEmpty()) break;
        transferred_ += bytes.size();
        if (transferred_ > total_)
        {
            finish(QStringLiteral("下载超过声明长度"));
            return;
        }
        if (destination_->write(bytes) != bytes.size())
        {
            finish(destination_->errorString());
            return;
        }
        hash_.addData(bytes);
        emit progress(transferred_, total_);
    }
}

void MediaTransfer::finish(const QString& error)
{
    ++generation_;
    deadline_.stop();
    if (reply_)
    {
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    source_.reset();
    cached_.reset();
    destination_.reset();
    mediaId_.clear();
    busy_ = publishing_ = false;
    emit finished();
    if (!error.isEmpty()) emit failed(error);
}

void MediaTransfer::cancel()
{
    if (!busy_ || publishing_) return;
    const QString id = source_ ? mediaId_ : QString();
    finish();
    if (!id.isEmpty()) request("cancel", {{"media_id", id}}, [](const QJsonObject&, const QString&) {});
}

void MediaTransfer::connectionLost()
{
    const auto pending = pending_;
    pending_.clear();
    for (const auto& item : pending)
    {
        item.timer->stop();
        item.timer->deleteLater();
        item.complete({}, QStringLiteral("聊天连接已断开"));
    }
    if (busy_) finish(QStringLiteral("聊天连接已断开"));
}
