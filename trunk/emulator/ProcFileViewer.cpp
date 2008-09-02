#include "ProcFileViewer.h"
#include <QGridLayout>
#include <QTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QTimer>
#include "kernel_emul.h"


ProcFileViewer::ProcFileViewer(QWidget *p, Qt::WindowFlags f)
    : QWidget(p, f)
{
    setWindowTitle("Emulated /proc/RealtimeFSM View");

    QGridLayout *l = new QGridLayout(this);
    
    
    te = new QTextEdit(this);
    te->setReadOnly(true);

    QPushButton *but = new QPushButton("Refresh", this);
    connect(but, SIGNAL(clicked()), this, SLOT(refreshView()));
    QCheckBox *chk = new QCheckBox("Auto refresh every 500 ms", this);
    connect(chk, SIGNAL(toggled(bool)), this, SLOT(setAutoRefresh(bool)));
    l->addWidget(but, 0, 0, Qt::AlignLeft);
    l->addWidget(chk, 0, 1, Qt::AlignLeft);
    l->addWidget(te, 1, 0, 1, 2);
    l->setColumnStretch(1, 1);
    timer = new QTimer(this);
    timer->setInterval(500);
    timer->setSingleShot(false);
    connect(timer, SIGNAL(timeout()), this, SLOT(refreshView()));
}

void ProcFileViewer::showEvent(QShowEvent *e)
{
    refreshView();
    QWidget::showEvent(e);
}

void ProcFileViewer::refreshView()
{
    te->setText(Emul::readFSMProcFile().c_str());
}

void ProcFileViewer::setAutoRefresh(bool on)
{
    if (on) timer->start();
    else timer->stop();
}
