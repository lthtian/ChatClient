#include "tcpclient.h"
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>

MyTcpClient::MyTcpClient()
{
    connect();
    m_buffer.clear();
}

MyTcpClient::~MyTcpClient()
{
    if(socket) close();
}

QTcpSocket *MyTcpClient::getSocket()
{
    return socket;
}

void MyTcpClient::connect()
{
    // 建立Tcp连接
    socket = new QTcpSocket();
    socket->connectToHost("82.156.254.74", 8000);
    bool ret = socket->waitForConnected();
    if(!ret)
    {
        qDebug() << "连接失败";
        QMessageBox::warning(nullptr, "连接失败", "无法连接到服务器，请检查网络连接！");
        exit(0);
    }
    else qDebug() << "连接成功";
}

void MyTcpClient::send(QByteArray jsonData)
{
    // 通过 socket 发送 JSON 数据
    if (socket && socket->state() == QAbstractSocket::ConnectedState) {
        socket->write(jsonData);  // 发送数据
        socket->flush();          // 确保数据被立即发送
    } else {
        QMessageBox::warning(nullptr, "连接失败", "无法连接到服务器！");
    }
}

QByteArray MyTcpClient::read() {
    // 读取所有可用数据并添加到缓冲区
    m_buffer.append(socket->readAll());

    // 尝试找到一个完整的JSON对象
    int braceCount = 0;
    int startPos = -1;

    // 找到第一个 '{'
    for (int i = 0; i < m_buffer.size(); i++) {
        if (m_buffer[i] == '{') {
            startPos = i;
            break;
        }
    }

    if (startPos == -1) {
        // 没有找到开始的 '{'，清空缓冲区
        if (m_buffer.size() > 1024) {
            m_buffer.clear();
        }
        return QByteArray();
    }

    // 从startPos开始，计算括号匹配
    for (int i = startPos; i < m_buffer.size(); i++) {
        if (m_buffer[i] == '{') {
            braceCount++;
        } else if (m_buffer[i] == '}') {
            braceCount--;
            if (braceCount == 0) {
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
