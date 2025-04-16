#ifndef MYDIALOG_H
#define MYDIALOG_H
#include <QDialog>


class MyDialog : public QDialog
{
    Q_OBJECT
public:
    MyDialog();
    void init();
};

#endif // MYDIALOG_H
