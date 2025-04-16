#include "qnchatmessage.h"
#include <QFontMetrics>
#include <QPaintEvent>
#include <QDateTime>
#include <QPainter>
#include <QMovie>
#include <QLabel>
#include <QDebug>

QNChatMessage::QNChatMessage(QWidget *parent, QPixmap* avatar) : QWidget(parent), m_customAvatar(avatar)
{
    QFont te_font = this->font();
    te_font.setFamily("MicrosoftYaHei");
    te_font.setPointSize(12);
    this->setFont(te_font);

    // 仍然加载默认头像，以便在没有提供自定义头像时使用
    m_leftPixmap = QPixmap(":/image/Customer Copy.png");
    m_rightPixmap = QPixmap(":/image/CustomerService.png");

    m_loadingMovie = new QMovie(this);
    m_loadingMovie->setFileName(":/image/loading4.gif");
    m_loading = new QLabel(this);
    m_loading->setMovie(m_loadingMovie);
    m_loading->resize(16,16);
    m_loading->setAttribute(Qt::WA_TranslucentBackground , true);
    m_loading->setAutoFillBackground(false);

    // 强制设置初始宽度
    this->setFixedWidth(parent->width()); // 确保初始宽度与父控件一致

    this->setMouseTracking(true);

    // 初始化定时器
    mouseCheckTimer = new QTimer(this);
    connect(mouseCheckTimer, &QTimer::timeout, this, &QNChatMessage::checkMousePosition);
    mouseCheckTimer->start(100); // 每 100ms 检查一次鼠标位置
}

void QNChatMessage::setAvatar(QPixmap* avatar)
{
    m_customAvatar = avatar;
    update(); // 更新界面以显示新头像
}

void QNChatMessage::setTextSuccess()
{
    m_loading->hide();
    m_loadingMovie->stop();
    m_isSending = true;
}

void QNChatMessage::setText(QString text, QString time, QSize allSize, QNChatMessage::User_Type userType, QString name, bool isgroup)
{
    m_msg = text;
    m_userType = userType;
    m_time = time;
    m_allSize = allSize;
    m_name = name;
    m_isgroupchat = isgroup;

    if(userType == User_Time)
    {
        if(text == QString("a")) m_curTime = QDateTime::fromTime_t(time.toInt()).toString("yyyy-MM-dd");
        else m_curTime = QDateTime::fromTime_t(time.toInt()).toString("hh:mm");
    }

    // 强制计算字体矩形区域，确保布局正确
    fontRect(m_msg);

    if(userType == User_Me) {
        if(!m_isSending) {
            m_loading->move(m_kuangRightRect.x() - m_loading->width() - 10, m_kuangRightRect.y()+m_kuangRightRect.height()/2- m_loading->height()/2);
            m_loading->show();
            m_loadingMovie->start();
        }
    } else {
        m_loading->hide();
    }

    this->update();
}

QSize QNChatMessage::fontRect(QString str)
{
    m_msg = str;
    int minHei = 30;
    int iconWH = 40;
    int iconSpaceW = 20;
    int iconRectW = 5;
    int iconTMPH = 10;
    int sanJiaoW = 6;
    int kuangTMP = 20;
    int textSpaceRect = 12;
    m_kuangWidth = this->width() - kuangTMP - 2*(iconWH+iconSpaceW+iconRectW);
    m_textWidth = m_kuangWidth - 2*textSpaceRect;
    m_spaceWid = this->width() - m_textWidth;
    m_iconLeftRect = QRect(iconSpaceW, iconTMPH, iconWH, iconWH);
    m_iconRightRect = QRect(this->width() - iconSpaceW - iconWH, iconTMPH, iconWH, iconWH);

    QSize size = getRealString(m_msg); // 整个的size

    int hei = size.height() < minHei ? minHei : size.height();

    // 计算基本位置（不考虑群组聊天的情况）
    int baseYPosition = m_lineHeight/2;
    int baseKuangYPosition = m_lineHeight/4*3;

    // 如果是群组聊天中的她发的消息，则下移气泡位置
    int nameSpaceHeight = 0;
    if (m_userType == User_Type::User_She && m_isgroupchat) {
        nameSpaceHeight = 20; // 为用户名预留的高度
    }

    m_sanjiaoLeftRect = QRect(iconWH+iconSpaceW+iconRectW, baseYPosition + nameSpaceHeight, sanJiaoW, hei - m_lineHeight);
    m_sanjiaoRightRect = QRect(this->width() - iconRectW - iconWH - iconSpaceW - sanJiaoW, baseYPosition, sanJiaoW, hei - m_lineHeight);

    if(size.width() < (m_textWidth+m_spaceWid)) {
        m_kuangLeftRect.setRect(m_sanjiaoLeftRect.x()+m_sanjiaoLeftRect.width(),
                               baseKuangYPosition + nameSpaceHeight, // 为群组聊天添加额外空间
                               size.width()-m_spaceWid+2*textSpaceRect,
                               hei-m_lineHeight);
        m_kuangRightRect.setRect(this->width() - size.width() + m_spaceWid - 2*textSpaceRect - iconWH - iconSpaceW - iconRectW - sanJiaoW,
                                 baseKuangYPosition, size.width()-m_spaceWid+2*textSpaceRect, hei-m_lineHeight);
    } else {
        m_kuangLeftRect.setRect(m_sanjiaoLeftRect.x()+m_sanjiaoLeftRect.width(),
                               baseKuangYPosition + nameSpaceHeight, // 为群组聊天添加额外空间
                               m_kuangWidth,
                               hei-m_lineHeight);
        m_kuangRightRect.setRect(iconWH + kuangTMP + iconSpaceW + iconRectW - sanJiaoW, baseKuangYPosition, m_kuangWidth, hei-m_lineHeight);
    }

    // 文本区域也需要下移
    m_textLeftRect.setRect(m_kuangLeftRect.x()+textSpaceRect,
                          m_kuangLeftRect.y()+iconTMPH,
                          m_kuangLeftRect.width()-2*textSpaceRect,
                          m_kuangLeftRect.height()-2*iconTMPH);
    m_textRightRect.setRect(m_kuangRightRect.x()+textSpaceRect,
                           m_kuangRightRect.y()+iconTMPH,
                           m_kuangRightRect.width()-2*textSpaceRect,
                           m_kuangRightRect.height()-2*iconTMPH);

    // 如果是群组聊天中的其他用户消息，增加总高度以容纳用户名
    if (m_userType == User_Type::User_She && m_isgroupchat) {
        return QSize(size.width(), hei + nameSpaceHeight);
    }

    return QSize(size.width(), hei);
}

QSize QNChatMessage::getRealString(QString src)
{
    QFontMetricsF fm(this->font());
    m_lineHeight = fm.lineSpacing();
    int nCount = src.count("\n");
    int nMaxWidth = 0;
    if(nCount == 0) {
        nMaxWidth = fm.width(src);
        QString value = src;
        if(nMaxWidth > m_textWidth) {
            nMaxWidth = m_textWidth;
            int size = m_textWidth / fm.width(" ");
            int num = fm.width(value) / m_textWidth;
            //int ttmp = num*fm.width(" ");
            num = ( fm.width(value) ) / m_textWidth;
            nCount += num;
            QString temp = "";
            for(int i = 0; i < num; i++) {
                temp += value.mid(i*size, (i+1)*size) + "\n";
            }
            src.replace(value, temp);
        }
    } else {
        for(int i = 0; i < (nCount + 1); i++) {
            QString value = src.split("\n").at(i);
            nMaxWidth = fm.width(value) > nMaxWidth ? fm.width(value) : nMaxWidth;
            if(fm.width(value) > m_textWidth) {
                nMaxWidth = m_textWidth;
                int size = m_textWidth / fm.width(" ");
                int num = fm.width(value) / m_textWidth;
                num = ((i+num)*fm.width(" ") + fm.width(value)) / m_textWidth;
                nCount += num;
                QString temp = "";
                for(int i = 0; i < num; i++) {
                    temp += value.mid(i*size, (i+1)*size) + "\n";
                }
                src.replace(value, temp);
            }
        }
    }
    return QSize(nMaxWidth+m_spaceWid, (nCount + 1) * m_lineHeight+2*m_lineHeight);
}

void QNChatMessage::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform); // 消锯齿
    painter.setPen(Qt::NoPen);

    if (m_userType == User_Type::User_She) { // 用户
        // 头像 - 使用自定义头像或默认头像
        if (m_customAvatar && !m_customAvatar->isNull()) {
            painter.drawPixmap(m_iconLeftRect, *m_customAvatar);
        } else {
            painter.drawPixmap(m_iconLeftRect, m_leftPixmap);
        }

        // 检查是否为群组聊天且需要显示用户名
        if (m_isgroupchat) {
            // 为用户名创建一个较小的字体
            QFont nameFont = this->font();
            nameFont.setPointSize(nameFont.pointSize() - 2); // 字体大小小一些
            painter.setFont(nameFont);

            // 设置用户名的颜色
            QPen namePen;
            namePen.setColor(QColor(102, 102, 102)); // 灰色文本
            painter.setPen(namePen);

            // 计算用户名的位置 - 在头像旁边，气泡上方
            QRect nameRect(m_sanjiaoLeftRect.x() + m_sanjiaoLeftRect.width(),
                          m_kuangLeftRect.y() - 20, // 在气泡上方的空间
                          m_kuangLeftRect.width() + 200,
                          20);

            // 绘制用户名
            painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignBottom, m_name);

            // 重置为默认字体
            painter.setFont(this->font());
            painter.setPen(Qt::NoPen);
        }

        // 框加边
        QColor col_KuangB(234, 234, 234);
        painter.setBrush(QBrush(col_KuangB));
        painter.drawRoundedRect(m_kuangLeftRect.x() - 1, m_kuangLeftRect.y() - 1, m_kuangLeftRect.width() + 2, m_kuangLeftRect.height() + 2, 4, 4);

        // 框
        QColor col_Kuang(255, 255, 255);
        if (m_isHoveredRect) {
            col_Kuang = QColor(245, 245, 245); // 悬浮时颜色变深
        }
        painter.setBrush(QBrush(col_Kuang));
        painter.drawRoundedRect(m_kuangLeftRect, 4, 4);

        // 三角 - 需要根据是否为群组聊天调整位置
        int triangleYBase = 30;
        if (m_isgroupchat) {
            triangleYBase += 20; // 如果是群组聊天，三角形也下移20px
        }

        QPointF points[3] = {
            QPointF(m_sanjiaoLeftRect.x(), triangleYBase),
            QPointF(m_sanjiaoLeftRect.x() + m_sanjiaoLeftRect.width(), triangleYBase - 5),
            QPointF(m_sanjiaoLeftRect.x() + m_sanjiaoLeftRect.width(), triangleYBase + 5),
        };
        QPen pen;
        pen.setColor(col_Kuang);
        painter.setPen(pen);
        painter.drawPolygon(points, 3);

        // 三角加边 - 同样需要调整位置
        QPen penSanJiaoBian;
        penSanJiaoBian.setColor(col_KuangB);
        painter.setPen(penSanJiaoBian);
        painter.drawLine(QPointF(m_sanjiaoLeftRect.x() - 1, triangleYBase),
                        QPointF(m_sanjiaoLeftRect.x() + m_sanjiaoLeftRect.width(), triangleYBase - 6));
        painter.drawLine(QPointF(m_sanjiaoLeftRect.x() - 1, triangleYBase),
                        QPointF(m_sanjiaoLeftRect.x() + m_sanjiaoLeftRect.width(), triangleYBase + 6));

        // 内容
        QPen penText;
        penText.setColor(QColor(51, 51, 51));
        painter.setPen(penText);
        QTextOption option(Qt::AlignLeft | Qt::AlignVCenter);
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        painter.setFont(this->font());
        painter.drawText(m_textLeftRect, m_msg, option);
    } else if (m_userType == User_Type::User_Me) { // 自己
        // 头像 - 使用自定义头像或默认头像
        if (m_customAvatar && !m_customAvatar->isNull()) {
            painter.drawPixmap(m_iconRightRect, *m_customAvatar);
        } else {
            painter.drawPixmap(m_iconRightRect, m_rightPixmap);
        }

        // 框
        QColor col_Kuang("#07c160");
        if (m_isHoveredRect) {
            col_Kuang = QColor("#05a050"); // 悬浮时颜色变深
        }
        painter.setBrush(QBrush(col_Kuang));
        painter.drawRoundedRect(m_kuangRightRect, 4, 4);

        // 三角
        QPointF points[3] = {
            QPointF(m_sanjiaoRightRect.x() + m_sanjiaoRightRect.width(), 30),
            QPointF(m_sanjiaoRightRect.x(), 25),
            QPointF(m_sanjiaoRightRect.x(), 35),
        };
        QPen pen;
        pen.setColor(col_Kuang);
        painter.setPen(pen);
        painter.drawPolygon(points, 3);

        // 内容
        QPen penText;
        penText.setColor(Qt::white);
        painter.setPen(penText);
        QTextOption option(Qt::AlignLeft | Qt::AlignVCenter);
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        painter.setFont(this->font());
        painter.drawText(m_textRightRect, m_msg, option);
    } else if (m_userType == User_Type::User_Time) { // 时间
        QPen penText;
        penText.setColor(QColor(153, 153, 153));
        painter.setPen(penText);
        QTextOption option(Qt::AlignCenter);
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        QFont te_font = this->font();
        te_font.setFamily("MicrosoftYaHei");
        te_font.setPointSize(10);
        painter.setFont(te_font);
        painter.drawText(this->rect(), m_curTime, option);
    }
}

void QNChatMessage::mouseMoveEvent(QMouseEvent *event)
{
    QPoint mousePos = event->pos();  // 获取鼠标在控件内的位置
    // 判断鼠标是否在矩形区域内
    if (m_userType == User_She && m_kuangLeftRect.contains(mousePos)) {
        if (!m_isHoveredRect) {
            m_isHoveredRect = true;
            update();  // 触发重绘
        }
    } else if (m_userType == User_Me && m_kuangRightRect.contains(mousePos)) {
        if (!m_isHoveredRect) {
            m_isHoveredRect = true;
            update();  // 触发重绘
        }
    } else {
        if (m_isHoveredRect) {
            m_isHoveredRect = false;
            update();  // 触发重绘
        }
    }

    QWidget::mouseMoveEvent(event);  // 调用基类的事件处理
}

void QNChatMessage::leaveEvent(QEvent *event)
{
    // 鼠标离开控件时恢复状态
    if (m_isHoveredRect) {
        m_isHoveredRect = false;
        update();  // 触发重绘
    }

    QWidget::leaveEvent(event);  // 调用基类的事件处理
}

void QNChatMessage::checkMousePosition()
{
    // 获取鼠标的全局位置
    QPoint globalMousePos = QCursor::pos();

    // 将全局位置转换为控件内的位置
    QPoint localMousePos = this->mapFromGlobal(globalMousePos);

    // 判断鼠标是否在控件内
    if (!this->rect().contains(localMousePos)) {
        // 鼠标不在控件内，恢复状态
        if (m_isHoveredRect) {
            m_isHoveredRect = false;
            update();  // 触发重绘
        }
    }
}
