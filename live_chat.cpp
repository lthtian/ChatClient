#include "mainwindow.h"
#include "media_transfer.h"
#include "media_window.h"
#include "desktop_broadcast.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QStatusBar>
#include <QTimer>
#include <QUuid>

namespace {
QString StateName(const QString& state) {
    if (state == "live") return QStringLiteral("直播中");
    if (state == "interrupted") return QStringLiteral("推流暂时中断");
    if (state == "ended") return QStringLiteral("已结束");
    return QStringLiteral("等待开播");
}
}

void MainWindow::setupLive()
{
    connect(liveButton, &QPushButton::clicked, this, &MainWindow::createLive);
    connect(tcpclient, &MyTcpClient::disconnected, this, [this] {
        if (broadcast_) broadcast_->SetEnded(QStringLiteral("聊天连接已断开，桌面推流已停止"));
    });
    auto* timer = new QTimer(this);
    timer->setInterval(5000);
    connect(timer, &QTimer::timeout, this, [this] {
        // 推送负责及时更新，轮询负责补齐丢失通知；不为已结束历史卡片持续发请求。
        QSet<QString> queried;
        auto query = [this, &queried](const QString& id, const QJsonObject& conversation) {
            if (id.isEmpty() || queried.contains(id)) return;
            queried.insert(id);
            media_->request("live_status", {{"conversation", conversation}, {"live_id", id}},
                [this, conversation](const QJsonObject& data, const QString& error) {
                    if (error.isEmpty()) handleLiveState(conversation, data["live"].toObject());
                });
        };
        for (auto* card : messageList->findChildren<QFrame*>("liveCard"))
            if (card->property("live_state").toString() != "ended")
                query(card->property("live_id").toString(), currentConversation());
        if (player_ && !watchingLiveId_.isEmpty()) query(watchingLiveId_, watchingConversation_);
    });
    timer->start();
}

void MainWindow::createLive()
{
    const auto conversation = currentConversation();
    if (conversation.isEmpty()) { statusBar()->showMessage(QStringLiteral("请先选择好友或群聊"), 5000); return; }
    openDesktopBroadcast(conversation);
}

void MainWindow::openDesktopBroadcast(const QJsonObject& conversation, const QJsonObject& live)
{
    if (broadcast_ && !broadcast_->Busy() && broadcast_->property("conversation").toMap() != conversation.toVariantMap())
        delete broadcast_.data();
    if (broadcast_) { broadcast_->show(); broadcast_->raise(); broadcast_->activateWindow(); return; }
    auto* dialog = new DesktopBroadcast(this);
    broadcast_ = dialog;
    dialog->setWindowTitle(QStringLiteral("桌面直播 · ") + conversationName(conversation));
    dialog->setProperty("conversation", conversation.toVariantMap());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (!live.isEmpty()) dialog->SetExisting(conversation, live);
    // 一个控制窗口保留创建 ID；超时后重试仍对应同一张服务端直播卡片。
    connect(dialog, &DesktopBroadcast::StartRequested, this,
        [this, conversation, target = QPointer<DesktopBroadcast>(dialog)](const QString& title) {
            if (!target) return;
            const QString id = target->LiveId();
            if (target->property("create_request_id").toString().isEmpty())
                target->setProperty("create_request_id", QUuid::createUuid().toString(QUuid::WithoutBraces));
            const QJsonObject fields = id.isEmpty()
                ? QJsonObject{{"conversation", conversation}, {"title", title}, {"client_msg_id", target->property("create_request_id").toString()}}
                : QJsonObject{{"conversation", conversation}, {"live_id", id}};
            media_->request(id.isEmpty() ? "live_create" : "live_publish", fields,
                [this, conversation, target](const QJsonObject& data, const QString& error) {
                    if (!target) return;
                    if (error.isEmpty() && data.contains("message")) handleMediaMessage(conversation, data["message"].toObject());
                    target->SetAuthorization(conversation, data["live"].toObject(), data["publish"].toObject(), error);
                });
        });
    connect(dialog, &DesktopBroadcast::EndRequested, this, [this, conversation, target = QPointer<DesktopBroadcast>(dialog)] {
        if (!target) return;
        media_->request("live_end", {{"conversation", conversation}, {"live_id", target->LiveId()}},
            [this, conversation, target](const QJsonObject& data, const QString& error) {
                if (!error.isEmpty()) { if (target) target->SetEnded(error); return; }
                handleLiveState(conversation, data["live"].toObject());
                if (target) target->SetEnded({});
            });
    });
    dialog->show();
}

void MainWindow::watchLive(const QJsonObject& conversation, const QJsonObject& live, const QString& format)
{
    auto* player = ensurePlayer();
    watchingLiveId_ = live["live_id"].toString();
    watchingConversation_ = conversation;
    disconnect(player, &MediaWindow::AuthorizationRequested, this, nullptr);
    connect(player, &MediaWindow::AuthorizationRequested, this,
        [this, conversation, live, format](quint64 session, const QString&) {
            const QPointer<MediaWindow> target = player_;
            media_->requestLiveSource(conversation, live, format,
                [target, session](MediaSource source, const QString& error) {
                    if (target) target->SetOnlineSource(session, std::move(source), error);
                });
        });
    player->OpenLive(live["title"].toString(), format == "hls");
}

void MainWindow::handleLiveState(const QJsonObject& conversation, const QJsonObject& live)
{
    const QString id = live["live_id"].toString(), state = live["state"].toString();
    if (id.isEmpty()) return;
    if (broadcast_ && broadcast_->LiveId() == id && state == "ended") broadcast_->SetEnded({});
    if (conversation == currentConversation()) {
        for (auto* card : messageList->findChildren<QFrame*>("liveCard")) {
            if (card->property("live_id").toString() != id) continue;
            card->setProperty("live_state", state);
            card->findChild<QLabel*>("liveState")->setText(StateName(state));
            for (auto* button : card->findChildren<QPushButton*>()) {
                const bool watch = button->property("watch").toBool();
                button->setEnabled(watch ? state == "live" : state != "ended");
            }
        }
    }
    if (player_ && watchingLiveId_ == id && watchingConversation_ == conversation) player_->SetLiveState(state);
}

void MainWindow::appendLiveMessage(const QJsonObject& conversation, const QJsonObject& message)
{
    const auto messageId = message["message_id"].toString();
    if (messageId.isEmpty() || displayedMedia_.contains(messageId)) return;
    displayedMedia_.insert(messageId);
    const auto live = message["live"].toObject();
    auto* row = new QWidget(messageList);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(20, 8, 20, 8);
    auto* card = new QFrame(row);
    card->setObjectName("liveCard");
    card->setProperty("live_id", live["live_id"].toString());
    card->setFixedWidth(390);
    card->setStyleSheet("QFrame#liveCard { background:#f3f7fb; border:1px solid #d6e2ee; border-radius:10px; } QLabel {border:none;background:transparent;}");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 12, 16, 12);
    auto* title = new QLabel(live["title"].toString(), card);
    title->setTextFormat(Qt::PlainText);
    title->setWordWrap(true);
    layout->addWidget(title);
    auto* state = new QLabel(card);
    state->setObjectName("liveState");
    layout->addWidget(state);
    auto* actions = new QHBoxLayout;
    for (const auto& format : {QString("flv"), QString("hls")}) {
        auto* watch = new QPushButton(format == "flv" ? QStringLiteral("观看直播") : QStringLiteral("HLS 观看"), card);
        watch->setProperty("watch", true);
        actions->addWidget(watch);
        connect(watch, &QPushButton::clicked, this, [this, conversation, live, format] { watchLive(conversation, live, format); });
    }
    if (live["owner_id"].toInt() == userid) {
        auto* info = new QPushButton(QStringLiteral("桌面直播"), card);
        auto* end = new QPushButton(QStringLiteral("结束"), card);
        actions->addWidget(info); actions->addWidget(end);
        connect(info, &QPushButton::clicked, this, [this, conversation, live] {
            openDesktopBroadcast(conversation, live);
        });
        connect(end, &QPushButton::clicked, this, [this, conversation, live] {
            if (broadcast_ && broadcast_->LiveId() == live["live_id"].toString()) broadcast_->Stop();
            media_->request("live_end", {{"conversation", conversation}, {"live_id", live["live_id"]}},
                [this, conversation](const QJsonObject& data, const QString& error) {
                    if (!error.isEmpty()) statusBar()->showMessage(error, 10000);
                    else handleLiveState(conversation, data["live"].toObject());
                });
        });
    }
    layout->addLayout(actions);
    const bool outgoing = message["sender_id"].toInt() == userid;
    if (outgoing) rowLayout->addStretch();
    rowLayout->addWidget(card);
    if (!outgoing) rowLayout->addStretch();
    auto* item = new QListWidgetItem(messageList);
    item->setSizeHint(row->sizeHint());
    messageList->setItemWidget(item, row);
    handleLiveState(conversation, live);
}
