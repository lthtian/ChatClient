#include <QCloseEvent>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QProgressDialog>
#include <QStatusBar>
#include <algorithm>

#include "mainwindow.h"
#include "media_transfer.h"
#include "media_window.h"
#include "public.h"

void MainWindow::setupMedia()
{
    media_ = new MediaTransfer(tcpclient, this);
    transferProgress_ = new QProgressDialog(this);
    transferProgress_->setWindowTitle(QStringLiteral("视频文件传输"));
    transferProgress_->setAutoClose(true);
    transferProgress_->setAutoReset(false);
    transferProgress_->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    transferProgress_->setMinimumWidth(360);
    transferProgress_->setMinimumDuration(0);
    transferProgress_->reset();
    connect(videoButton, &QPushButton::clicked, this, &MainWindow::sendVideo);
    connect(transferProgress_, &QProgressDialog::canceled, media_, &MediaTransfer::cancel);
    connect(media_, &MediaTransfer::phaseChanged, this,
            [this](const QString& text, bool cancellable)
            {
                transferProgress_->setLabelText(text);
                transferProgress_->setRange(0, 0);
                transferProgress_->setCancelButtonText(cancellable ? QStringLiteral("取消") : QString());
                transferProgress_->show();
                videoButton->setEnabled(false);
                for (auto* button : messageList->findChildren<QPushButton*>("playVideoButton"))
                    button->setEnabled(false);
            });
    connect(
        media_, &MediaTransfer::progress, this,
        [this](qint64 done, qint64 total)
        {
            if (total <= 0) return;
            transferProgress_->setRange(0, 1000);
            transferProgress_->setValue(static_cast<int>(std::clamp<qint64>(done * 1000 / total, 0, 1000)));
        });
    connect(media_, &MediaTransfer::finished, this,
            [this]
            {
                transferProgress_->reset();
                videoButton->setEnabled(true);
                for (auto* button : messageList->findChildren<QPushButton*>("playVideoButton"))
                {
                    const QJsonObject media = QJsonObject::fromVariantMap(button->property("media").toMap());
                    if (QFileInfo(media["name"].toString()).suffix().compare("mp4", Qt::CaseInsensitive) != 0)
                        continue;
                    const QString cached = media_->cachedVideoPath(media);
                    button->setEnabled(true);
                    button->setText(cached.isEmpty() ? QStringLiteral("下载并播放") : QStringLiteral("播放"));
                    button->setToolTip(cached.isEmpty() ? QStringLiteral("下载完整视频后播放") : cached);
                }
            });
    connect(media_, &MediaTransfer::failed, this, [this](const QString& error)
            { statusBar()->showMessage(QStringLiteral("视频操作失败：") + error, 15000); });
    connect(media_, &MediaTransfer::messagePublished, this, &MainWindow::handleMediaMessage);
    connect(media_, &MediaTransfer::fileReady, this,
            [this](const QString& path)
            {
                // 本地入口使用通过完整下载和摘要校验的文件路径。
                ensurePlayer()->OpenFile(path);
            });
}

MediaWindow* MainWindow::ensurePlayer()
{
    if (!player_)
    {
        player_ = new MediaWindow;
        player_->setParent(this, Qt::Window);
        player_->setAttribute(Qt::WA_DeleteOnClose);
    }
    player_->show();
    player_->raise();
    return player_;
}

void MainWindow::playOnlineVideo(const QJsonObject& conversation, const QJsonObject& media)
{
    auto* player = ensurePlayer();
    // 切换视频时替换消息绑定；播放器只发出授权请求，不认识 media_id 或聊天连接。
    disconnect(player, &MediaWindow::AuthorizationRequested, this, nullptr);
    connect(player, &MediaWindow::AuthorizationRequested, this,
            [this, conversation, media](quint64 session, const QString& rendition)
            {
                const QPointer<MediaWindow> target = player_;
                media_->requestPlaybackSource(conversation, media, rendition,
                    [target, session](MediaSource source, const QString& error)
                    {
                        // 关闭窗口和更换视频期间，旧授权返回不能打开错误的视频。
                        if (target) target->SetOnlineSource(session, std::move(source), error);
                    });
            });
    player->OpenOnline(media["name"].toString());
}

QJsonObject MainWindow::currentConversation() const
{
    if (!contactList->currentItem()) return {};
    const auto found = _list.find(contactList->currentItem()->text());
    if (found == _list.end()) return {};
    return {{"is_group", found->second.second}, {"target", found->second.first}};
}

QString MainWindow::conversationName(const QJsonObject& conversation) const
{
    for (const auto& entry : _list)
    {
        if (entry.second.first == conversation["target"].toInt() &&
            entry.second.second == conversation["is_group"].toBool())
            return entry.first;
    }
    return {};
}

void MainWindow::sendVideo()
{
    const QJsonObject conversation = currentConversation();
    if (conversation.isEmpty())
    {
        statusBar()->showMessage(QStringLiteral("请先选择好友或群聊"), 5000);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("发送视频"), QString(),
                                                      QStringLiteral("MP4 视频 (*.mp4 *.MP4)"));
    if (!path.isEmpty()) media_->sendVideo(conversation, path);
}

void MainWindow::requestMediaHistory(const QJsonObject& conversation, const QString& before,
                                     quint64 generation)
{
    QJsonObject fields{{"conversation", conversation}, {"limit", 100}};
    if (!before.isEmpty()) fields["before_id"] = before.toInt();
    media_->request("history", fields,
                    [this, conversation, generation](const QJsonObject& data, const QString& error)
                    {
                        if (generation != historyGeneration_ || conversation != currentConversation()) return;
                        if (!error.isEmpty())
                        {
                            statusBar()->showMessage(QStringLiteral("读取历史失败：") + error, 10000);
                            showMediaHistory(conversation);
                            return;
                        }
                        // 服务端每页按时间正序返回，继续向前翻页时把较早的一页放在前面。
                        QJsonArray records = data["messages"].toArray();
                        for (const auto& message : mediaHistory_) records.append(message);
                        mediaHistory_ = records;
                        const QString next = data["next_cursor"].toString();
                        if (!next.isEmpty())
                            requestMediaHistory(conversation, next, generation);
                        else
                            showMediaHistory(conversation);
                    });
}

void MainWindow::showMediaHistory(const QJsonObject& conversation)
{
    QJsonArray history;
    for (const auto& value : mediaHistory_)
    {
        const QJsonObject message = value.toObject();
        const QString kind = message["kind"].toString();
        QJsonObject row{
            {"id", QString::number(message["sender_id"].toInt())},
            {"name", message["sender_name"]},
            {"kind", kind},
            {"record", message},
            {"time", QDateTime::fromMSecsSinceEpoch(message["time"].toVariant().toLongLong())
                         .toString("yyyy-MM-dd HH:mm:ss")},
            {"message", kind == "image" ? QJsonValue(QStringLiteral("[图片消息]")) : message["text"]}};
        history.append(QString::fromUtf8(QJsonDocument(row).toJson(QJsonDocument::Compact)));
    }
    QJsonObject response{{"history", history},
                         {"isgroup", conversation["is_group"]},
                         {"id1", userid},
                         {"id2", conversation["target"]},
                         {"groupid", conversation["target"]}};
    handleHistoryMsgAck(response);
    historyLoading_ = false;
    const QJsonArray pending = pendingMedia_;
    pendingMedia_ = {};
    for (const auto& message : pending) handleMediaMessage(conversation, message.toObject());
}

void MainWindow::handleMediaMessage(const QJsonObject& conversation, const QJsonObject& message)
{
    const QString name = conversationName(conversation);
    if (name.isEmpty()) return;
    if (conversation != currentConversation())
    {
        // 只有来自他人的实时消息需要增加服务端未读；自己的发送确认不增加。
        if (message["sender_id"].toInt() != userid)
        {
            const auto item = mp.find(name);
            if (item != mp.end())
                contactList->setItemValue(item->second, contactList->getItemValue(item->second) + 1);
            tcpclient->sendJson({{"msgid", addNewMsgCnt},
                                 {"userid", userid},
                                 {"sender", conversation["target"]},
                                 {"isgroup", conversation["is_group"]}});
        }
        return;
    }
    if (historyLoading_ && message["kind"].toString() == "file")
    {
        pendingMedia_.append(message);
        return;
    }
    if (message["kind"].toString() == "file")
        appendVideoMessage(conversation, message);
    else
    {
        const bool group = conversation["is_group"].toBool();
        QJsonObject text{
            {"sender", name},
            {"groupname", name},
            {"sendername", message["sender_name"]},
            {"message", message["kind"].toString() == "image" ? QJsonValue(QStringLiteral("[图片消息]"))
                                                              : message["text"]}};
        handleChatMsg(group ? GroupChatMsg : OTOMsg, text);
    }
    if (messageList->count() > 0) messageList->scrollToBottom();
}

void MainWindow::appendVideoMessage(const QJsonObject& conversation, const QJsonObject& message)
{
    const QString id = message["message_id"].toString();
    if (id.isEmpty() || displayedMedia_.contains(id)) return;
    displayedMedia_.insert(id);
    const QJsonObject media = message["media"].toObject();
    const QString name = media["name"].toString();
    const bool outgoing = message["sender_id"].toInt() == userid;
    auto* row = new QWidget(messageList);
    row->setObjectName("videoMessage_" + id);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(20, 8, 20, 8);
    auto* card = new QFrame(row);
    card->setObjectName("videoCard");
    card->setFixedWidth(380);
    card->setStyleSheet(
        QStringLiteral("QFrame#videoCard { background: %1; border: 1px solid %2; border-radius: 10px; }"
                       "QLabel { background: transparent; border: none; color: #25332e; font-size: 14px; }"
                       "QLabel#videoSender, QLabel#videoDetails { color: #738079; font-size: 12px; }")
            .arg(outgoing ? "#edf9f1" : "#f6f8fa", outgoing ? "#cfe8d7" : "#e2e7eb"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);
    const QString sender = outgoing ? QStringLiteral("我") : message["sender_name"].toString();
    auto* senderLabel = new QLabel(sender, card);
    senderLabel->setTextFormat(Qt::PlainText);
    senderLabel->setObjectName("videoSender");
    layout->addWidget(senderLabel);
    auto* label = new QLabel(card);
    label->setText(label->fontMetrics().elidedText(name, Qt::ElideMiddle, 346));
    label->setTextFormat(Qt::PlainText);
    label->setToolTip(name);
    layout->addWidget(label);
    const bool mp4 = QFileInfo(name).suffix().compare("mp4", Qt::CaseInsensitive) == 0;
    auto* footer = new QHBoxLayout;
    auto* details = new QLabel(QStringLiteral("%1 · %2 MiB")
                                   .arg(mp4 ? QStringLiteral("MP4 视频") : QStringLiteral("文件"))
                                   .arg(media["bytes"].toDouble() / (1024 * 1024), 0, 'f', 2),
                               card);
    details->setObjectName("videoDetails");
    layout->addWidget(details);
    footer->addStretch();
    auto* online = new QPushButton(QStringLiteral("在线播放"), card);
    online->setObjectName("onlineVideoButton");
    online->setFixedHeight(32);
    online->setEnabled(mp4);
    online->setToolTip(QStringLiteral("边接收边播放，不保存本地视频文件"));
    footer->addWidget(online);
    connect(online, &QPushButton::clicked, this,
            [this, conversation, media] { playOnlineVideo(conversation, media); });
    const QString cached = media_->cachedVideoPath(media);
    auto* open =
        new QPushButton(mp4 ? (cached.isEmpty() ? QStringLiteral("下载并播放") : QStringLiteral("播放"))
                            : QStringLiteral("暂不支持播放"),
                        card);
    open->setObjectName("playVideoButton");
    open->setProperty("media", media.toVariantMap());
    open->setFixedHeight(32);
    open->setToolTip(cached.isEmpty() ? QStringLiteral("仅支持 MP4，完整下载后播放") : cached);
    open->setEnabled(mp4 && !media_->busy());
    footer->addWidget(open);
    layout->addLayout(footer);
    // 发出的文件靠右，收到的文件靠左；卡片内部的文件名与操作按钮保持相邻。
    if (outgoing) rowLayout->addStretch();
    rowLayout->addWidget(card);
    if (!outgoing) rowLayout->addStretch();
    connect(open, &QPushButton::clicked, this,
            [this, conversation, media] { media_->downloadVideo(conversation, media); });
    auto* item = new QListWidgetItem;
    item->setSizeHint(QSize(0, 170));
    messageList->addItem(item);
    messageList->setItemWidget(item, row);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // 关闭播放和未完成的传输；已校验的缓存文件继续保留。
    if (player_) delete player_.data();
    media_->cancel();
    tcpclient->close();
    QMainWindow::closeEvent(event);
}
