#ifndef TCPCLIENT_H
#define TCPCLIENT_H

#include <QTcpSocket>


class MyTcpClient
{
public:
    MyTcpClient();
    ~MyTcpClient();
    QTcpSocket* getSocket();
    void connect();
    void send(QByteArray jsonData);
    QByteArray read();
    void close();

private:
    QTcpSocket* socket;
    QByteArray m_buffer; // 缓冲区
};

#endif // TCPCLIENT_H
