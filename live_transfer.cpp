#include "media_transfer.h"
#include <QDateTime>
#include <QRegularExpression>
#include <stdexcept>

void MediaTransfer::requestLiveSource(const QJsonObject& conversation, const QJsonObject& live,
                                      const QString& format, SourceCompletion complete)
{
    const QString id = live["live_id"].toString();
    if (!QRegularExpression("^[0-9a-f]{32}$").match(id).hasMatch() || (format != "flv" && format != "hls"))
    {
        complete({}, QStringLiteral("无效的直播入口"));
        return;
    }
    request("live_read", {{"conversation", conversation}, {"live_id", id}, {"format", format}},
            [this, format, name = live["title"].toString(), complete = std::move(complete)]
            (const QJsonObject& data, const QString& error)
            {
                if (!error.isEmpty()) { complete({}, error); return; }
                try
                {
                    const auto descriptor = data["download"].toObject();
                    const auto request = makeRequest(descriptor, "GET");
                    MediaSource source;
                    source.kind = format == "hls" ? MediaSource::Kind::LiveHls : MediaSource::Kind::LiveFlv;
                    source.name = name;
                    source.location = QString::fromUtf8(request.url().toEncoded());
                    source.headers = "Authorization: " + request.rawHeader("Authorization") + "\r\n";
                    source.expires_at_ms = descriptor["expires_at"].toVariant().toLongLong();
                    if (source.expires_at_ms <= QDateTime::currentMSecsSinceEpoch() + 1000)
                        throw std::runtime_error("直播凭证已过期");
                    complete(std::move(source), {});
                }
                catch (const std::exception& failure) { complete({}, QString::fromUtf8(failure.what())); }
            });
}
