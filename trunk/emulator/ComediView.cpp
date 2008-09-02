#include "ComediView.h"
#include <QLabel>
#include <QGridLayout>
#include <QDoubleValidator>
#include <QLineEdit>
#include <QGroupBox>
#include <QScrollArea>
#include <QPalette>
#include <QPainter>
#include <QMouseEvent>
#include <QLCDNumber>

AbstractComediView::AbstractComediView(const QString & devfile,
                                       int sdtype,
                                       QWidget *p, Qt::WindowFlags f)
    : QWidget(p, f), c(0), sdtype(sdtype), subdev(-1)
{
    c = ComediEmul::getInstance(devfile.toUtf8().data());
    if (c) subdev = c->findByType(sdtype);
}

AbstractComediView::~AbstractComediView()
{
    if (c) c->putInstance();
    c = 0;
}

ComediAIView::ComediAIView(const QString & devfile, 
                           double ailow, double aihigh,
                           QWidget *p, Qt::WindowFlags f)
    : AbstractComediView(devfile, COMEDI_SUBD_AI, p, f), ailow(ailow), aihigh(aihigh), didpalinit(false)
{
    if (subdev < 0) return;
    QGridLayout *lo = new QGridLayout(this);
    QDoubleValidator *dv = new QDoubleValidator(0.0, 5.0, 2, this);
    QLabel *l = new QLabel("AI Low: ", this);
    lo->addWidget(l, 0, 0, 1, 1, Qt::AlignRight);
    l->setToolTip("The 'low' voltage to use when pushbutton is not depressed");
    QLineEdit *e = new QLineEdit(this);
    lo->addWidget(e, 0, 1, 1, 1, Qt::AlignLeft);
    e->setValidator(dv);
    e->setText(QString::number(ailow));
    connect(e, SIGNAL(textEdited(const QString &)), this, SLOT(aiLowChanged(const QString &)));
    l = new QLabel("AI High: ", this);
    lo->addWidget(l, 0, 2, 1, 1, Qt::AlignRight);
    l->setToolTip("The 'high' voltage to use when pushbutton is depressed");
    e = new QLineEdit(this);
    lo->addWidget(e, 0, 3, 1, 1, Qt::AlignLeft);
    e->setValidator(dv);
    e->setText(QString::number(aihigh));
    lo->setColumnStretch(4, 1);
    connect(e, SIGNAL(textEdited(const QString &)), this, SLOT(aiHighChanged(const QString &)));
    int nchans = c->getNChans(subdev);
    QGroupBox *gb = new QGroupBox(QString("AI Channels (0 - %1)").arg(nchans-1), this);
    gb->setToolTip("Click channel buttons to toggle AI state high/low");
    lo->addWidget(gb, 1, 0, 1, 5);
    QGridLayout *lo2 = new QGridLayout(gb);
    QScrollArea *qsa = new QScrollArea(gb);
    QWidget *w = new QWidget(0);
    qsa->setWidgetResizable(true);
    lo2->addWidget(qsa, 0, 0);
    lo2 = new QGridLayout(w);
    
    for (int i = 0; i < nchans; ++i) {
        QToolButton *b = new QToolButton(w);
        if (!didpalinit) {
            QPalette p = b->palette();            
            p.setColor(QPalette::ButtonText, QColor(0, 0, 0));
            p.setColor(QPalette::Button, QColor(0, 255, 0));
            normalpal = p;            
            p.setColor(QPalette::ButtonText, QColor(255, 0, 0));
            p.setColor(QPalette::Button, QColor(0, 0, 0));
            hipal = p;
            didpalinit = true;
        }
        lo2->addWidget(b, 0, i);
        b->setCheckable(true);
        connect(b, SIGNAL(toggled(bool)), this, SLOT(buttonToggle(bool)));
        aibuttons.push_back(b);
    }
    qsa->setWidget(w);
    refreshView();
}

void ComediAIView::aiLowChanged(const QString &t)
{
    bool ok;    
    double tmp = t.toDouble(&ok);
    if (ok) ailow = tmp;
    refreshView();
}

void ComediAIView::aiHighChanged(const QString &t)
{
    bool ok;    
    double tmp = t.toDouble(&ok);
    if (ok) aihigh = tmp;
    refreshView();
}

void ComediAIView::buttonToggle(bool state)
{
    QList<QToolButton *>::iterator it;
    int ch;
    for (it = aibuttons.begin(), ch = 0; it != aibuttons.end(); ++it, ++ch)
        if (*it == sender()) {
            if (state)
                c->write(subdev, ch, aihigh);
            else
                c->write(subdev, ch, ailow);
            break;
        }
    refreshView();
}


void ComediAIView::refreshView()
{
    QList<QToolButton *>::iterator it;
    int ch;
    for (it = aibuttons.begin(), ch = 0; it != aibuttons.end(); ++it, ++ch) {
        QToolButton *b = *it;
        b->blockSignals(true);
        double v = c->read(subdev, ch);
        bool isdown = false;
        if (v >= aihigh-0.001) isdown = true;
        b->setChecked(isdown);
        if (isdown) {            
            b->setPalette(hipal);
            b->setText(QString("Ch. %1 %2V (hi)").arg(ch,2,10,QLatin1Char('0')).arg(v));
        } else {
            b->setPalette(normalpal);
            b->setText(QString("Ch. %1 %2V (lo)").arg(ch,2,10,QLatin1Char('0')).arg(v));
            b->setForegroundRole(QPalette::NoRole);
        }
        b->blockSignals(false);
    }
}


DIOLed::DIOLed(int sd, int ch, QWidget *p, Qt::WindowFlags f)
  : QWidget(p, f), ch(ch), sd(sd), ishigh(true), high("#ff0000"), low("#661111")
{
  setMaximumWidth(10);
  setMaximumHeight(10);
  setMinimumWidth(10);
  setMinimumHeight(10);
}

DIOLed::~DIOLed() {}

void DIOLed::setState(bool h)
{
  ishigh = h;
  repaint();
}

void DIOLed::paintEvent(QPaintEvent *e)
{
  (void)e;
  QPainter p(this);
  QColor color = ishigh ? high : low;

  // gray it out if it is disabled..
  if (!isEnabled()) color = QColor((QColor(Qt::gray).red()+color.red())/2,
                                   (QColor(Qt::gray).green()+color.green())/2,
                                   (QColor(Qt::gray).blue()+color.blue())/2);
  p.setBrush( Qt::SolidPattern );
  p.setBrush( color );
  p.setPen( color );
  p.drawRect(0, 0, width(), height());
  e->accept();
}

void DIOLed::mousePressEvent(QMouseEvent *e)
{
  e->accept();
  emit clicked();
}


ComediDIOView::ComediDIOView(const QString & devfile, QWidget *p, Qt::WindowFlags f)
    : AbstractComediView(devfile, COMEDI_SUBD_DIO, p, f)
{
    if (subdev < 0) return;
    //QGridLayout *l1 = new QGridLayout(this);
    //QScrollArea *qsa = new QScrollArea(this);
    //QWidget *w = new QWidget(qsa);
    QWidget *w = this;
    //qsa->setWidget(w);
    //qsa->setWidgetResizable(true);
    //l1->addWidget(qsa, 0, 0);
    setToolTip("Click the LEDs toggle DIO channels high/low");
    QGridLayout *l = new QGridLayout(w);
    int row = 0, nchans, chan, sd = subdev;
    do {
        nchans = c->getNChans(sd);
        l->addWidget(new QLabel(QString("DIO Subdev %1:").arg(sd)), row, 0, Qt::AlignRight);
        for (chan = 0; chan < nchans; ++chan) {
            DIOLed *but = new DIOLed(sd, chan, w);
            but->setToolTip(QString("DIO Chan %1 on subdev %2").arg(chan).arg(sd));
            l->addWidget(but, row, chan+1);
            leds.push_back(but);
            connect(but, SIGNAL(clicked()), this, SLOT(ledClicked()));
        }
        l->addWidget(new QWidget, row, chan+1);
        l->setColumnStretch(chan+1, 1);
        ++row;
    } while ( (sd = c->findByType(COMEDI_SUBD_DIO, sd+1)) > -1);
}


void ComediDIOView::ledClicked()
{
    DIOLed *d;
    if ((d = dynamic_cast<DIOLed *>(sender()))) {
        double v = !d->state() ? 1.0 : 0.0;
        c->write(d->subDev(), d->chan(), v);
        refreshView();
    }
}

void ComediDIOView::refreshView()
{
    QList<DIOLed *>::iterator it;
    for (it = leds.begin(); it != leds.end(); ++it) {
        DIOLed *d = *it;
        bool ishigh = c->read(d->subDev(), d->chan()) > 0.0;
        d->blockSignals(true);
        d->setState(ishigh);
        d->blockSignals(false);
    }
}


ComediAOView::ComediAOView(const QString &d, QWidget *p, Qt::WindowFlags f)
    : AbstractComediView(d, COMEDI_SUBD_AO, p, f)
{
    if (subdev < 0) return;
    QGridLayout *lo = new QGridLayout(this);
    int ch, nchans = c->getNChans(subdev), col;
    
    for (ch = 0, col = 0; ch < nchans; ++ch) {
        QLabel *lbl = new QLabel(QString("Ch. %1:").arg(ch, 2, 10, QLatin1Char('0')), this);
        QLCDNumber *lcd = new QLCDNumber(this);
        lcd->setSmallDecimalPoint(true);
        lcd->setNumDigits(4);
        lcds.push_back(lcd);
        lo->addWidget(lbl, 0, col++, Qt::AlignRight);
        lo->addWidget(lcd, 0, col++, Qt::AlignLeft);
    }
}

void ComediAOView::refreshView()
{
    QList<QLCDNumber *>::iterator it;
    int ch;
    for (ch = 0, it = lcds.begin(); it != lcds.end(); ++ch, ++it) {
        double v = c->read(subdev, ch);
        (*it)->display(v);
    }
}
