#ifndef MEDIA_TRANSFER_H
#define MEDIA_TRANSFER_H

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>
#include <functional>
#include <memory>

#include "player/media_source.h"

class MyTcpClient;

// GUI 线程中的文件业务：TCP 申请授权，HTTP 分块传输，发布后才形成聊天消息。
class MediaTransfer : public QObject
{
    Q_OBJECT
   public:
    using Completion = std::function<void(const QJsonObject&, const QString&)>;
    using SourceCompletion = std::function<void(MediaSource, const QString&)>;  // 在线输入或授权错误。

    explicit MediaTransfer(MyTcpClient* client, QObject* parent = nullptr);  // 借用已登录的 TCP 连接。
    ~MediaTransfer() override;  // 终止本模块的 HTTP 请求，已完成的缓存保留。
    void request(const QString& operation, QJsonObject fields,
                 Completion complete);                                     // 发控制请求，回调返回数据或错误。
    void handleResponse(const QJsonObject& response);                      // 按 request_id 分发 msgid 27。
    void sendVideo(const QJsonObject& conversation, const QString& path);  // 哈希、上传并发布一个 MP4。
    void downloadVideo(const QJsonObject& conversation,
                       const QJsonObject& media);             // 授权下载并校验，完成后交付本地路径。
    QString cachedVideoPath(const QJsonObject& media) const;  // 返回长度匹配的缓存路径，播放前仍校验摘要。
    void requestPlaybackSource(const QJsonObject& conversation, const QJsonObject& media,
                               const QString& rendition, SourceCompletion complete);  // 在线授权；保留档位，不写本地缓存。
    void cancel();                                            // 取消尚未进入发布阶段的传输。
    bool busy() const { return busy_; }                       // 本模块一次只运行一个文件任务。

   signals:
    void phaseChanged(QString text, bool cancellable);                     // 更新任务说明及取消入口。
    void progress(qint64 bytes, qint64 total);                             // 已处理字节与目标总字节。
    void finished();                                                       // 任务结束，界面可关闭进度展示。
    void failed(QString message);                                          // 展示本次任务的失败原因。
    void messagePublished(QJsonObject conversation, QJsonObject message);  // 服务端确认入库后的正式消息。
    void fileReady(QString path);  // 完整下载且通过长度、SHA-256 校验的 MP4。

   private:
    struct Pending
    {
        Completion complete;  // 控制请求完成后的处理。
        QTimer* timer;        // 请求超时计时器，由本对象拥有。
    };

    void hashNext(quint64 generation);     // 每次读取 64 KiB 算摘要，让出 GUI 事件循环。
    void beginUpload(quint64 generation);  // 申请上传资源及一次性 PUT 凭证。
    void upload(const QJsonObject& descriptor,
                quint64 generation);   // QFile 作为 HTTP 请求体，避免整文件进内存。
    void publish(quint64 generation);  // 使用同一 client_msg_id 发布已就绪资源。
    void checkCache(const QJsonObject& descriptor, quint64 generation);     // 分块校验本地副本，失败则下载。
    void startDownload(const QJsonObject& descriptor, quint64 generation);  // 将 HTTP 响应保存到缓存。
    void readDownload(QNetworkReply* reply, quint64 generation);            // 分块写临时文件并累计摘要。
    QNetworkRequest makeRequest(const QJsonObject& descriptor,
                                const QString& method) const;  // 验证地址、方法及授权头。
    void finish(const QString& error = QString());             // 释放当前传输状态并通知界面。
    void connectionLost();                                     // TCP 断开后让等待中的控制请求失败。

    MyTcpClient* client_;                                  // 借用聊天连接，其登录会话绑定媒体凭证。
    QNetworkAccessManager network_;                        // 本线程的 HTTP 请求管理器。
    QHash<QString, Pending> pending_;                      // request_id 到控制请求回调。
    QString directory_;                                    // 系统缓存目录中的 ChatClient/videos，退出不删除。
    QPointer<QNetworkReply> reply_;                        // 当前 HTTP 请求，由 Qt 管理生命周期。
    std::unique_ptr<QFile> source_;                        // 上传源文件，哈希结束后从头读取上传。
    std::unique_ptr<QFile> cached_;                        // 正在校验的缓存文件，不作为上传源。
    std::unique_ptr<QSaveFile> destination_;               // 下载成功前不暴露完整文件名。
    QCryptographicHash hash_{QCryptographicHash::Sha256};  // 当前上传或下载的流式摘要。
    QTimer deadline_;                                      // 文件任务的总超时上限。
    QJsonObject conversation_;                             // 开始任务时冻结的会话，切换联系人不改变它。
    QString clientMessageId_;                              // 一次上传和发布共同使用的幂等编号。
    QString mediaId_;                                      // 服务端分配的资源编号。
    QString expectedHash_;                                 // 上传声明或下载授权返回的 SHA-256。
    QString path_;                                         // 上传源路径或下载最终路径。
    qint64 total_ = 0;                                     // 预期文件字节数，最多 100 MiB。
    qint64 transferred_ = 0;                               // 哈希或下载阶段已消费字节数。
    quint64 generation_ = 0;                               // 使取消前的异步回调失效。
    bool busy_ = false;                                    // 是否存在活动文件任务。
    bool publishing_ = false;                              // 发布进行中不能通过取消撤回已提交消息。
};

#endif
