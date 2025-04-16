#include "mylistwidget.h"
#include "mainwindow.h"
#include "public.h"
#include <QJsonObject>
#include <QJsonDocument>

void MyListWidget::showContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = itemAt(pos); // 获取点击的 item
    if (!item) return;

    // 获取父对象并确保它是 MainWindow 类型
    QString name = item->text();
    QAction *editAction;
    if(!mwindow->_list[name].second) editAction = contextMenu->addAction("删除好友");
    else editAction = contextMenu->addAction("退出/移除群聊");

    // 连接菜单的 aboutToHide 信号
    connect(contextMenu, &QMenu::aboutToHide, this, [this]() {
        emit menuHidden(); // 发射自定义信号
    });

    QAction *selectedAction = contextMenu->exec(mapToGlobal(pos)); // 显示菜单
    mwindow->isMenuVisible = true;

    if (selectedAction == editAction) {
        // 向后端发出删除请求
        if(!mwindow->_list[name].second) // 删除个人
        {
            QJsonObject jsonObj;
            jsonObj["userid"] = mwindow->getUserId();
            jsonObj["friendid"] = mwindow->_list[name].first;
            jsonObj["msgid"] = RemoveFriendMsg;  // 添加 msgid 属性

            // 转换为 JSON 字符串
            QJsonDocument jsonDoc(jsonObj);
            QByteArray jsonData = jsonDoc.toJson();  // 关键修改：紧凑格式
            qDebug() << "发送的删除请求数据：" << jsonData;
            mwindow->tcpclient->send(jsonData);
        }
        else  // 删除群组
        {
            QJsonObject jsonObj;
            jsonObj["userid"] = mwindow->getUserId();
            jsonObj["groupid"] = mwindow->_list[name].first;
            jsonObj["msgid"] = RemoveGroupMsg;  // 添加 msgid 属性

            // 转换为 JSON 字符串
            QJsonDocument jsonDoc(jsonObj);
            QByteArray jsonData = jsonDoc.toJson();  // 关键修改：紧凑格式
            qDebug() << "发送的删除请求数据：" << jsonData;
            mwindow->tcpclient->send(jsonData);
        }

        delete item;
    }
    contextMenu->clear();
    return;
}
