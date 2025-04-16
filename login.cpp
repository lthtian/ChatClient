#include "login.h"
#include "public.h"
#include "mainwindow.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QIcon>
#include <QGraphicsDropShadowEffect>
#include <QBuffer>

LoginWindow::LoginWindow(QWidget *parent, MyTcpClient* tc)
    : QDialog(parent), tcpclient(tc), islogin(true)
{
    setWindowTitle(QString(" "));
    setWindowIcon(QIcon(":/image/chat.png"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setFixedSize(400, 300);  // 调整窗口大小

    // Tcp响应机制
    connect(tcpclient->getSocket(), &QTcpSocket::readyRead, this, &LoginWindow::login);

    // 创建控件
    label = new QLabel(this);
    label->setText("登录");
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet("QLabel { font-size: 24px; font-weight: bold; color: #333; }");
    label->setMaximumHeight(40);

    usernameInput = new QLineEdit(this);
    usernameInput->setPlaceholderText("请输入用户名");
    usernameInput->setStyleSheet("QLineEdit {"
                                 "border: 2px solid #ccc;"
                                 "border-radius: 10px;"
                                 "padding-left: 10px;"
                                 "margin-left: 20px;"
                                 "margin-right: 20px;"
                                 "font-size: 18px;"
                                 "}");
    usernameInput->setMinimumHeight(50);

    passwordInput = new QLineEdit(this);
    passwordInput->setEchoMode(QLineEdit::Password);
    passwordInput->setPlaceholderText("请输入密码");
    passwordInput->setStyleSheet("QLineEdit {"
                                 "border: 2px solid #ccc;"
                                 "border-radius: 10px;"
                                 "padding-left: 10px;"
                                 "margin-left: 20px;"
                                 "margin-right: 20px;"
                                 "font-size: 18px;"
                                 "}"
                                 "QLineEdit::password {"
                                 "font-family: 'Courier';"
                                 "font-size: 16px;"
                                 "color: #333;"
                                 "}");
    passwordInput->setMinimumHeight(50);

    // 创建头像选择器（初始隐藏）
    avatarLabel = new CircularAvatarLabel(this);
    avatarLabel->setMinimumSize(100, 100);
    avatarLabel->setMaximumSize(100, 100);

    // 设置头像点击回调，直接打开选择器
    avatarLabel->setClickCallback([this]() {
        openAvatarSelector();
    });

    avatarLabel->hide(); // 初始隐藏

    // 设置默认头像（如果有的话）
    QPixmap defaultAvatar(":/image/default_avatar.png"); // 替换为您的默认头像路径
    if (!defaultAvatar.isNull()) {
        avatarLabel->setPixmapCircular(defaultAvatar);
    }

    loginButton = new QPushButton("确认", this);
    loginButton->setStyleSheet("QPushButton {"
                               "width: 40px;"
                               "background-color: #4CAF50;"
                               "color: white;"
                               "border-radius: 5px;"
                               "padding: 10px;"
                               "font-size: 16px;"
                               "font-weight: bold;"
                               "}"
                               "QPushButton:hover {"
                               "background-color: #45a049;"
                               "}"
                               "QPushButton:focus {"
                               "background-color: #45a049;"
                               "}"
                               "QPushButton:pressed {"
                               "background-color: #3d8b40;"
                               "}");

    // 添加阴影效果
    QGraphicsDropShadowEffect* shadowEffect = new QGraphicsDropShadowEffect(this);
    shadowEffect->setBlurRadius(15);
    shadowEffect->setColor(QColor(0, 0, 0, 100));
    shadowEffect->setOffset(0, 5);
    loginButton->setGraphicsEffect(shadowEffect);

    // 连接信号和槽
    connect(loginButton, &QPushButton::clicked, this, &LoginWindow::handleLogin);

    // 创建主布局
    mainLayout = new QVBoxLayout(this);

    // 创建输入区域布局
    inputAreaLayout = new QHBoxLayout();

    // 创建输入框容器
    inputContainer = new QVBoxLayout();
    inputContainer->addWidget(usernameInput);
    inputContainer->addSpacing(10);
    inputContainer->addWidget(passwordInput);

    // 将输入框容器添加到输入区域布局
    inputAreaLayout->addLayout(inputContainer);

    // 设置布局
    mainLayout->addWidget(label, 2);
    mainLayout->addLayout(inputAreaLayout, 6);
    mainLayout->addSpacing(10);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addSpacing(70);
    buttonLayout->addWidget(loginButton, 5);

    changeReg = new QPushButton("没有账号?去注册", this);
    changeReg->setStyleSheet("QPushButton {"
                             "color: #4CAF50;"
                             "border: none;"
                             "font-size: 14px;"
                             "}"
                             "QPushButton:hover {"
                             "color: #45a049;"
                             "}");
    connect(changeReg, &QPushButton::clicked, this, &LoginWindow::change);
    buttonLayout->addWidget(changeReg, 5);
    buttonLayout->addSpacing(60);

    mainLayout->addLayout(buttonLayout, 4);
    mainLayout->addSpacing(30);

    // 设置主布局
    setLayout(mainLayout);

    // 设置背景颜色和圆角效果
    setStyleSheet("QDialog {"
                  "background-color: #f5f5f5;"
                  "border-radius: 15px;"
                  "}");

    // 设置焦点到用户名输入框
    usernameInput->setFocus();

    // 安装事件过滤器
    usernameInput->installEventFilter(this);
    passwordInput->installEventFilter(this);
    loginButton->installEventFilter(this);
}

void LoginWindow::openAvatarSelector() {
    AvatarSelectorDialog dialog(this);

    if (dialog.exec() == QDialog::Accepted) {
        QPixmap selectedAvatar = dialog.getSelectedAvatar();
        if (!selectedAvatar.isNull()) {
            avatarLabel->setPixmapCircular(selectedAvatar);
            // 这里可以获取头像数据并准备发送到后端
            // QByteArray avatarData = getAvatarData();
        }
    }
}

bool LoginWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);

        // 如果按下回车键
        if (keyEvent->key() == Qt::Key_Return) {
            if (obj == usernameInput) {
                passwordInput->setFocus();  // 聚焦到密码输入框
                return true;  // 阻止事件继续传播
            } else if (obj == passwordInput) {
                loginButton->setFocus();  // 聚焦到登录按钮
                return true;  // 阻止事件继续传播
            }
        }
    }

    // 如果事件没有被处理，返回 false 让父类继续处理
    return QDialog::eventFilter(obj, event);
}

LoginWindow::~LoginWindow()
{
    // 析构函数, 可以进行资源释放等
}

void LoginWindow::handleLogin()
{
    QString username = usernameInput->text();
    QString password = passwordInput->text();

    if(username == "" || password == "")
    {
        label->setText("输入不能为空");
        return;
    }

    // 创建 JSON 对象并添加数据
    QJsonObject jsonObj;
    jsonObj["username"] = username;
    jsonObj["password"] = password;

    if(islogin) jsonObj["msgid"] = LoginMsg;  // 添加 msgid 属性
    else {
        jsonObj["msgid"] = RegMsg;

        // 如果是注册，添加头像数据
        if (!avatarLabel->getOriginalPixmap().isNull()) {
            // 压缩头像
            QByteArray avatarData;
            QBuffer buffer(&avatarData);
            buffer.open(QIODevice::WriteOnly);

            // 获取原始图像
            QPixmap originalPixmap = avatarLabel->getOriginalPixmap();
            QImage image = originalPixmap.toImage();

            // 调整图片大小（头像通常不需要太大）
            if (image.width() > 200 || image.height() > 200) {
                image = image.scaled(200, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }

            // 以JPEG格式保存，控制质量
            image.save(&buffer, "JPEG", 85);

            qDebug() << "原始头像大小: " << originalPixmap.toImage().sizeInBytes() << " 字节";
            qDebug() << "压缩后头像大小: " << avatarData.size() << " 字节";

            jsonObj["avatar"] = QString(avatarData.toBase64());
        }
    }

    // 转换为 JSON 字符串
    QJsonDocument jsonDoc(jsonObj);
    QByteArray jsonData = jsonDoc.toJson();  // 获取 JSON 数据的字节数组

    tcpclient->send(jsonData);
}

// 在前端上传图片前进行压缩
QByteArray LoginWindow::compressImageBeforeUpload(const QString &filePath)
{
    QImage image(filePath);
    if (image.isNull()) {
        return QByteArray();
    }

    // 调整图片大小（头像通常不需要太大）
    if (image.width() > 200 || image.height() > 200) {
        image = image.scaled(200, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // 压缩为JPEG格式
    QByteArray compressed;
    QBuffer buffer(&compressed);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 85); // 85%质量
    buffer.close();
    return compressed;
}

void LoginWindow::change()
{
    if(islogin) // 切换到注册
    {
        label->setText("注册");
        changeReg->setText("已注册?去登录");
        islogin = false;

        // 安全地修改布局
        // 1. 首先从布局中移除输入框容器，但不删除它
        inputAreaLayout->removeItem(inputContainer);

        // 2. 显示头像选择器并添加到布局
        avatarLabel->show();
        inputAreaLayout->addWidget(avatarLabel, 3);

        // 3. 调整输入框样式，使其变窄
        usernameInput->setStyleSheet("QLineEdit {"
                                    "border: 2px solid #ccc;"
                                    "border-radius: 10px;"
                                    "padding-left: 10px;"
                                    "margin-left: 10px;"
                                    "margin-right: 20px;"
                                    "font-size: 16px;"
                                    "}");

        passwordInput->setStyleSheet("QLineEdit {"
                                    "border: 2px solid #ccc;"
                                    "border-radius: 10px;"
                                    "padding-left: 10px;"
                                    "margin-left: 10px;"
                                    "margin-right: 20px;"
                                    "font-size: 16px;"
                                    "}"
                                    "QLineEdit::password {"
                                    "font-family: 'Courier';"
                                    "font-size: 14px;"
                                    "color: #333;"
                                    "}");

        // 4. 重新添加输入框容器
        inputAreaLayout->addLayout(inputContainer, 7);
    }
    else    // 切换到登录
    {
        label->setText("登录");
        changeReg->setText("没有账号?去注册");
        islogin = true;

        // 安全地修改布局
        // 1. 首先从布局中移除输入框容器和头像，但不删除它们
        inputAreaLayout->removeItem(inputContainer);
        inputAreaLayout->removeWidget(avatarLabel);

        // 2. 隐藏头像选择器
        avatarLabel->hide();

        // 3. 恢复输入框样式
        usernameInput->setStyleSheet("QLineEdit {"
                                    "border: 2px solid #ccc;"
                                    "border-radius: 10px;"
                                    "padding-left: 10px;"
                                    "margin-left: 20px;"
                                    "margin-right: 20px;"
                                    "font-size: 18px;"
                                    "}");

        passwordInput->setStyleSheet("QLineEdit {"
                                    "border: 2px solid #ccc;"
                                    "border-radius: 10px;"
                                    "padding-left: 10px;"
                                    "margin-left: 20px;"
                                    "margin-right: 20px;"
                                    "font-size: 18px;"
                                    "}"
                                    "QLineEdit::password {"
                                    "font-family: 'Courier';"
                                    "font-size: 16px;"
                                    "color: #333;"
                                    "}");

        // 4. 重新添加输入框容器
        inputAreaLayout->addLayout(inputContainer);
    }

    // 强制更新布局
    inputAreaLayout->update();
    mainLayout->update();
    update();
}

void LoginWindow::login()
{
    QByteArray responseData = tcpclient->read();  // 读取返回的数据
    QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);  // 解析 JSON 数据

    if(islogin)
    {
        if (jsonDoc.isObject()) {
            QJsonObject jsonObj = jsonDoc.object();
            int success = jsonObj["errno"].toInt();  // 获取 success 字段的值

            if (!success) {
                label->setText("登录成功");
                qDebug() << "登录成功";

                // 获取用户id, 并且向聊天窗口传递信息
                int id = jsonObj["id"].toInt();
                QString name = jsonObj["name"].toString();
                emit loginSuccess(id, name);
                accept();
            }
            else
            {
                label->setText(jsonObj["errmsg"].toString());
                usernameInput->clear();
                passwordInput->clear();
            }
        }
        else {
            qDebug() << "返回数据不是有效的 JSON 格式！";
        }
    }
    else // 注册业务
    {
        QJsonObject jsonObj = jsonDoc.object();
        int success = jsonObj["errno"].toInt();
        if (!success) {
            label->setText("注册成功");
            qDebug() << "注册成功";
        }
        else
        {
            label->setText(jsonObj["errmsg"].toString());
            usernameInput->clear();
            passwordInput->clear();
        }
    }
}
