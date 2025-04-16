#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QPushButton>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QLabel>
#include <set>
#include <unordered_map>
#include "tcpclient.h"
#include "mytextedit.h"
#include "mylistwidget.h"
using std::set;
using std::unordered_map;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

struct node{
    int y, m, d;
    bool operator==(const node& w) const
    {
        return y == w.y && m == w.m && d == w.d;
    }

    bool operator<(const node& w) const
    {
        if(y != w.y) return y < w.y;
        if(m != w.m) return m < w.m;
        return d < w.d;
    }
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr, MyTcpClient* tc = nullptr);
    ~MainWindow();
    int getUserId();
    void initByTCP();
    void addToContactList(QString name, bool isgroup);

private slots:
    void reloadMsgList();
    void onAddFriendClicked();
    void onAddGroupClicked();
    void onCreateGroupClicked();
    void onExitButtonClicked();

public slots:
    void sendMessage();
    void setUser(int id, QString name);
    void recvHandler();
    void getImage(QString& name);


private:
    // UI 元素
    QListWidget* messageList;  // 用于显示聊天消息
    QLabel* msgListLabel;      // 显示当前聊天对象
    MyTextEdit *messageInput;   // 用于输入消息
    QPushButton *sendButton;   // 用于发送消息
    MyListWidget* contactList;
    QPushButton* addFriend;
    QPushButton* addGroup;
    QPushButton* exitButton;
    QPushButton* createGroup;

    QPixmap* avatar; // 记录用户的头像

    // 添加好友/添加群组/创建群组界面配置
    QDialog* addFriendDialog;
    QLabel* addFriendLabel;
    QLineEdit* addFriendInput;

    int dialogOP = -1;

    // 用户信息
    int userid = -1;
    QString username = "lth";

    // 存储时间记录
    set<node> timeset;

    // UI 设置
    void setupUI();
    void applyStyles();
    void initDialog1();
    void dealMessageTime(QDateTime dateTime, int op);
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void reloadAvatar();
    void refreshMessageLayout();

public:
    MyTcpClient* tcpclient;     // 网络模块

    bool isMenuVisible = false;
public:
    unordered_map<QString, std::pair<int, bool>> _list;  // 好友/群组列表  <名称, id, 是否群组>
    unordered_map<QString, QPixmap*> _avatars;           // 名称到头像
    unordered_map<QString, QListWidgetItem*> mp;         // 名称到表项的映射
};

#endif // MAINWINDOW_H
