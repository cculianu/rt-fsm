#ifndef ComediView_H
#define ComediView_H

#include <QWidget>
#include <QString>
#include "ComediEmul.h"
#include <QList>
#include <QToolButton>
#include <QPalette>
#include <QColor>
class QLCDNumber;

class AbstractComediView : public QWidget
{
    Q_OBJECT
public:
    AbstractComediView(const QString & devfile,
                       int subdevtype,
                       QWidget *parent = 0, Qt::WindowFlags f = 0);
    virtual ~AbstractComediView();
    unsigned subDev() const { return subdev; }
public slots:
    virtual void refreshView() = 0;
protected:
    ComediEmul *c;
    int sdtype, subdev;
};

class ComediAIView : public AbstractComediView
{
    Q_OBJECT

public:
    ComediAIView(const QString & devfile, double ailow, double aihi, QWidget *parent = 0, Qt::WindowFlags f = 0);

    double aiLow() const { return ailow; }
    double aiHigh() const { return aihigh; }

public slots:    
    /// re-reads all data from comedi and populates view/controls
    void refreshView();

private slots:
    void aiLowChanged(const QString &);
    void aiHighChanged(const QString &);
    void buttonToggle(bool);

private:
    void clear();
    double ailow, aihigh;
    QList<QToolButton *> aibuttons;
    QPalette hipal, normalpal;
    bool didpalinit;
};


class DIOLed : public QWidget
{
    Q_OBJECT
public:
    DIOLed(int sd, int chan, QWidget *parent = 0, Qt::WindowFlags f = 0);
    ~DIOLed();
    int chan() const { return ch; }
    void setChan(int chan) { ch = chan; }
    int subDev() const { return sd; }
    void setSubDev(int s) { sd = s; }
    const QColor & highColor() const { return high; }
    const QColor & lowColor() const { return low; }
    bool state() const { return ishigh; }
    void setHighColor(const QColor & c) { high = c; repaint(); }
    void setLowColor(const QColor & c) { low = c; repaint(); }
public slots:
    void setState(bool high);
protected:
    void paintEvent(QPaintEvent *);
    void mousePressEvent(QMouseEvent *);
signals:
    void clicked();
private:
    int ch, sd;
    bool ishigh;
    QColor high, low;
};

class ComediDIOView : public AbstractComediView
{
    Q_OBJECT
public:
    ComediDIOView(const QString & devfile, QWidget *parent = 0, Qt::WindowFlags f = 0);
public slots:
    void refreshView();
private slots:
    void ledClicked();
private:
    QList<DIOLed *> leds;
    
};

class ComediAOView : public AbstractComediView
{
    Q_OBJECT
public:
    ComediAOView(const QString & devfile, QWidget *parent = 0, Qt::WindowFlags f = 0);
public slots:
    void refreshView();
private:
    QList<QLCDNumber *> lcds;
};

#endif
