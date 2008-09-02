#include "ControlWin.h"
#include <QGroupBox>
#include <QLabel>
#include <QGridLayout>
#include <QTimer>
#include "ComediView.h"
#include "kernel_emul.h"
#include <strings.h>

ControlWin::ControlWin(QWidget *p, Qt::WindowFlags f)
    : QWidget(p,f)
{
    pausedValidRdyLbl = cycleCtLbl = tsLbl = currentStateLbl = transitionCountLbl = schedWavesLbl = aoWavesLbl = 0;
    QTimer *timer_ctls = new QTimer(this);

    setWindowTitle("FSM Emulator - Controls");
    QGridLayout *l = new QGridLayout(this);
    QGroupBox *gb = new QGroupBox("Analog Input Controls", this);
    l->addWidget(gb, 0, 0);
    QGridLayout *l2 = new QGridLayout(gb);
    ComediAIView *ai = new ComediAIView("/dev/comedi0", 0.5, 4.5, gb);
    l2->addWidget(ai, 0, 0);
    connect(timer_ctls, SIGNAL(timeout()), ai, SLOT(refreshView()));

    gb = new QGroupBox("DIO Controls", this);
    l->addWidget(gb, 1, 0);
    l2 = new QGridLayout(gb);
    ComediDIOView *dio = new ComediDIOView("/dev/comedi0", gb);
    gb->setToolTip(dio->toolTip());
    l2->addWidget(dio, 0, 0);
    connect(timer_ctls, SIGNAL(timeout()), dio, SLOT(refreshView()));
 
    gb = new QGroupBox("AO Voltages", this);
    l->addWidget(gb, 2, 0);
    l2 = new QGridLayout(gb);
    ComediAOView *ao = new ComediAOView("/dev/comedi0", gb);
    l2->addWidget(ao, 0, 0);
    connect(timer_ctls, SIGNAL(timeout()), ao, SLOT(refreshView()));
    
    gb = new QGroupBox("Misc. Stats", this);
    l->addWidget(gb, 3, 0);
    l2 = new QGridLayout(gb);
    QLabel *tmpl;
    int col = 0, row = 0;
    tmpl = new QLabel("FSM 0: ", gb);
    pausedValidRdyLbl = new QLabel("", gb);
    l2->addWidget(tmpl, row, col++, Qt::AlignRight);
    l2->addWidget(pausedValidRdyLbl, 0, col++, Qt::AlignLeft);
    tmpl = new QLabel("Cycle:", gb);
    l2->addWidget(tmpl, 0, col++, Qt::AlignRight);
    cycleCtLbl = new QLabel("", gb);
    l2->addWidget(cycleCtLbl, row, col++, Qt::AlignLeft);
    tmpl = new QLabel("Time:", gb);
    l2->addWidget(tmpl, 0, col++, Qt::AlignRight);
    tsLbl = new QLabel("", gb);
    l2->addWidget(tsLbl, row, col++, Qt::AlignLeft);
    tmpl = new QLabel("State:", gb);
    l2->addWidget(tmpl, 0, col++, Qt::AlignRight);
    currentStateLbl = new QLabel("", gb);
    l2->addWidget(currentStateLbl, row, col++, Qt::AlignLeft);
    tmpl = new QLabel("Transitions:", gb);
    l2->addWidget(tmpl, 0, col++, Qt::AlignRight);
    transitionCountLbl = new QLabel("", gb);
    l2->addWidget(transitionCountLbl, row, col++, Qt::AlignLeft);
    
    col = 0; ++row;
    {
        QWidget *w = new QWidget(gb);
        l2->addWidget(w, row, col++, 1, l2->columnCount());
        int col = 0, row = 0;
        QGridLayout *l3 = new QGridLayout(w);
        tmpl = new QLabel("Active Sched Waves:", w);
        schedWavesLbl = new QLabel("", w);
        l3->addWidget(tmpl, row, col++, Qt::AlignRight);
        l3->addWidget(schedWavesLbl, row, col++, Qt::AlignLeft);
        tmpl = new QLabel("Active AO Waves:", w);
        aoWavesLbl = new QLabel("", w);
        l3->addWidget(tmpl, row, col++, Qt::AlignRight);
        l3->addWidget(aoWavesLbl, row, col++, Qt::AlignLeft);
        tmpl = new QLabel("Recently Triggered Sounds:", w);
        sndsLbl = new QLabel("", w);
        l3->addWidget(tmpl, row, col++, Qt::AlignRight);
        l3->addWidget(sndsLbl, row, col++, Qt::AlignLeft);
    }

    connect(timer_ctls, SIGNAL(timeout()), this, SLOT(updateMiscStats()));

    timer_ctls->setSingleShot(false);
    timer_ctls->setInterval(100);// update comedi view every 100ms.. 
    timer_ctls->start(); 
}

void ControlWin::updateMiscStats()
{
    struct FSMStats st;
    fsm_get_stats(0, &st);
    QString pvr = "";
    if (st.isValid) {
        pvr += "valid";
        if (st.isPaused) pvr += ", <font color='#aaaa00'><b>paused</b></font>";
        else pvr += ", <font color='green'><b>running</b></font>";
        if (st.readyForTrial) pvr += ", <font color='#00ffff'><b>ready for trial</b></font>";
    } else
        pvr += "<font color='red'>invalid or not specified</font>";
    pausedValidRdyLbl->setText(pvr);
    cycleCtLbl->setText(QString::number(st.cycle));
    tsLbl->setText(QString::number(st.ts, 'f', 3)+"s");
    currentStateLbl->setText(QString::number(st.state));
    transitionCountLbl->setText(QString::number(st.transitions));
    if (st.activeSchedWaves) {
        QString txt("");
        int w;
        while ((w=ffs(st.activeSchedWaves))) {
            --w;
            st.activeSchedWaves &= ~(0x1<<w);
            txt += QString::number(w) + " ";
        }
        schedWavesLbl->setText(QString("<b>") + txt + "</b>");
    } else
        schedWavesLbl->setText("(none)");

    if (st.activeAOWaves) {
        QString txt("");
        int w;
        while ((w=ffs(st.activeAOWaves))) {
            --w;
            st.activeAOWaves &= ~(0x1<<w);
            txt += QString::number(w) + " ";
        }
        aoWavesLbl->setText(QString("<b>") + txt + "</b>");
    } else
        aoWavesLbl->setText("(none)");

    if (lastPlayedSounds.size()) {
        QTime now = QTime::currentTime();
        QString txt("");
        for(SndTimeMap::iterator it = lastPlayedSounds.begin();
            it != lastPlayedSounds.end(); ++it)
        {
            if (now.msecsTo(it->second) <= 0) {
                lastPlayedSounds.erase(it);
            } else
                txt += QString::number(it->first) + " ";
        }
        if (txt.length())
            sndsLbl->setText(QString("<b>") + txt + "</b>");        
        else 
            sndsLbl->setText("(none)");            
    } else {
        sndsLbl->setText("(none)");
    }
}

void ControlWin::triggeredSound(unsigned id)
{
    lastPlayedSounds[id] = QTime::currentTime().addSecs(3);
    updateMiscStats();
}
void ControlWin::untriggeredSound(unsigned id)
{
    lastPlayedSounds.erase(id);
    updateMiscStats();
}

void ControlWin::resetAllSoundStats()
{
    lastPlayedSounds.clear();
    updateMiscStats();
}
