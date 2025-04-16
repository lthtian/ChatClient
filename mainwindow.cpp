#include "mainwindow.h"
#include "public.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QListWidget>
#include <QTextEdit>
#include <QPushButton>
#include <QLineEdit>
#include <QFrame>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDialog>
#include <QLabel>
#include <QMessageBox>
#include <QEvent>
#include "qnchatmessage.h"
#include <QTimer>

MainWindow::MainWindow(QWidget *parent, MyTcpClient* tc)
    : QMainWindow(parent), tcpclient(tc)
{
    // 窗口设置
    setWindowTitle("聊天");
    resize(1200, 800);  // 更灵活的初始尺寸
    setMinimumSize(800, 600);  // 设置最小尺寸
    this->setWindowTitle(" ");  // 隐藏标题
    this->setWindowIcon(QIcon(":/image/chat.png"));  // 移除图标
    setupUI();
    applyStyles();  // 应用样式
    avatar = new QPixmap();
}

MainWindow::~MainWindow()
{

}


// ========================================== // 


void MainWindow::setupUI()
{
    // 主分割器（左右布局）
    QSplitter *mainSplitter = new QSplitter(this);

    /* 左侧联系人列表 */
    QWidget *leftWidget = new QWidget();
    QVBoxLayout *leftLayout = new QVBoxLayout(leftWidget);

    contactList = new MyListWidget(this, this);
    contactList->setObjectName("contactList");
    contactList->setMinimumWidth(250);  // 设置最小宽度
    contactList->setFocusPolicy(Qt::NoFocus);  // 禁用焦点框
    leftLayout->addWidget(contactList, 9);

    QWidget* buttonWidget = new QWidget();
    QHBoxLayout *buttonLayout = new QHBoxLayout(buttonWidget);
    buttonLayout->setContentsMargins(5, 5, 5, 5);
    buttonLayout->setSpacing(0);

    exitButton = new QPushButton();
    addFriend = new QPushButton();
    addGroup = new QPushButton();
    createGroup = new QPushButton();
    exitButton->setFixedSize(QSize(50, 50));
    addFriend->setFixedSize(QSize(50, 50));
    addGroup->setFixedSize(QSize(50, 50));
    createGroup->setFixedSize(QSize(50, 50));
    addFriend->setIcon(QIcon(":/image/addfriend.svg"));
    addFriend->setIconSize(QSize(30, 30));
    addGroup->setIcon(QIcon(":/image/addgroup.svg"));
    addGroup->setIconSize(QSize(30, 30));
    createGroup->setIcon(QIcon(":/image/creategroup.svg"));
    createGroup->setIconSize(QSize(30, 30));
    exitButton->setIcon(QIcon(":/image/exit.svg"));
    exitButton->setIconSize(QSize(30, 30));
    buttonLayout->addWidget(exitButton);
    buttonLayout->addWidget(addFriend);
    buttonLayout->addWidget(addGroup);
    buttonLayout->addWidget(createGroup);

    leftLayout->addWidget(buttonWidget, 1);

    /* 右侧聊天区域 */
    QWidget *rightWidget = new QWidget();
    QVBoxLayout *rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);  // 移除默认边距

    // 聊天记录区域
    msgListLabel = new QLabel("欢迎来到TianMu的聊天服务器!");
    msgListLabel->setFont(QFont("Arial", 16));
    msgListLabel->setIndent(30);
    msgListLabel->setStyleSheet(
        "QLabel {"
        "  border-left: 10px solid #07c160;"  // 左侧添加 10px 绿色长条
        "  background-color: #f5f5f5;"
        "}"
    );

    rightLayout->addWidget(msgListLabel, 4);

    messageList = new QListWidget(this);
    messageList->setObjectName("chatHistory");
    messageList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    messageList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    messageList->setVerticalScrollMode(QListWidget::ScrollPerPixel);    // 平滑滚动
    messageList->setSelectionMode(QAbstractItemView::NoSelection);
    messageList->setFocusPolicy(Qt::NoFocus);  // 禁用焦点框
    messageList->setStyleSheet("QListWidget { padding: 0px; }"
                               "QListWidget::item:selected { border: none; background-color: transparent; }");
    rightLayout->addWidget(messageList, 30);

    // 输入区域
    QWidget *inputWidget = new QWidget();
    QVBoxLayout *inputLayout = new QVBoxLayout(inputWidget);
    inputLayout->setContentsMargins(0, 0, 0, 0);  // 移除默认边距
    inputLayout->setSpacing(0);

    messageInput = new MyTextEdit();
    messageInput->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    messageInput->setStyleSheet("QTextEdit { background-color: #F0F0F0; border: none; font-size: 20px; padding-left: 30px; padding-right: 30px; padding-top: 20px; }");
    messageInput->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    sendButton = new QPushButton("发送");
    sendButton->setFixedWidth(120);  // 固定按钮宽度
    sendButton->setFixedHeight(70);
    sendButton->setObjectName("sendButton");

    QHBoxLayout *bLayout = new QHBoxLayout();
    bLayout->addStretch();  // 将按钮推到右侧
    bLayout->addWidget(sendButton);

    // 创建一个水平布局来放置输入框和发送按钮
    QVBoxLayout *inputButtonLayout = new QVBoxLayout();
    inputButtonLayout->setContentsMargins(0, 0, 0, 0);
    inputButtonLayout->setSpacing(0);
    inputButtonLayout->addWidget(messageInput);
    inputButtonLayout->addLayout(bLayout);

    inputLayout->addLayout(inputButtonLayout);
    inputLayout->setSpacing(0);

    rightLayout->addWidget(inputWidget, 5);

    // 组装主界面
    mainSplitter->addWidget(leftWidget);
    mainSplitter->addWidget(rightWidget);
    mainSplitter->setHandleWidth(1);  // 分割线宽度
    mainSplitter->setStretchFactor(1, 3);  // 右侧区域更宽

    setCentralWidget(mainSplitter);

    // 连接信号槽
    // 界面部分
    connect(sendButton, &QPushButton::clicked, this, &MainWindow::sendMessage);
    connect(messageInput, &MyTextEdit::returnPressed, this, &MainWindow::sendMessage);
    connect(contactList, &QListWidget::itemSelectionChanged, this, &MainWindow::reloadMsgList);
    connect(exitButton, &QPushButton::clicked, this, &MainWindow::onExitButtonClicked);
    connect(addFriend, &QPushButton::clicked, this, &MainWindow::onAddFriendClicked);
    connect(addGroup, &QPushButton::clicked, this, &MainWindow::onAddGroupClicked);
    connect(createGroup, &QPushButton::clicked, this, &MainWindow::onCreateGroupClicked);
    // 网络部分
    connect(tcpclient->getSocket(), &QTcpSocket::readyRead, this, &MainWindow::recvHandler);
    // 连接 MyListWidget 的信号
    connect(contactList, &MyListWidget::menuHidden, this, [this]() {
        isMenuVisible = false; // 更新标志位
    });

    // 初始dialog
    initDialog1();
}

void MainWindow::initDialog1()
{
    // 创建 Dialog
    addFriendDialog = new QDialog(this, Qt::FramelessWindowHint);
    addFriendDialog->setWindowTitle("添加好友");

    // 设置对话框的样式表，添加外边框和圆角
    addFriendDialog->setStyleSheet("QDialog { border: 2px solid #ccc; border-radius: 10px; background-color: white; }");

    // 创建布局
    QVBoxLayout* layout = new QVBoxLayout(addFriendDialog);

    // 创建标题行布局
    QHBoxLayout* titleLayout = new QHBoxLayout();
    addFriendLabel = new QLabel("添加好友", addFriendDialog);

    // 创建退出按钮
    QPushButton* closeButton = new QPushButton(addFriendDialog);
    closeButton->setIcon(QIcon(":/image/close.png"));
    closeButton->setIconSize(QSize(15, 15));
    closeButton->setFlat(true);
    closeButton->setFixedSize(30, 30);

    // 使用样式表为此按钮设置样式，避免全局样式影响
    closeButton->setStyleSheet(
        "QPushButton {"
        "   background: transparent;"  // 背景透明
        "   border: none;"            // 无边框
        "}"
        "QPushButton:hover {"
        "   icon-size: 20px;"         // 鼠标悬停时图标变大
        "}"
    );



    // 将标签和按钮添加到标题行布局
    titleLayout->addWidget(addFriendLabel);
    titleLayout->addStretch();  // 添加一个弹簧，使按钮靠右
    titleLayout->addWidget(closeButton);

    // 将标题行布局添加到主布局
    layout->addLayout(titleLayout);

    // 输入框 + 确定按钮
    QHBoxLayout* inputLayout = new QHBoxLayout();
    addFriendInput = new QLineEdit(addFriendDialog);
    QPushButton* confirmButton = new QPushButton("确定", addFriendDialog);
    confirmButton->setDefault(true);

    inputLayout->addWidget(addFriendInput);
    inputLayout->addWidget(confirmButton);
    layout->addLayout(inputLayout);

    // 连接按钮槽函数
    connect(confirmButton, &QPushButton::clicked, [&]() {
        // 向后端发送添加好友请求
        qDebug() << "已触发lambda";
        if(dialogOP == -1) return;
        else if(dialogOP == 1)
        {
            QString name = addFriendInput->text();
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
        }
        else if(dialogOP == 2)
        {
            QString name = addFriendInput->text();
            if(name.isEmpty()) return;
            QJsonObject jsonObj;
            jsonObj["userid"] = userid;
            jsonObj["groupname"] = name;
            jsonObj["msgid"] = AddGroupMsg;  // 添加 msgid 属性
            jsonObj["role"] = "normal";

            // 转换为 JSON 字符串
            QJsonDocument jsonDoc(jsonObj);
            QByteArray jsonData = jsonDoc.toJson();  // 获取 JSON 数据的字节数组
            qDebug() << "已发送加入群聊申请: " + jsonData;
            // 通过 socket 发送 JSON 数据
            tcpclient->send(jsonData);
        }
        else if(dialogOP == 3)
        {
            QString name = addFriendInput->text();
            if(name.isEmpty()) return;
            QJsonObject jsonObj;
            jsonObj["userid"] = userid;
            jsonObj["groupname"] = name;
            jsonObj["msgid"] = CreateGroupMsg;  // 添加 msgid 属性

            // 转换为 JSON 字符串
            QJsonDocument jsonDoc(jsonObj);
            QByteArray jsonData = jsonDoc.toJson();  // 获取 JSON 数据的字节数组
            // 通过 socket 发送 JSON 数据
            tcpclient->send(jsonData);
        }
        addFriendInput->clear();
    });

    // 连接退出按钮的槽函数
    connect(closeButton, &QPushButton::clicked, [this]() {
        addFriendDialog->close();
    });
}

void MainWindow::applyStyles()
{
    // 通用样式
    setStyleSheet(R"(
        QWidget {
            font-family: "微软雅黑";
        }

        #contactList {
            background-color: #f5f5f5;
            border: none;
            padding: 5px;
            font-size: 16px;

        }

        #contactList::item {
            height: 50px;
            padding: 5px;
            border-bottom: 1px solid #e0e0e0;
        }

        #contactList::item:selected {
            background-color: #e0e0e0;
        }

        #chatHistory {
            background-color: white;
            border: none;
            padding: 15px;
        }

        QLineEdit {
            border: 1px solid #e0e0e0;
            padding: 8px;
        }

        QPushButton {
            background-color: #07c160;
            color: white;
            border: none;
            border-radius: 4px;
            padding: 8px 15px;
        }

        QPushButton#sendButton{
            margin-right: 25px;
            margin-bottom: 25px;
            border-radius: 25px;
        }

        QPushButton:hover {
            background-color: #05a050;
        }

        QSplitter::handle {
            background-color: #e0e0e0;
        }
    )");
}

// ------------------------------------------------------------------------------------------ //

void MainWindow::setUser(int id, QString name)
{
    userid = id;
    username = name;
}

int MainWindow::getUserId()
{
    qDebug() << userid;
    return userid;
}

// 根据返回的msgid, 触发不同的回复响应代码
void MainWindow::recvHandler()
{
    QByteArray responseData = tcpclient->read();  // 读取返回的数据

    // qDebug() << "Received data:" << responseData;

    QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);  // 解析 JSON 数据

    QJsonObject jsonObj;
    if (jsonDoc.isObject()) jsonObj = jsonDoc.object();
    else
    {
        qDebug() << "返回数据不是有效的 JSON 格式！";
        return;
    }

    int msgid = jsonObj["msgid"].toInt();
    switch(msgid)
    {
    // 初始化回复处理  ========================================== //
    case InitMsgAck:
        // 初始化好友列表
        if (jsonObj.contains("friends") && jsonObj["friends"].isArray()) {
            QJsonArray friendsArray = jsonObj["friends"].toArray();

            // 遍历 friends 数组
            for (const QJsonValue &friendValue : friendsArray) {
                if (friendValue.isString()) {
                    // 处理每个好友 JSON 字符串
                    QString friendJsonStr = friendValue.toString();
                    QJsonDocument friendDoc = QJsonDocument::fromJson(friendJsonStr.toUtf8());
                    if (friendDoc.isObject()) {
                        QJsonObject friendObj = friendDoc.object();
                        int id = friendObj["id"].toString().toInt();  // 这里以前有坑!!!
                        QString name = friendObj["name"].toString();
                        QString state = friendObj["state"].toString();
                        // 存入当前好友列表中
                        _list.insert({name, {id, false}});
                        // 再向后端申请对应好友的头像
                        getImage(name);
                        // 更新前端界面
                        addToContactList(name, false);
                    }
                }
            }
        }
        // 初始化群组列表
        if (jsonObj.contains("groups") && jsonObj["groups"].isArray()) {
            QJsonArray groupsArray = jsonObj["groups"].toArray();

            // 遍历 groups 数组
            for (const QJsonValue &groupValue : groupsArray) {
                if (groupValue.isString()) {
                    // 解析 JSON 字符串
                    QString groupJsonStr = groupValue.toString();
                    QJsonDocument groupDoc = QJsonDocument::fromJson(groupJsonStr.toUtf8());
                    if (groupDoc.isObject()) {
                        QJsonObject groupObj = groupDoc.object();
                        int groupId = groupObj["id"].toString().toInt();  // 转换群组 ID
                        QString groupName = groupObj["groupname"].toString();

                        // 存入当前群组列表
                        _list.insert({groupName, {groupId, true}});
                        // 更新前端 UI（假设群组列表是 groupList）
                        addToContactList(groupName, true);
                    }
                }
            }
        }

        contactList->clearSelection();
        contactList->setCurrentItem(nullptr);

        break;
    // 私聊回复处理 ========================================== //
    case OTOMsg:
    case GroupChatMsg:
    {
        // sendername
        QString name, sendername;
        if(msgid == OTOMsg) name = jsonObj["sender"].toString(), sendername = name;
        else name = jsonObj["groupname"].toString(), sendername = jsonObj["sendername"].toString();
        QString message = jsonObj["message"].toString();

        // 只有当前选中的item是对应的才更新列表
        if(!contactList->currentItem() || name != contactList->currentItem()->text())
        {
            // 到达这里之前肯定已经经历过查询了
            // 说明当前消息未查看, 计数+1
            qDebug() << mp[name];
            contactList->setItemValue(mp[name], contactList->getItemValue(mp[name]) + 1);
            int row = contactList->row(mp[name]);
            QListWidgetItem* takenItem = contactList->takeItem(row); // 移除item
            contactList->insertItem(0, takenItem); // 插入到顶部(第0行)

            // 告知后端未读消息数+1
            QJsonObject jsonObj;
            jsonObj["userid"] = userid;
            jsonObj["sender"] = _list[name].first;
            jsonObj["isgroup"] = _list[name].second;
            jsonObj["msgid"] = addNewMsgCnt;  // 添加 msgid 属性
            // 转换为 JSON 字符串
            QJsonDocument jsonDoc(jsonObj);
            QByteArray jsonData = jsonDoc.toJson();  // 获取 JSON 数据的字节数组
            // 通过 socket 发送 JSON 数据
            tcpclient->send(jsonData);
            break;
        }

        // 创建消息控件
        QNChatMessage* messageWidget = new QNChatMessage(messageList, _avatars[sendername]);
        QString time = QString::number(QDateTime::currentDateTime().toTime_t());
        QSize size = messageWidget->fontRect(message);

        // 设置对方消息样式
        QNChatMessage::User_Type userType = QNChatMessage::User_She;

        if(msgid == OTOMsg) messageWidget->setText(message, time, size, userType, name, false);
        else messageWidget->setText(message, time, size, userType, jsonObj["sendername"].toString(), true);

        // 添加到列表
        QListWidgetItem* item = new QListWidgetItem();
        item->setSizeHint(size);
        messageList->addItem(item);
        messageList->setItemWidget(item, messageWidget);

        // 接收到消息直接拉到最底
        messageList->scrollToItem(messageList->item(messageList->count() - 1), QAbstractItemView::PositionAtBottom);
    }
        break;
    // 添加业务处理 ========================================== //
    case AddFriendMsgAck:
        if(jsonObj.contains("errmsg"))
            addFriendLabel->setText(jsonObj["errmsg"].toString());
        if(jsonObj["errno"].toInt() != 0) return;
        // 更新聊天列表与map
        addToContactList(jsonObj["friendname"].toString(), false);
        _list.insert({jsonObj["friendname"].toString(), {jsonObj["friendid"].toInt(), false}});
        break;
    case AddGroupMsgAck:
        if(jsonObj.contains("errmsg"))
            addFriendLabel->setText(jsonObj["errmsg"].toString());
        if(jsonObj["errno"].toInt() != 0) return;
        // 更新聊天列表与map
        addToContactList(jsonObj["groupname"].toString(), true);
        _list.insert({jsonObj["groupname"].toString(), {jsonObj["groupid"].toInt(), true}});
        break;
    // 创建群聊业务 ========================================== //
    case CreateGroupMsgAck:
        if(jsonObj.contains("errmsg"))
            addFriendLabel->setText(jsonObj["errmsg"].toString());
        if(jsonObj["errno"].toInt() != 0) return;
        // 更新聊天列表与map
        addToContactList(jsonObj["groupname"].toString(), true);
        _list.insert({jsonObj["groupname"].toString(), {jsonObj["groupid"].toInt(), true}});
        break;
    // 历史记录处理 ========================================== //
    case HistoryMsgAck:
        if (jsonObj.contains("history") && jsonObj["history"].isArray()) {
            QJsonArray historyArray = jsonObj["history"].toArray();
            bool isgroup = jsonObj["isgroup"].toBool();
            // 遍历历史记录
            QDateTime currentDateTime = QDateTime::currentDateTime();
            node today{currentDateTime.date().year(), currentDateTime.date().month(), currentDateTime.date().day()};
            QDateTime LastTime = QDateTime::fromSecsSinceEpoch(0);

            for (const QJsonValue &historyValue : historyArray) {
                if (historyValue.isString()) {
                    // 处理每个好友 JSON 字符串
                    QString friendJsonStr = historyValue.toString();
                    QJsonDocument HistoryDoc = QJsonDocument::fromJson(friendJsonStr.toUtf8());
                    if (HistoryDoc.isObject()) {
                        QJsonObject historyObj = HistoryDoc.object();
                        int id = historyObj["id"].toString().toInt();  // 这里以前有坑!!!
                        QString message = historyObj["message"].toString();
                        QString msgtime = historyObj["time"].toString();
                        QString name = historyObj["name"].toString();

                        QDateTime dateTime = QDateTime::fromString(msgtime, "yyyy-MM-dd HH:mm:ss");
                        // 根据dateTime判断是否显示时间
                        node x{dateTime.date().year(), dateTime.date().month(), dateTime.date().day()};
                        if(timeset.count(x) == 0 && !(x == today))
                        {
                            dealMessageTime(dateTime, 0);
                            timeset.insert(x);
                        }
                        qint64 diffMinutes = LastTime.msecsTo(dateTime) / 60000;
                        if(x == today && diffMinutes >= 10)
                        {
                            dealMessageTime(dateTime, 1);
                            LastTime = dateTime;
                        }

                        // 更新前端界面
                        QNChatMessage* messageWidget;

                        // 设置对方消息样式
                        QNChatMessage::User_Type userType;
                        if(id != userid)
                        {
                            if(_avatars[name] == nullptr)
                            {
                                _list[name] = {id, false};
                                getImage(name);
                            }
                            userType = QNChatMessage::User_She, messageWidget = new QNChatMessage(messageList, _avatars[name]);
                        }
                        else userType = QNChatMessage::User_Me, messageWidget = new QNChatMessage(messageList, avatar);

                        QString time = QString::number(QDateTime::currentDateTime().toTime_t());
                        QSize size = messageWidget->fontRect(message);

                        // 这里要用到后台发来的姓名
                        if(id == userid) messageWidget->setText(message, time, size, userType, username);
                        else
                        {
                            if(isgroup)
                            {
                                messageWidget->setText(message, time, size, userType, name, true);
                            }
                            else messageWidget->setText(message, time, size, userType, name);
                        }

                        // 更新消息状态为发送成功
                        messageWidget->setTextSuccess();

                        // 添加到列表
                        QListWidgetItem* item = new QListWidgetItem();
                        item->setSizeHint(size);
                        messageList->addItem(item);
                        messageList->setItemWidget(item, messageWidget);
                    }
                }
            }
            QTimer::singleShot(2, this, &MainWindow::refreshMessageLayout);
            messageList->scrollToItem(messageList->item(messageList->count() - 1), QAbstractItemView::PositionAtBottom);
        }
        break;
    // 查询未读消息数处理========================================================//
    case NewMsgAck:
    {
        qDebug() << "获取NewMsgAck";
        QString name = jsonObj["name"].toString();
        int cnt = jsonObj["cnt"].toInt();
        if(cnt == 0) contactList->setItemValue(mp[name], 0);
        else if(cnt > 0)
        {
            contactList->setItemValue(mp[name], cnt);
            int row = contactList->row(mp[name]);
            QListWidgetItem* takenItem = contactList->takeItem(row); // 移除item
            contactList->insertItem(0, takenItem); // 插入到顶部(第0行)
        }
    }
        break;
    // 图片查询处理==============================================================//
    case imageReqAck:
        {
            if(jsonObj["isSuccess"].toString() == QString("false")) break;
            qDebug() << "触发imageReqAck";
            QString base64Data = jsonObj["image_data"].toString();
            qDebug() << "Base64数据长度: " << base64Data.length();

            if (base64Data.isEmpty()) {
                qDebug() << "错误: Base64数据为空";
                break;
            }

            // Base64解码
            QByteArray imageData = QByteArray::fromBase64(base64Data.toUtf8());
            qDebug() << "解码后图片数据大小: " << imageData.size() << " 字节";

            if (imageData.isEmpty()) {
                qDebug() << "错误: 解码后图片数据为空";
                break;
            }

            QString name = jsonObj["username"].toString();

            // 转换为QPixmap
            if(name == username) avatar->loadFromData(imageData);
            else
            {
                if(_avatars[name] == nullptr) _avatars[name] = new QPixmap();
                _avatars[name]->loadFromData(imageData);
                // 如果当前list不为空, 更新一遍头像
                if(messageList->count() != 0) reloadAvatar();
            }
        }
        break;
    default:
        break;
    }

}

void MainWindow::reloadAvatar()
{
    for (int i = 0; i < messageList->count(); ++i) {
        // 获取当前 item
        QListWidgetItem *item = messageList->item(i);

        // 获取 item 关联的 widget（即 QNChatMessage）
        QWidget *widget = messageList->itemWidget(item);
        if (!widget) continue;  // 确保 widget 存在

        // 尝试转换为 QNChatMessage
        QNChatMessage *chatMessage = qobject_cast<QNChatMessage*>(widget);
        if (!chatMessage) continue;  // 确保转换成功
        if(chatMessage->userType() != QNChatMessage::User_Me) chatMessage->setAvatar(_avatars[chatMessage->name()]);
    }
}

void MainWindow::dealMessageTime(QDateTime dateTime, int op)
{
    // 更新前端界面
    QNChatMessage* messageWidget = new QNChatMessage(messageList);
    QString time = QString::number(dateTime.toTime_t());
    QSize size = QSize(messageList->width(), 40);

    // 设置对方消息样式
    QString message;
    if(op == 0) message = "a";
    else if(op == 1) message = "b";
    QNChatMessage::User_Type userType;
    userType = QNChatMessage::User_Time;
    messageWidget->setText(message, time, size, userType);

    // 添加到列表
    QListWidgetItem* item = new QListWidgetItem();
    item->setSizeHint(size);
    messageList->addItem(item);
    messageList->setItemWidget(item, messageWidget);
}

void MainWindow::addToContactList(QString name, bool isgroup)
{
    QListWidgetItem* item = new QListWidgetItem(name);
    mp[name] = item;

    // 向后台查询未读消息数
    QJsonObject jsonObj;
    jsonObj["userid"] = userid;
    jsonObj["sender"] = _list[name].first;
    jsonObj["name"] = name;
    jsonObj["isgroup"] = _list[name].second;
    jsonObj["msgid"] = NewMsg;  // 添加 msgid 属性
    // 转换为 JSON 字符串
    QJsonDocument jsonDoc(jsonObj);
    QByteArray jsonData = jsonDoc.toJson();  // 获取 JSON 数据的字节数组
    // 通过 socket 发送 JSON 数据
    tcpclient->send(jsonData);

    if(!isgroup) item->setIcon(QIcon(":/image/otochat.png"));
    else item->setIcon(QIcon(":/image/groupchat.png"));
    contactList->addItem(item);
}

void MainWindow::sendMessage()
{
    qDebug() << "触发sendMessage";
    // 获取输入框中的文本
    QString message = messageInput->toPlainText();
    if (message.isEmpty()) return;
    //if(messageList->item(0)->text() == QString("Welcome to my ChatClient!")) return;

    // 清空输入框
    messageInput->clear();

    // 获取当前选中的联系人
    QListWidgetItem *currentItem = contactList->currentItem();
    if(!currentItem)
    {
        qDebug() << "当前未选中联系人!";
        return;
    }

    // -----------------------------------------------------------------//
    // 向别人发送消息的同时也更新自己的对话框界面
    // 创建消息控件
    QNChatMessage* messageWidget = new QNChatMessage(messageList, avatar);
    QString time = QString::number(QDateTime::currentDateTime().toTime_t());
    QSize size = messageWidget->fontRect(message); // 计算气泡尺寸

    // 根据用户类型设置消息方向（示例中假设是用户自己发送）
    QNChatMessage::User_Type userType = QNChatMessage::User_Me;
    messageWidget->setText(message, time, size, userType);

    // 创建ListWidgetItem并设置大小
    QListWidgetItem* item = new QListWidgetItem();
    item->setSizeHint(size);
    messageList->addItem(item);
    messageList->setItemWidget(item, messageWidget);

    // 更新消息状态为发送成功
    messageWidget->setTextSuccess();
    // -----------------------------------------------------------------//
    messageList->scrollToItem(messageList->item(messageList->count() - 1), QAbstractItemView::PositionAtBottom);

    // 检测联系人是个人还是群
    QString name = currentItem->text();

    if(!_list[name].second)  // 个人, 私聊
    {
        QJsonObject jsonObj;
        jsonObj["id"] = userid;
        jsonObj["sender"] = username;
        jsonObj["to"] = _list[name].first;
        jsonObj["msgid"] = OTOMsg;  // 添加 msgid 属性
        jsonObj["message"] = message;

        // 转换为 JSON 字符串
        QJsonDocument jsonDoc(jsonObj);
        QByteArray jsonData = jsonDoc.toJson();  // 获取 JSON 数据的字节数组

        // 通过 socket 发送 JSON 数据
        tcpclient->send(jsonData);
    }
    else   // 群, 群聊
    {
        QJsonObject jsonObj;
        jsonObj["userid"] = userid;
        jsonObj["sendername"] = username;
        jsonObj["groupid"] = _list[name].first;
        jsonObj["groupname"] = name;
        jsonObj["msgid"] = GroupChatMsg;  // 添加 msgid 属性
        jsonObj["message"] = message;

        // 转换为 JSON 字符串
        QJsonDocument jsonDoc(jsonObj);
        QByteArray jsonData = jsonDoc.toJson();  // 获取 JSON 数据的字节数组
        // 通过 socket 发送 JSON 数据
        tcpclient->send(jsonData);
    }
}

void MainWindow::reloadMsgList()
{
    // 一旦触发reload就清空timeset
    timeset.clear();
    QString current = contactList->currentItem()->text();
    if(current.isEmpty()) return;

    if(isMenuVisible)  // 使用标志位判断菜单是否可见
    {
        isMenuVisible = false;
        return;
    }

    msgListLabel->setText(current);

    messageList->clear();
    // 向后端发出查找历史记录的请求
    QJsonObject jsonObj;
    jsonObj["msgid"] = HistoryMsg;  // 添加 msgid 属性
    if(!_list[current].second)  // 个人
    {
        jsonObj["isgroup"] = false;
        jsonObj["id1"] = userid;
        jsonObj["id2"] = _list[current].first;
    }
    else // 群组
    {
        jsonObj["isgroup"] = true;
        jsonObj["groupid"] = _list[current].first;
    }
    // 转换为 JSON 字符串
    QJsonDocument jsonDoc(jsonObj);
    QByteArray jsonData = jsonDoc.toJson();  // 获取 JSON 数据的字节数组

    // 通过 socket 发送 JSON 数据
    tcpclient->send(jsonData);

    // 请求到信息后再发出清除未读消息数的消息
    // 向后台查询未读消息数
    QJsonObject jsonObj2;
    jsonObj2["userid"] = userid;
    jsonObj2["sender"] = _list[current].first;
    jsonObj2["isgroup"] = _list[current].second;
    jsonObj2["msgid"] = removeNewMsgCnt;  // 添加 msgid 属性
    // 转换为 JSON 字符串
    QJsonDocument jsonDoc2(jsonObj2);
    QByteArray jsonData2 = jsonDoc2.toJson();  // 获取 JSON 数据的字节数组
    // 通过 socket 发送 JSON 数据
    tcpclient->send(jsonData2);

    // 取消标记
    contactList->setItemValue(mp[current], 0);
}

// 向后端发出初始化界面的请求
void MainWindow::initByTCP()
{
    // 创建 JSON 对象并添加数据
    QJsonObject jsonObj;
    jsonObj["msgid"] = InitMsg;  // 添加 msgid 属性
    jsonObj["id"] = userid;

    // 转换为 JSON 字符串
    QJsonDocument jsonDoc(jsonObj);
    QByteArray jsonData = jsonDoc.toJson();  // 获取 JSON 数据的字节数组

    qDebug() << QString("已发送: ") + jsonData;

    tcpclient->send(jsonData);

    _list.insert({username, {userid, false}});
    getImage(username);
}

void MainWindow::getImage(QString& name)
{
    QJsonObject jsonObj;
    jsonObj["userid"] = _list[name].first;
    jsonObj["username"] = name;
    jsonObj["msgid"] = imageReq;
    QJsonDocument jsonDoc(jsonObj);
    QByteArray jsonData = jsonDoc.toJson();
    tcpclient->send(jsonData);
}

void MainWindow::onAddFriendClicked() {
    addFriendLabel->setText("添加好友");
    dialogOP = 1;
    addFriendInput->setFocus();
    addFriendDialog->exec();  // 显示对话框
}

void MainWindow::onAddGroupClicked()
{
    addFriendLabel->setText("添加群组");
    dialogOP = 2;
    addFriendInput->setFocus();
    addFriendDialog->exec();
}

void MainWindow::onCreateGroupClicked()
{
    addFriendLabel->setText("创建群组");
    dialogOP = 3;
    addFriendInput->setFocus();
    addFriendDialog->exec();
}

void MainWindow::onExitButtonClicked()
{
    this->close();
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);

    // 遍历所有消息项，调整气泡宽度
    for (int i = 0; i < messageList->count(); i++) {
        QListWidgetItem* item = messageList->item(i);
        QNChatMessage* widget = qobject_cast<QNChatMessage*>(messageList->itemWidget(item));
        if (widget) {
            QSize newSize = widget->fontRect(widget->text());
            item->setSizeHint(newSize);
            widget->setFixedWidth(messageList->width()); // 强制宽度适应
        }
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange) {
        // 窗口状态变化（如最大化/最小化）后强制触发resizeEvent
        QTimer::singleShot(0, this, [this]() {
            QResizeEvent resizeEvent(size(), QSize());
            this->resizeEvent(&resizeEvent);
        });
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::refreshMessageLayout()
{
    // 创建一个resize事件并发送给窗口
    QResizeEvent resizeEvent(this->size(), this->size());
    QApplication::sendEvent(this, &resizeEvent);

    // 确保滚动到底部
    if (messageList->count() > 0) {
        messageList->scrollToItem(messageList->item(messageList->count() - 1), QAbstractItemView::PositionAtBottom);
    }
}
