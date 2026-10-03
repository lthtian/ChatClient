#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QPushButton>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QLabel>
#include <QJsonArray>
#include <QPointer>
#include <QSet>
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

class MediaTransfer;
class MediaWindow;
class QProgressDialog;
class QCloseEvent;

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
    void sendVideo(); // 选择一个 MP4，发送到开始操作时的会话。


private:
    // UI 元素
    QListWidget* messageList;  // 用于显示聊天消息
    QLabel* msgListLabel;      // 显示当前聊天对象
    MyTextEdit *messageInput;   // 用于输入消息
    QPushButton *sendButton;   // 用于发送消息
    QPushButton* videoButton; // 选择并上传 MP4 的入口。
    MyListWidget* contactList;
    QPushButton* addFriend;
    QPushButton* addGroup;
    QPushButton* exitButton;
    QPushButton* createGroup;

    QPixmap avatar; // 记录用户的头像

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
    void closeEvent(QCloseEvent* event) override; // 关闭播放器、文件任务和登录连接。

    // 文件业务负责聊天权限与传输；播放器接收本地路径或经过验证的在线输入。
    void setupMedia(); // 连接传输状态、文件消息与播放器。
    MediaWindow* ensurePlayer(); // 创建或显示播放器，两种输入共用同一窗口。
    void playOnlineVideo(const QJsonObject& conversation, const QJsonObject& media); // 绑定本条消息的授权回调。
    QJsonObject currentConversation() const; // 当前联系人对应的目标 ID 与群聊标记。
    QString conversationName(const QJsonObject& conversation) const; // 按 ID 查界面名称。
    void requestMediaHistory(const QJsonObject& conversation, const QString& before, quint64 generation); // 分页取得含媒体元数据的历史。
    void handleMediaMessage(const QJsonObject& conversation, const QJsonObject& message); // 实时通知与发送确认共用入口。
    void appendVideoMessage(const QJsonObject& conversation, const QJsonObject& message); // 显示文件名、大小及下载播放按钮。
    void showMediaHistory(const QJsonObject& conversation); // 将已收齐的记录交给消息展示。

    MediaTransfer* media_ = nullptr; // 拥有控制请求及单个文件传输任务。
    QProgressDialog* transferProgress_ = nullptr; // 上传下载的进度和取消入口。
    QPointer<MediaWindow> player_; // 同进程播放器，关闭窗口后指针自动清空。
    QSet<QString> displayedMedia_; // 当前会话已经显示的文件消息编号，避免重复。
    QJsonArray mediaHistory_; // 当前分页查询已取得的结构化消息。
    QJsonArray pendingMedia_; // 历史查询期间到达的实时媒体消息，历史展示后再追加。
    bool historyLoading_ = false; // 是否正在加载当前会话历史。
    quint64 historyGeneration_ = 0; // 切换会话时拒收此前的历史查询结果。

    // 消息处理方法（由 recvHandler 分发）
    void handleInitMsgAck(const QJsonObject& jsonObj);
    void handleChatMsg(int msgid, const QJsonObject& jsonObj);
    void handleAddFriendAck(const QJsonObject& jsonObj);
    void handleAddGroupAck(const QJsonObject& jsonObj);
    void handleCreateGroupAck(const QJsonObject& jsonObj);
    void handleHistoryMsgAck(const QJsonObject& jsonObj);
    void handleNewMsgAck(const QJsonObject& jsonObj);
    void handleImageReqAck(const QJsonObject& jsonObj);

public:
    MyTcpClient* tcpclient;     // 网络模块

    bool isMenuVisible = false;
public:
    unordered_map<QString, std::pair<int, bool>> _list;  // 好友/群组列表  <名称, id, 是否群组>
    unordered_map<QString, QPixmap> _avatars;            // 名称到头像
    unordered_map<QString, QListWidgetItem*> mp;         // 名称到表项的映射
};

#endif // MAINWINDOW_H
