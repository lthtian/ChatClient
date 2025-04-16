#ifndef QNCHATMESSAGE_H
#define QNCHATMESSAGE_H

#include <QWidget>
#include <QTimer>

class QPaintEvent;
class QPainter;
class QLabel;
class QMovie;

class QNChatMessage : public QWidget
{
    Q_OBJECT
public:
    // 添加新的构造函数重载，接受 QPixmap* 参数
    explicit QNChatMessage(QWidget *parent = nullptr, QPixmap* avatar = nullptr);

    enum User_Type{
        User_System,//系统
        User_Me,    //自己
        User_She,   //用户
        User_Time,  //时间
    };
    void setTextSuccess();
    void setText(QString text, QString time, QSize allSize, User_Type userType, QString name = "none", bool isgroup = false);

    // 添加设置头像的方法，以便后续更新
    void setAvatar(QPixmap* avatar);

    QSize getRealString(QString src);
    QSize fontRect(QString str);

    inline QString text() {return m_msg;}
    inline QString time() {return m_time;}
    inline User_Type userType() {return m_userType;}
    inline QString name() {return m_name;}
protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override; // 添加 leaveEvent

private slots:
    void checkMousePosition(); // 定时检查鼠标位置

private:
    QString m_msg;
    QString m_time;
    QString m_curTime;
    QString m_name;

    bool m_isgroupchat;

    QSize m_allSize;
    User_Type m_userType = User_System;

    int m_kuangWidth;
    int m_textWidth;
    int m_spaceWid;
    int m_lineHeight;

    QRect m_iconLeftRect;
    QRect m_iconRightRect;
    QRect m_sanjiaoLeftRect;
    QRect m_sanjiaoRightRect;
    QRect m_kuangLeftRect;
    QRect m_kuangRightRect;
    QRect m_textLeftRect;
    QRect m_textRightRect;
    QPixmap m_leftPixmap;
    QPixmap m_rightPixmap;
    QPixmap* m_customAvatar = nullptr; // 新增：自定义头像指针
    QLabel* m_loading = Q_NULLPTR;
    QMovie* m_loadingMovie = Q_NULLPTR;
    bool m_isSending = false;

    bool m_isHoveredRect = false;  // 标记鼠标是否在矩形区域内
    QTimer* mouseCheckTimer; // 定时器
};

#endif // QNCHATMESSAGE_H
