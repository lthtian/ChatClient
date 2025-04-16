#ifndef MYLISTWIDGET_H
#define MYLISTWIDGET_H

#include <QApplication>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMouseEvent>
#include <QWidget>
#include <QPainter>
#include <QMap>
#include <QTimer>

class MainWindow;

class MyListWidget : public QListWidget {
    Q_OBJECT
public:
    MyListWidget(MainWindow* mw, QWidget *parent = nullptr)
        : QListWidget(parent), mwindow(mw), m_indicatorVisible(true) {
        // 启用自定义右键菜单
        setContextMenuPolicy(Qt::CustomContextMenu);
        connect(this, &QListWidget::customContextMenuRequested, this, &MyListWidget::showContextMenu);
        contextMenu = new QMenu();

        // 设置闪烁定时器
        m_blinkTimer = new QTimer(this);
        connect(m_blinkTimer, &QTimer::timeout, this, &MyListWidget::onBlinkTimerTimeout);

        // 设置可见和不可见的持续时间（毫秒）
        m_visibleDuration = 1200;    // 1.2秒可见
        m_invisibleDuration = 300;   // 0.3秒不可见
    }

    ~MyListWidget() {
        if (m_blinkTimer->isActive()) {
            m_blinkTimer->stop();
        }
    }

    QMenu* getMenu() { return contextMenu; }

    // 为特定项目设置整数值
    void setItemValue(QListWidgetItem* item, int value) {
        if (item) {
            // 无论值是多少，都存储在映射中
            m_itemValues[item] = value;

            // 检查是否有任何非零值的项目
            bool hasNonZeroValues = false;
            for (auto it = m_itemValues.begin(); it != m_itemValues.end(); ++it) {
                if (it.value() > 0) {
                    hasNonZeroValues = true;
                    break;
                }
            }

            // 根据是否有非零值来启动或停止定时器
            if (hasNonZeroValues) {
                if (!m_blinkTimer->isActive()) {
                    m_indicatorVisible = true;
                    m_blinkTimer->start(m_visibleDuration);
                }
            } else {
                if (m_blinkTimer->isActive()) {
                    m_blinkTimer->stop();
                    m_indicatorVisible = true;
                }
            }

            viewport()->update(); // 触发重绘
        }
    }

    // 检查项目是否有值
    bool itemHasValue(QListWidgetItem* item) const {
        return m_itemValues.contains(item);
    }

    // 获取特定项目的整数值
    int getItemValue(QListWidgetItem* item) const {
        return m_itemValues.value(item, 0); // 如果没有值，返回0
    }

    // 清除特定项目的值
    void clearItemValue(QListWidgetItem* item) {
        if (item && m_itemValues.contains(item)) {
            m_itemValues.remove(item);

            // 检查是否有任何非零值的项目
            bool hasNonZeroValues = false;
            for (auto it = m_itemValues.begin(); it != m_itemValues.end(); ++it) {
                if (it.value() > 0) {
                    hasNonZeroValues = true;
                    break;
                }
            }

            // 如果没有非零值，停止定时器
            if (!hasNonZeroValues && m_blinkTimer->isActive()) {
                m_blinkTimer->stop();
                m_indicatorVisible = true; // 重置可见性状态
            }

            viewport()->update();
        }
    }

    // 清除所有项目的值
    void clearAllValues() {
        m_itemValues.clear();

        // 停止定时器
        if (m_blinkTimer->isActive()) {
            m_blinkTimer->stop();
            m_indicatorVisible = true; // 重置可见性状态
        }

        viewport()->update();
    }

    // 设置闪烁间隔（可见和不可见的持续时间，单位：毫秒）
    void setBlinkDurations(int visibleMs, int invisibleMs) {
        m_visibleDuration = visibleMs;
        m_invisibleDuration = invisibleMs;

        // 如果定时器正在运行，重新启动以应用新的间隔
        if (m_blinkTimer->isActive()) {
            m_blinkTimer->stop();
            m_indicatorVisible = true;
            m_blinkTimer->start(m_visibleDuration);
        }
    }

    // 启用或禁用闪烁效果
    void enableBlinking(bool enable) {
        // 检查是否有任何非零值的项目
        bool hasNonZeroValues = false;
        for (auto it = m_itemValues.begin(); it != m_itemValues.end(); ++it) {
            if (it.value() > 0) {
                hasNonZeroValues = true;
                break;
            }
        }

        if (enable && hasNonZeroValues) {
            if (!m_blinkTimer->isActive()) {
                m_indicatorVisible = true;
                m_blinkTimer->start(m_visibleDuration);
            }
        } else {
            if (m_blinkTimer->isActive()) {
                m_blinkTimer->stop();
                m_indicatorVisible = true; // 重置可见性状态
                viewport()->update();
            }
        }
    }

protected:
    // 重写绘制事件以绘制圆圈和数字
    void paintEvent(QPaintEvent* event) override {
        // 首先让基类正常绘制列表部件
        QListWidget::paintEvent(event);

        // 如果指示器当前可见，为每个有非零值的项目绘制圆圈和数字
        if (m_indicatorVisible) {
            QPainter painter(viewport());
            painter.setRenderHint(QPainter::Antialiasing);

            // 遍历所有可见项目
            for (int i = 0; i < count(); ++i) {
                QListWidgetItem* item = this->item(i);
                if (m_itemValues.contains(item) && m_itemValues[item] > 0) { // 只绘制值大于0的项目
                    // 获取项目的矩形区域
                    QRect itemRect = visualItemRect(item);

                    // 计算圆圈位置（项目右侧）
                    int circleSize = 24; // 圆圈直径
                    int margin = 5;      // 右边缘的边距
                    QRect circleRect(
                        itemRect.right() - circleSize - margin,
                        itemRect.top() + (itemRect.height() - circleSize) / 2,
                        circleSize,
                        circleSize
                    );

                    // 绘制金色圆圈
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QBrush("#FFD700"));
                    painter.drawEllipse(circleRect);

                    // 绘制白色文本
                    painter.setPen(Qt::white);
                    painter.setFont(QFont("Arial", 10, QFont::Bold));
                    painter.drawText(circleRect, Qt::AlignCenter, QString::number(m_itemValues[item]));
                }
            }
        }
    }

private slots:
    void showContextMenu(const QPoint &pos);

    // 处理闪烁定时器超时的槽
    void onBlinkTimerTimeout() {
        // 切换指示器可见性
        m_indicatorVisible = !m_indicatorVisible;

        // 根据当前状态设置下一个定时器间隔
        if (m_indicatorVisible) {
            // 如果现在变为可见，下一个间隔应该是可见持续时间
            m_blinkTimer->start(m_visibleDuration);
        } else {
            // 如果现在变为不可见，下一个间隔应该是不可见持续时间
            m_blinkTimer->start(m_invisibleDuration);
        }

        viewport()->update(); // 触发重绘
    }

signals:
    void menuHidden(); // 自定义信号

private:
    MainWindow* mwindow;
    QMenu* contextMenu;
    QMap<QListWidgetItem*, int> m_itemValues; // 存储每个项目的整数值
    QTimer* m_blinkTimer;                     // 控制闪烁的定时器
    bool m_indicatorVisible;                  // 指示器当前是否可见
    int m_visibleDuration;                    // 指示器可见的持续时间（毫秒）
    int m_invisibleDuration;                  // 指示器不可见的持续时间（毫秒）
};

#endif // MYLISTWIDGET_H
