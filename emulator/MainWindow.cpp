#include "MainWindow.h"
#include <QMenuBar>
#include <QApplication>
#include <QTextEdit>
#include <QCloseEvent>
#include "Common.h"

MainWindow::MainWindow()
    : QMainWindow(0, 0)
{
    QMenuBar *mb = menuBar();
    QMenu *m = mb->addMenu("&File");
    m->addAction("&Restart FSM", emulApp(), SLOT(restartFSMAndServer()));
    m->addSeparator();
    m->addAction("&Quit", qApp, SLOT(quit()));
    statusBar();

    m = mb->addMenu("&Window");
    m->addAction("&Control Window", emulApp(), SLOT(showControlWin()));
    m->addSeparator();
    m->addAction("&ProcFile Viewer Window", emulApp(), SLOT(showProcFileViewer()));
    m->addAction("&ModParms Viewer Window", emulApp(), SLOT(showModParmsViewer()));

    m = mb->addMenu("&Help");
    m->addAction("&About", emulApp(), SLOT(about()));
    m->addAction("About &Qt", emulApp(), SLOT(aboutQt()));


    QTextEdit *te = new QTextEdit(this);
    te->setUndoRedoEnabled(false);
    te->setReadOnly(true);
    setCentralWidget(te);     
    //setUnifiedTitleAndToolBarOnMac ( true );
}

QTextEdit *MainWindow::textEdit() const
{
    return centralWidget() ? dynamic_cast<QTextEdit *>(centralWidget()) : 0;
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    e->accept();
    qApp->quit();
}
