#ifndef LOGIN_H
#define LOGIN_H

#include <QDialog>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include "tcpclient.h"
#include "circularavatarlabe.h"

class LoginWindow : public QDialog
{
    Q_OBJECT

public:
    LoginWindow(QWidget *parent = nullptr, MyTcpClient* tc = nullptr);  // 构造函数声明
    ~LoginWindow();                         // 析构函数声明

private slots:
    void handleLogin();  // 登录按钮点击事件的槽函数
    void change();
    void login();

signals:
    void loginSuccess(int userid, QString username);

protected:
    bool eventFilter(QObject *obj, QEvent *event);

private:
    void openAvatarSelector();
    QByteArray compressImageBeforeUpload(const QString &filePath);

    QLabel* label;             // 显示信息
    QLineEdit *usernameInput;  // 用户名输入框
    QLineEdit *passwordInput;  // 密码输入框
    QPushButton *loginButton;  // 登录按钮
    QPushButton *changeReg;    // 切换注册界面

    bool islogin = true; // 判断是否在登录界面

    // 头像相关
    CircularAvatarLabel *avatarLabel;
    QHBoxLayout *inputAreaLayout;
    QVBoxLayout *inputContainer;
    QVBoxLayout *mainLayout;

    MyTcpClient* tcpclient;
};

#endif // LOGIN_H


