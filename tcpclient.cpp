#include "tcpclient.h"
#include <QJsonDocument>
#include <QJsonObject>

MyTcpClient::MyTcpClient()
    : socket(nullptr)
{
}

MyTcpClient::~MyTcpClient()
{
    if(socket) close();
}

QTcpSocket *MyTcpClient::getSocket()
{
    return socket;
}

void MyTcpClient::connectToHost(const QString &host, uint16_t port)
{
    if (socket) {
        socket->close();
        delete socket;
    }

    // 建立Tcp连接（异步，不阻塞UI）
    socket = new QTcpSocket(this);
    connect(socket, &QTcpSocket::connected, this, &MyTcpClient::connected);
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        emit connectionFailed(socket->errorString());
    });
    socket->connectToHost(host, port);
}

void MyTcpClient::send(QByteArray jsonData)
{
    // 通过 socket 发送 JSON 数据
    if (socket && socket->state() == QAbstractSocket::ConnectedState) {
        socket->write(jsonData);  // 发送数据
        socket->flush();          // 确保数据被立即发送
    }
}

QByteArray MyTcpClient::read() {
    // 读取所有可用数据并添加到缓冲区
    m_buffer.append(socket->readAll());

    // 尝试找到一个完整的JSON对象（带字符串感知的花括号匹配）
    int braceCount = 0;
    int startPos = -1;
    bool in_string = false;

    for (int i = 0; i < m_buffer.size(); i++) {
        char c = m_buffer[i];

        if (c == '"') {
            int backslashCount = 0;
            int j = i;
            while (j > 0 && m_buffer[j - 1] == '\\') {
                backslashCount++;
                j--;
            }
            if (backslashCount % 2 == 0) {
                in_string = !in_string;
            }
        }

        if (!in_string) {
            if (c == '{') {
                if (startPos == -1) startPos = i;
                braceCount++;
            } else if (c == '}') {
                braceCount--;
                if (braceCount == 0 && startPos != -1) {
                    // 找到了一个完整的JSON对象
                    QByteArray jsonData = m_buffer.mid(startPos, i - startPos + 1);

                    // 验证是否为有效的JSON
                    QJsonParseError error;
                    QJsonDocument doc = QJsonDocument::fromJson(jsonData, &error);

                    if (error.error == QJsonParseError::NoError) {
                        // 从缓冲区中移除已处理的数据
                        m_buffer.remove(0, i + 1);
                        return jsonData;
                    }
                }
            }
        }
    }

    // 如果缓冲区过大但没有找到完整的JSON，可能是数据损坏
    if (m_buffer.size() > 1024 * 1024) {
        qDebug() << "缓冲区过大，清空。当前大小: " << m_buffer.size();
        m_buffer.clear();
    }

    // 没有找到完整的JSON对象
    return QByteArray();
}

void MyTcpClient::close()
{
    qDebug() << "已触发tcpclient析构";
    socket->close();
}
