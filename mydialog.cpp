#include "mydialog.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QJsonObject>


MyDialog::MyDialog()
{
    
}

void MyDialog::init()
{
    // 创建 Dialog
    setWindowTitle("添加好友");

    // 布局
    QVBoxLayout layout(this);

    // 添加标题
    QLabel label("添加好友");
    layout.addWidget(&label);

    // 输入框 + 确定按钮
    QHBoxLayout inputLayout;
    QLineEdit input;
    QPushButton confirmButton("确定");

    inputLayout.addWidget(&input);
    inputLayout.addWidget(&confirmButton);
    layout.addLayout(&inputLayout);

    // 连接按钮槽函数
    connect(&confirmButton, &QPushButton::clicked, [&]() {
        // 想后端发送添加好友请求
        QString name = input.text();
        if(name.isEmpty()) return;
        QJsonObject jsonObj;
        jsonObj["id"] = userid;
        jsonObj["friendname"] = name;
        jsonObj["msgid"] = AddFriendMsg;  // 添加 msgid 属性

        // 转换为 JSON 字符串
        QJsonDocument jsonDoc(jsonObj);
        QByteArray jsonData = jsonDoc.toJson();  // 获取 JSON 数据的字节数组

        // 通过 socket 发送 JSON 数据
        tcpclient->send(jsonData);
    });
}
