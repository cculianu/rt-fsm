#include "MainWindow.h"
#include <QMenuBar>
#include <QApplication>
#include <QTextEdit>
#include <QCloseEvent>
#include "Common.h"

MainWindow::MainWindow()
    : QMainWindow(0, 0)
{
    statusBar();

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
