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
