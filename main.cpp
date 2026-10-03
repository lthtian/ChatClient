#include "mainwindow.h"
#include "login.h"
#include "tcpclient.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    MyTcpClient* tcpclient = new MyTcpClient();
    LoginWindow* loginWindow = new LoginWindow(nullptr, tcpclient);
    MainWindow chatWindow(nullptr, tcpclient);

    // 使聊天窗口可以接收登录界面取得的id
    QObject::connect(loginWindow, &LoginWindow::loginSuccess, &chatWindow, &MainWindow::setUser);

    // 异步连接服务器
    // 直接连接服务器的聊天入口，媒体地址由服务端另行返回。
    const QString host = qEnvironmentVariable("CHAT_HOST", "39.105.18.142");
    const int port = qEnvironmentVariableIntValue("CHAT_PORT");
    tcpclient->connectToHost(host, port > 0 && port <= 65535 ? port : 7000);

    if (loginWindow->exec() == QDialog::Accepted) {
        loginWindow->close();  // 关闭登录窗口
        delete loginWindow;    // 释放登录窗口的内存
        // 登录成功，显示聊天窗口
        chatWindow.initByTCP();
        chatWindow.show();
        qDebug() << QString::number(chatWindow.getUserId()) + QString("号用户已登录");
        return a.exec();
    }

    tcpclient->close();
    return 0;
}
