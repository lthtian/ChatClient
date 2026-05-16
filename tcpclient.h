#ifndef TCPCLIENT_H
#define TCPCLIENT_H

#include <QTcpSocket>


class MyTcpClient : public QObject
{
    Q_OBJECT

public:
    MyTcpClient();
    ~MyTcpClient();
    QTcpSocket* getSocket();
    void connectToHost(const QString &host, uint16_t port);
    void send(QByteArray jsonData);
    QByteArray read();
    void close();

signals:
    void connected();
    void connectionFailed(QString error);

private:
    QTcpSocket* socket;
    QByteArray m_buffer; // 缓冲区
};

#endif // TCPCLIENT_H
