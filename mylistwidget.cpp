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

    QAction *selectedAction = contextMenu->exec(mapToGlobal(pos)); // 显示菜单
    mwindow->isMenuVisible = true;

    if (selectedAction == editAction) {
        // 向后端发出删除请求
        if(!mwindow->_list[name].second) // 删除个人
        {
            mwindow->tcpclient->sendJson(QJsonObject{
                {"userid", mwindow->getUserId()},
                {"friendid", mwindow->_list[name].first},
                {"msgid", RemoveFriendMsg}
            });
        }
        else  // 删除群组
        {
            mwindow->tcpclient->sendJson(QJsonObject{
                {"userid", mwindow->getUserId()},
                {"groupid", mwindow->_list[name].first},
                {"msgid", RemoveGroupMsg}
            });
        }

        delete item;
    }
    contextMenu->clear();
    return;
}
