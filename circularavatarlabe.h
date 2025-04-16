#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QBuffer>
#include <QByteArray>
#include <QMessageBox>
#include <QDialog>
#include <QFrame>
#include <QScrollArea>
#include <QMouseEvent>
#include <functional>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>

// 自定义圆形头像标签类
class CircularAvatarLabel : public QLabel {
public:
    CircularAvatarLabel(QWidget *parent = nullptr, bool isSelectable = false)
        : QLabel(parent), m_isSelectable(isSelectable), m_isSelected(false), m_isHovered(false) {
        setMinimumSize(80, 80);
        setMaximumSize(80, 80);
        setAlignment(Qt::AlignCenter);

        if (isSelectable) {
            setText("上传\n头像");
            setStyleSheet("font-size: 12px; color: #666666;");
        }

        setCursor(Qt::PointingHandCursor);

        // 启用鼠标跟踪以检测悬停
        setMouseTracking(true);
    }

    void setPixmapCircular(const QPixmap &pixmap) {
        if (pixmap.isNull()) {
            m_originalPixmap = QPixmap();
            update();
            return;
        }

        // 保存原始图片
        m_originalPixmap = pixmap;
        update();
    }

    QPixmap getOriginalPixmap() const {
        return m_originalPixmap;
    }

    void setSelected(bool selected) {
        m_isSelected = selected;
        update();
    }

    bool isSelected() const {
        return m_isSelected;
    }

    bool isSelectable() const {
        return m_isSelectable;
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        // 计算圆形区域
        int diameter = qMin(width(), height());
        QRect circleRect = QRect((width() - diameter) / 2, (height() - diameter) / 2, diameter, diameter);

        // 绘制圆形
        if (m_originalPixmap.isNull()) {
            // 如果没有图片，绘制虚线圆形边框
            painter.setPen(QPen(QColor("#CCCCCC"), 2, m_isSelectable ? Qt::DashLine : Qt::SolidLine));
            painter.setBrush(QColor("#F0F0F0"));
            painter.drawEllipse(circleRect);

            // 绘制文本
            if (m_isSelectable) {
                painter.setPen(QColor("#666666"));
                painter.drawText(rect(), Qt::AlignCenter, text());
            }
        } else {
            // 创建圆形裁剪路径
            QPainterPath path;
            path.addEllipse(circleRect);
            painter.setClipPath(path);

            // 缩放图片以填充圆形区域
            QPixmap scaledPixmap = m_originalPixmap.scaled(
                diameter, diameter,
                Qt::KeepAspectRatioByExpanding,
                Qt::SmoothTransformation
            );

            // 计算居中位置
            int x = (diameter - scaledPixmap.width()) / 2 + circleRect.x();
            int y = (diameter - scaledPixmap.height()) / 2 + circleRect.y();

            // 绘制图片
            painter.drawPixmap(x, y, scaledPixmap);

            // 如果鼠标悬停，绘制半透明覆盖层和编辑图标
            if (m_isHovered) {
                painter.setClipping(true); // 确保覆盖层也是圆形的
                painter.fillRect(QRect(x, y, scaledPixmap.width(), scaledPixmap.height()),
                                QColor(0, 0, 0, 80)); // 半透明黑色

                // 绘制编辑图标或文字
                painter.setPen(Qt::white);
                painter.setFont(QFont("Arial", 12, QFont::Bold));
                painter.drawText(rect(), Qt::AlignCenter, "编辑");
            }

            // 绘制圆形边框
            painter.setClipping(false);

            // 如果被选中，绘制高亮边框
            if (m_isSelected) {
                painter.setPen(QPen(QColor("#4A90E2"), 3, Qt::SolidLine));
            } else {
                painter.setPen(QPen(QColor("#CCCCCC"), 2, Qt::SolidLine));
            }

            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(circleRect);
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        // 不使用信号槽，而是直接调用回调函数
        if (m_clickCallback) {
            m_clickCallback();
        }
        QLabel::mousePressEvent(event);
    }

    void enterEvent(QEvent *event) override {
        m_isHovered = true;
        update();
        QLabel::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override {
        m_isHovered = false;
        update();
        QLabel::leaveEvent(event);
    }

public:
    // 使用函数指针代替信号
    typedef std::function<void()> ClickCallback;
    void setClickCallback(ClickCallback callback) {
        m_clickCallback = callback;
    }

private:
    QPixmap m_originalPixmap;
    bool m_isSelectable;
    bool m_isSelected;
    bool m_isHovered;
    ClickCallback m_clickCallback;
};

// 头像选择对话框
class AvatarSelectorDialog : public QDialog {
public:
    AvatarSelectorDialog(QWidget *parent = nullptr) : QDialog(parent) {
        setWindowTitle("选择头像");
        setMinimumSize(400, 300);
        setModal(true);

        // 创建主布局
        QVBoxLayout *mainLayout = new QVBoxLayout(this);

        // 创建标题
        QLabel *titleLabel = new QLabel("请选择您的头像", this);
        titleLabel->setStyleSheet("font-size: 16px; font-weight: bold; margin-bottom: 10px;");
        titleLabel->setAlignment(Qt::AlignCenter);

        // 创建头像网格布局
        QGridLayout *avatarGrid = new QGridLayout();
        avatarGrid->setSpacing(15);

        // 创建头像标签数组
        for (int i = 0; i < 8; i++) {
            bool isCustomUpload = (i == 7); // 第8个是自定义上传
            CircularAvatarLabel *avatarLabel = new CircularAvatarLabel(this, isCustomUpload);

            // 为前7个设置预设头像
            if (!isCustomUpload) {
                // 这里使用预设的头像图片，您可以替换为您自己的图片路径
                QString avatarPath = QString(":/avatars/avatar%1.png").arg(i + 1);

                // 如果资源文件不存在，使用占位图
                QPixmap avatarPixmap;
                if (!avatarPixmap.load(avatarPath)) {
                    // 创建一个彩色占位图
                    avatarPixmap = createColorAvatar(i);
                }

                avatarLabel->setPixmapCircular(avatarPixmap);
            }

            // 使用函数指针代替信号连接
            if (isCustomUpload) {
                avatarLabel->setClickCallback([this]() {
                    selectCustomImage();
                });
            } else {
                avatarLabel->setClickCallback([this, avatarLabel]() {
                    selectPresetAvatar(avatarLabel);
                });
            }

            // 添加到网格
            avatarGrid->addWidget(avatarLabel, i / 4, i % 4, Qt::AlignCenter);
            m_avatarLabels.append(avatarLabel);
        }

        // 创建一个容器来包含网格，并添加一些内边距
        QWidget *gridContainer = new QWidget(this);
        gridContainer->setLayout(avatarGrid);
        gridContainer->setStyleSheet("background-color: #F9F9F9; border-radius: 8px; padding: 20px;");

        // 创建按钮布局
        QHBoxLayout *buttonLayout = new QHBoxLayout();
        buttonLayout->setSpacing(10);

        // 创建确定和取消按钮
        QPushButton *confirmButton = new QPushButton("确定", this);
        confirmButton->setStyleSheet("padding: 8px 20px; background-color: #07c160; color: white; border-radius: 4px;");

        QPushButton *cancelButton = new QPushButton("取消", this);
        cancelButton->setStyleSheet("padding: 8px 20px; border: 1px solid #CCCCCC; border-radius: 4px;");

        // 连接按钮信号
        connect(confirmButton, &QPushButton::clicked, this, &AvatarSelectorDialog::accept);
        connect(cancelButton, &QPushButton::clicked, this, &AvatarSelectorDialog::reject);

        // 添加按钮到布局
        buttonLayout->addStretch();
        buttonLayout->addWidget(cancelButton);
        buttonLayout->addWidget(confirmButton);

        // 添加所有元素到主布局
        mainLayout->addWidget(titleLabel);
        mainLayout->addWidget(gridContainer, 1);
        mainLayout->addLayout(buttonLayout);

        // 设置对话框样式
        setStyleSheet("QDialog { background-color: white; }");
    }

    QPixmap getSelectedAvatar() const {
        for (auto label : m_avatarLabels) {
            if (label->isSelected()) {
                return label->getOriginalPixmap();
            }
        }
        return QPixmap();
    }

private:
    void selectPresetAvatar(CircularAvatarLabel *selectedLabel) {
        // 取消所有其他头像的选中状态
        for (auto label : m_avatarLabels) {
            label->setSelected(label == selectedLabel);
        }
    }

    void selectCustomImage() {
        QString fileName = QFileDialog::getOpenFileName(this,
            tr("选择头像图片"),
            QDir::homePath(),
            tr("图片文件 (*.png *.jpg *.jpeg *.bmp *.gif)"));

        if (!fileName.isEmpty()) {
            QPixmap pixmap(fileName);
            if (pixmap.isNull()) {
                QMessageBox::warning(this, tr("错误"), tr("无法加载图片!"));
                return;
            }

            // 设置自定义头像
            CircularAvatarLabel *customLabel = m_avatarLabels.last();
            customLabel->setPixmapCircular(pixmap);

            // 选中该头像
            selectPresetAvatar(customLabel);
        }
    }

    // 创建彩色占位头像
    QPixmap createColorAvatar(int index) {
        // 创建一些不同颜色的头像
        QColor colors[] = {
            QColor("#FF5252"), // 红色
            QColor("#FF9800"), // 橙色
            QColor("#FFEB3B"), // 黄色
            QColor("#4CAF50"), // 绿色
            QColor("#2196F3"), // 蓝色
            QColor("#673AB7"), // 紫色
            QColor("#E91E63")  // 粉色
        };

        QPixmap pixmap(80, 80);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);

        // 使用索引对应的颜色
        painter.setBrush(colors[index % 7]);
        painter.setPen(Qt::NoPen);

        // 绘制圆形
        painter.drawEllipse(0, 0, 80, 80);

        // 添加文字（首字母或数字）
        painter.setPen(Qt::white);
        painter.setFont(QFont("Arial", 30, QFont::Bold));
        painter.drawText(pixmap.rect(), Qt::AlignCenter, QString::number(index + 1));

        return pixmap;
    }

    QList<CircularAvatarLabel*> m_avatarLabels;
};

