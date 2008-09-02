#ifndef MainWindow_H
#define MainWindow_H

#include <QMainWindow>

class QTextEdit;

class MainWindow : public QMainWindow
{
 public:
    MainWindow();
    QTextEdit *textEdit() const;
 protected:
    void closeEvent(QCloseEvent *);
};

#endif
