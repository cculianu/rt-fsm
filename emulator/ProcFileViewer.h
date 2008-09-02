#ifndef ProcFileViewer_H
#define ProcFileViewer_H

class QTextEdit;
class QTimer;

#include <QWidget>
#include "EmulApp.h"

class ProcFileViewer : public QWidget
{
    Q_OBJECT
public:
    ProcFileViewer(QWidget *parent = 0, Qt::WindowFlags f = 0);
protected:
    void showEvent(QShowEvent *);
protected slots:
    void refreshView();
    void setAutoRefresh(bool on);

private:
    QTextEdit *te;
    QTimer *timer;
};

#endif
