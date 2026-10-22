#pragma once
#include "media_decoder.h"
#include <QJsonObject>
#include <QThread>
#include <memory>

// 一次 WebRTC 会话。信令、编解码和释放都在工作线程，GUI 只交换有界数据。
class RtcConnection : public QThread {
    Q_OBJECT
public:
    RtcConnection(QJsonObject descriptor, bool publishing, int output_rate, QObject* parent = nullptr);
    ~RtcConnection() override; // 正常关闭等 finished 后销毁；析构提供最后的生命周期保护。
    void SetPicture(QImage image); // 替换最新摄像头画面，不积累待编码画面。
    void SetMicrophone(bool enabled); // 原子开关；设备只在编码线程创建/释放。
    void Stop(); // 原子取消网络等待和媒体循环，不阻塞 GUI。
    MediaBatch TakeMedia(); // GUI 取走当前批次，锁只保护移动操作。
signals:
    void Status(const QString& message); // 连接建立、音频设备等状态。
    void Failed(const QString& message); // 本次会话终止，界面允许重新申请票据。
    void Connected(); // ICE/DTLS 已建立，轨道可以发送媒体。
protected:
    void run() override; // 完成 Offer/Answer 后持续编码发送或接收解码，结束时 DELETE 资源。
private:
    struct Data;
    std::shared_ptr<Data> data_; // 网络回调只持弱引用，防止关闭后访问已释放对象。
};
