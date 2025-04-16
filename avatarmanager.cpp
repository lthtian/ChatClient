#include "avatarmanager.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QtConcurrent/QtConcurrent>
#include <QBuffer>
#include <QPainter>
#include "public.h"

AvatarManager* AvatarManager::instance = nullptr;

AvatarManager* AvatarManager::getInstance()
{
    if (!instance) {
        instance = new AvatarManager();
    }
    return instance;
}

AvatarManager::AvatarManager(QObject* parent) : QObject(parent), tcpClient(nullptr)
{
    // 加载默认头像
    defaultAvatar = QPixmap(":/image/Customer Copy.png");

    // 如果默认头像加载失败，创建一个简单的默认头像
    if (defaultAvatar.isNull()) {
        defaultAvatar = QPixmap(40, 40);
        defaultAvatar.fill(Qt::gray);
    }

    // 确保默认头像是圆形的
    QPixmap rounded = QPixmap(defaultAvatar.size());
    rounded.fill(Qt::transparent);

    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QBrush(defaultAvatar));
    painter.drawEllipse(rounded.rect());

    defaultAvatar = rounded;
}

AvatarManager::~AvatarManager()
{
}

void AvatarManager::setTcpClient(MyTcpClient* client)
{
    tcpClient = client;
}

QPixmap AvatarManager::getAvatar(int userId)
{
    QMutexLocker locker(&mutex);

    // 检查是否已缓存
    if (avatarCache.contains(userId)) {
        return avatarCache[userId];
    }

    // 如果没有缓存且不在请求队列中，开始请求
    if (!pendingRequests.contains(userId)) {
        pendingRequests.insert(userId);

        // 使用QtConcurrent在后台线程请求头像
        QFuture<void> future = QtConcurrent::run(this, &AvatarManager::requestAvatar, userId);
    }

    // 返回默认头像
    return defaultAvatar;
}

void AvatarManager::preloadAvatar(int userId)
{
    // 如果头像已经缓存或正在请求中，则不做任何事
    QMutexLocker locker(&mutex);
    if (avatarCache.contains(userId) || pendingRequests.contains(userId)) {
        return;
    }

    // 添加到请求队列并开始请求
    pendingRequests.insert(userId);
    QFuture<void> future = QtConcurrent::run(this, &AvatarManager::requestAvatar, userId);
}

void AvatarManager::clearCache()
{
    QMutexLocker locker(&mutex);
    avatarCache.clear();
}

void AvatarManager::requestAvatar(int userId)
{
    if (!tcpClient) {
        qDebug() << "TCP客户端未设置，无法请求头像";
        return;
    }

    // 创建请求头像的JSON消息
    QJsonObject jsonObj;
    jsonObj["userid"] = userId;
    jsonObj["msgid"] = imageReq; // 假设imageReq是您定义的消息ID常量
    QJsonDocument jsonDoc(jsonObj);
    QByteArray jsonData = jsonDoc.toJson();

    // 发送请求
    tcpClient->send(jsonData);

    // 注意：响应将通过TCP客户端的readyRead信号处理
    // 您需要在MainWindow的recvHandler中处理imageReqAck消息
    // 并调用AvatarManager::handleAvatarData方法
}

void AvatarManager::handleAvatarData(int userId, const QByteArray& data)
{
    // 在后台线程处理图像数据
    QFuture<void> future = QtConcurrent::run(this, &AvatarManager::processAvatarData, userId, data);
}

void AvatarManager::processAvatarData(int userId, const QByteArray& data)
{
    // 解码图像数据
    QPixmap pixmap;
    if (!pixmap.loadFromData(data)) {
        qDebug() << "无法加载头像数据";
        return;
    }

    // 创建圆形头像
    QPixmap rounded = QPixmap(pixmap.size());
    rounded.fill(Qt::transparent);

    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QBrush(pixmap));
    painter.drawEllipse(rounded.rect());

    // 更新缓存
    {
        QMutexLocker locker(&mutex);
        avatarCache[userId] = rounded;
        pendingRequests.remove(userId);
    }

    // 发出信号通知UI更新
    emit avatarLoaded(userId);
}
