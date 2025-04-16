#ifndef AVATAR_MANAGER_H
#define AVATAR_MANAGER_H

#include <QObject>
#include <QMap>
#include <QPixmap>
#include <QMutex>
#include <QFuture>
#include <QFutureWatcher>
#include "tcpclient.h"

class AvatarManager : public QObject
{
    Q_OBJECT

public:
    static AvatarManager* getInstance();

    // 获取头像，如果未加载则返回默认头像并开始异步加载
    QPixmap getAvatar(int userId);

    // 预加载头像（可选，用于提前加载可能需要的头像）
    void preloadAvatar(int userId);

    // 清除缓存
    void clearCache();

    // 设置TCP客户端
    void setTcpClient(MyTcpClient* client);

signals:
    // 当头像加载完成时发出信号
    void avatarLoaded(int userId);

private slots:
    // 处理从服务器接收到的头像数据
    void handleAvatarData(int userId, const QByteArray& data);

private:
    AvatarManager(QObject* parent = nullptr);
    ~AvatarManager();

    // 请求头像数据
    void requestAvatar(int userId);

    // 处理接收到的头像数据
    void processAvatarData(int userId, const QByteArray& data);

    static AvatarManager* instance;
    QMap<int, QPixmap> avatarCache;
    QPixmap defaultAvatar;
    QSet<int> pendingRequests;
    QMutex mutex;
    MyTcpClient* tcpClient;
};

#endif // AVATAR_MANAGER_H
