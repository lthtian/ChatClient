#ifndef MYTEXTEDIT_H
#define MYTEXTEDIT_H
#include <QTextEdit>
#include <QKeyEvent>

class MyTextEdit : public QTextEdit {
    Q_OBJECT

public:
    MyTextEdit(QWidget *parent = nullptr) : QTextEdit(parent) {}

protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            // 处理回车事件
            emit returnPressed();  // 自定义信号，可以在外部连接
        } else {
            // 处理其他按键
            QTextEdit::keyPressEvent(event);
        }
    }

signals:
    void returnPressed();  // 自定义信号
};

#endif // MYTEXTEDIT_H
