#if defined(Q_OS_WIN) || defined(Q_OS_CYGWIN) || defined(WIN32)
#  define _WIN32_WINNT 0x0501
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#else
#  include <unistd.h>
#endif
#include <QTextEdit>
#include <QMessageBox>
#include "EmulApp.h"
#include "MainWindow.h"
#include "Log.h"
#include "Version.h"
#include "Common.h"
#include <qglobal.h>
#include <QEvent>
#include <cstdlib>
#include <QSettings>
#include <QMetaType>
#include <QStatusBar>
#include <QTimer>
#include <QKeyEvent>
#include <QFileDialog>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QPixmap>
#include <QIcon>
#include <QRadialGradient>
#include <QPainter>
#include <QDir>
#include <QDesktopWidget>
#include <QMutexLocker>
#include <sstream>
#include <QCloseEvent>
#include "kernel_emul.h"
#include "ProcFileViewer.h"
#include "ModuleParmView.h"
#include <QPushButton>
#include <QGridLayout>
#include <QScrollArea>
#include <QLabel>
#include <QList>
#include <QGroupBox>
#include "ControlWin.h"
#include <QStringList>
#include <QRegExp>
#include <QFile>
#include "FSMExternalTrig.h"
#include "rtos_utility.h"
#include <QProcess>
#if defined(OS_LINUX) || defined(OS_OSX)
#include "scanproc.h"
#endif
#include "SoundTrig.h"
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include "SoundPlayer.h"
#include <QMenuBar>
#include <QMenu>

Q_DECLARE_METATYPE(unsigned);

namespace {
    struct Init {
        Init() {
            qRegisterMetaType<unsigned>("unsigned");
        }
    };

    Init * volatile init = 0;


    class LogLineEvent : public QEvent
    {
    public:
        LogLineEvent(const QString &str, const QColor & color)
            : QEvent((QEvent::Type)EmulApp::LogLineEventType), str(str), color(color)
        {}
        QString str;
        QColor color;
    };

    class StatusMsgEvent : public QEvent
    {
    public:
        StatusMsgEvent(const QString &msg, int timeout)
            : QEvent((QEvent::Type)EmulApp::StatusMsgEventType), msg(msg), timeout(timeout)
        {}
        QString msg;
        int timeout;
    };

    struct SoundTrigListener
    {
        virtual ~SoundTrigListener() {}
        virtual void triggered(int trig) = 0;
    };

    struct SoundListener
    {
        virtual ~SoundListener() {}
        virtual void gotSound(unsigned id) = 0;
    };

    class SoundTrigEvent : public QEvent
    {
    public:
        SoundTrigEvent(int trig, SoundTrigListener *listener = 0)
            : QEvent((QEvent::Type)EmulApp::SoundTrigEventType), trig(trig), listener(listener) {}
        int trig;
        SoundTrigListener *listener;
    };

    class SoundEvent : public QEvent
    {
    public:
        SoundEvent(unsigned id, QString name, bool loops, SoundListener *listener = 0)
            : QEvent((QEvent::Type)EmulApp::SoundEventType), id(id), name(name), loops(loops), listener(listener) {}
        unsigned id;
        QString name;
        bool loops;
        SoundListener *listener;
    };

    class QuitEvent : public QEvent
    {
    public:
        QuitEvent()
            : QEvent((QEvent::Type)EmulApp::QuitEventType)
        {}
    };

    bool isSingleInstance()
    {
#if defined(OS_LINUX) || defined(OS_OSX)
        return ::num_procs_of_my_exe_no_children() <= 1;
#elif defined(OS_WINDOWS)
        HANDLE mut = CreateMutexA(NULL, FALSE, "Global\\FSMEmulator.exe.Mutex");
        if (mut) {
            DWORD res = WaitForSingleObject(mut, 1000);
            switch (res) {
            case WAIT_ABANDONED:
            case WAIT_OBJECT_0:
                return true;
            default:
                return false;
            }
        }
        // note: handle stays open, which is ok, because when process terminates it will auto-close
#endif
        return true;
    }

};

class SndThr : public QThread, public SoundTrigListener, public SoundListener

{
    volatile bool pleaseStop, trigDone, sndDone;
    EmulApp *app;
    SndShm *sndShm;
    QMutex mut, mut2;
    QWaitCondition cond, cond2;
public:
    SndThr(EmulApp *parent);
    ~SndThr();
    void triggered(int trig);
    void gotSound(unsigned id);
protected:
    void run();
};

SndThr::SndThr(EmulApp *parent)
    : QThread(parent), pleaseStop(false), trigDone(false), sndDone(false), app(parent), sndShm(0)
{
        RTOS::ShmStatus shmStatus;
        // do sound shm stuff..
        sndShm = reinterpret_cast<SndShm *>(RTOS::shmAttach(SND_SHM_NAME, SND_SHM_SIZE, &shmStatus, true));
        if (!sndShm) {
            throw FatalException(QString("Cannot create shm to ") + SND_SHM_NAME + " reason was: " + RTOS::statusString(shmStatus));
        }
        memset(sndShm, 0, SND_SHM_SIZE);
        sndShm->fifo_in[0] = sndShm->fifo_out[0] = -1;
        sndShm->magic = SND_SHM_MAGIC;
        *const_cast<unsigned *>(&sndShm->num_cards) = 1;
        unsigned minor;
        if (RTOS::createFifo(minor, SND_FIFO_SZ) == RTOS::INVALID_FIFO) 
            throw FatalException(QString("Cannot create out fifo for SndShm"));
        sndShm->fifo_out[0] = minor;
        if (RTOS::createFifo(minor, SND_FIFO_SZ) == RTOS::INVALID_FIFO) 
            throw FatalException(QString("Cannot create in fifo for SndShm"));
        sndShm->fifo_in[0] = minor;
        Debug() << "Sound Shm FIFOs -  In: " << sndShm->fifo_in[0] << "  Out: " << sndShm->fifo_out[0];
}

SndThr::~SndThr()
{
    pleaseStop = true;
    if (!wait(2000))
        terminate();    
    if (sndShm) {
        if (sndShm->fifo_out[0] > -1) {
            RTOS::closeFifo(sndShm->fifo_out[0]);
            sndShm->fifo_out[0] = -1;
        }
        if (sndShm->fifo_in[0] > -1) {
            RTOS::closeFifo(sndShm->fifo_in[0]);
            sndShm->fifo_in[0] = -1;
        }
        memset((void *)sndShm, 0, sizeof(*sndShm));
        RTOS::shmDetach(sndShm, 0, true);
        sndShm = 0;
    }
}

void SndThr::run()
{
    while (!pleaseStop) {
        if (RTOS::waitReadFifo(sndShm->fifo_in[0], 200)) {
            SndFifoNotify_t dummy;
            if (RTOS::readFifo(sndShm->fifo_in[0], &dummy, sizeof(dummy), false) == sizeof(dummy)) {
                bool do_reply = false;
                switch(sndShm->msg[0].id) {
                case GETLASTEVENT:
                    sndShm->msg[0].u.last_event = app->lastSndEvt;
                    do_reply = true;
                    break;
                case FORCEEVENT: {
                    int trig = app->lastSndEvt = sndShm->msg[0].u.forced_event;
                    Debug() << "Got sound trigger " << trig << " in EmulApp manual trigger thread.";
                    trigDone = false;
                    QCoreApplication::postEvent(app, new SoundTrigEvent(trig, this));                    
                    mut.lock();
                    if (!trigDone) {
                        cond.wait(&mut); // wait forever for the trigdone!
                    }
                    mut.unlock();
                    do_reply = true;
                }
                    break;
                case SOUND: { // notified of a new sound.. note the sound now lives on the filesystem
                    unsigned id = sndShm->msg[0].u.sound.id;
                    QString fname(sndShm->msg[0].u.sound.databuf);
                    bool islooped = sndShm->msg[0].u.sound.is_looped;
                    Debug() << "Got new sound " << id << ": `" << fname << "' in EmulApp SndThr.";
                    sndDone = false;
                    QCoreApplication::postEvent(app, new SoundEvent(id, fname, islooped, this));                    
                    mut2.lock();
                    if (!sndDone) {
                        cond2.wait(&mut2); // wait forever for the trigdone!
                    }
                    mut2.unlock();
                    do_reply = true;                    
                }
                    break;
                }
                if (do_reply) {
                    dummy = 1;                    
                    RTOS::writeFifo(sndShm->fifo_out[0], &dummy, sizeof(dummy), true);
                }
            }
        }
    }
}

void SndThr::triggered(int trig)
{
    (void)trig;
    mut.lock();
    trigDone = true;
    mut.unlock();
    //Debug() << "EmulApp manual trigger thread will be woken up for trigger " << trig;
    cond.wakeAll();
}

void SndThr::gotSound(unsigned id)
{
    (void)id;
    mut2.lock();
    sndDone = true;
    mut2.unlock();
    //Debug() << "EmulApp manual trigger thread will be woken up for trigger " << trig;
    cond2.wakeAll();
}

EmulApp * EmulApp::singleton = 0;

EmulApp::EmulApp(int & argc, char ** argv)
    : QApplication(argc, argv, true), fsmPrintFunctor(0), debug(false), initializing(true), nLinesInLog(0), nLinesInLogMax(1000), fsmRunning(false), fsmServerProc(0), soundServerProc(0), soundTrigShm(0), sndThr(0), lastSndEvt(0)
{
    if (!isSingleInstance()) {
        QMessageBox::critical(0, "FSM Emulator - Error", "Another copy of this program is already running!");
        std::exit(1);
    }
    if (singleton) {
        QMessageBox::critical(0, "FSM Emulator - Invariant Violation", "Only 1 instance of EmulApp allowed per application!");
        std::exit(1);
    }
    singleton = this;
    setApplicationName("FSMEmulator");
    try {

        if (!::init) ::init = new Init;
        loadSettings();

        installEventFilter(this); // filter our own events

        mainwin = new MainWindow;
        defaultLogColor = mainwin->textEdit()->textColor();

        Log() << "Application started";

        mainwin->installEventFilter(this);
        mainwin->textEdit()->installEventFilter(this);

        mainwin->setWindowTitle("FSM Emulator v." VersionSTR);
        mainwin->resize(600, 300);
        int delta = 0;
#ifdef Q_WS_X11
        delta += 22; // this is a rough guesstimate to correct for window frame size being unknown on X11
#endif
        mainwin->move(0,mainwin->frameSize().height()+delta);
        mainwin->show();
        
        // do sound trig shm stuff..
        RTOS::ShmStatus shmStatus;
        soundTrigShm = reinterpret_cast<FSMExtTrigShm *>(RTOS::shmAttach(FSM_EXT_TRIG_SHM_NAME, FSM_EXT_TRIG_SHM_SIZE, &shmStatus, true));
        if (!soundTrigShm) {
            throw FatalException(QString("Cannot create shm to ") + FSM_EXT_TRIG_SHM_NAME + " reason was: " + RTOS::statusString(shmStatus));
        }
        soundTrigShm->magic = FSM_EXT_TRIG_SHM_MAGIC;
        soundTrigShm->function = &EmulApp::soundTrigFunc;
        soundTrigShm->valid = 1;

        killProcs("FSMServer");
        killProcs("SoundServer");

        startFSM();
        startFSMServer();
        startSoundServer();

        pfv = new ProcFileViewer(0);
        pfv->resize(516, 621);
        pfv->hide();
        
        /// construct the moduleparmviewer..
        mpv = new QWidget(0);
        mpv->setWindowTitle("FSM Emulator - Module Parameter Viewer");
        QGridLayout *l = new QGridLayout(mpv);
        QScrollArea *qsa = new QScrollArea(mpv);
        mp = new ModuleParmView(qsa);        
        qsa->setWidget(mp);
        qsa->setWidgetResizable(true);
        l->addWidget(new QLabel("Modify FSM module parameters here, note that applying the parameters <B><font color=red>*will*</b></font> force an FSM restart!", mpv), 0, 0, 1, 3);
        l->addWidget(qsa, 1, 0, 1, 3);
        applyParamsBut = new QPushButton("Apply", mpv);
        applyParamsBut->setEnabled(false);
        revertParamsBut = new QPushButton("Revert", mpv);
        revertParamsBut->setEnabled(false);
        
        l->addWidget(applyParamsBut, 2, 0, 1, 1, Qt::AlignLeft);
        l->addWidget(revertParamsBut, 2, 1, 1, 1, Qt::AlignLeft);
        l->addWidget(new QWidget(mpv), 2, 2, 1, 1);
        connect(applyParamsBut, SIGNAL(clicked()), this, SLOT(applyModParams()));
        connect(revertParamsBut, SIGNAL(clicked()), this, SLOT(revertModParams()));
        connect(mp, SIGNAL(changed()), this, SLOT(modParamsChanged()));

        // construct the control window
        controlwin = new ControlWin(0);
        controlwin->show();

        setupAppIcon();
        buildAppMenus();
        
        initializing = false;

        Log() << "Application initialized";    

        QTimer *timer = new QTimer(this);
        connect(timer, SIGNAL(timeout()), this, SLOT(updateStatusBar()));
        timer->setSingleShot(false);
        timer->start(247); // update status bar every 247ms.. i like this non-round-numbre.. ;)

    } catch (const Exception & e) {
        Error() << e.what() << "\n";
        QMessageBox::critical(0, "FSM Emulator - Exception Caught", e.what());
        postEvent(mainwin, new QCloseEvent());
    }
}

EmulApp::~EmulApp()
{
    stopSoundServer();
    stopFSMServer();
    stopFSM();    

    if (soundTrigShm) {
        memset(soundTrigShm, 0, sizeof *soundTrigShm);
        RTOS::shmDetach(soundTrigShm, 0, true);
        soundTrigShm = 0;
    }
    if (sndThr) delete sndThr, sndThr = 0;
    Log() << "Exiting..";
    saveSettings();
    singleton = 0;
}

bool EmulApp::isDebugMode() const
{
    return debug;
}

void EmulApp::setDebugMode(bool d)
{
    debug = d;
    saveSettings();
}

bool EmulApp::eventFilter(QObject *watched, QEvent *event)
{
    int type = static_cast<int>(event->type());
    if (type == QEvent::KeyPress) {
    } 
    MainWindow *mw = dynamic_cast<MainWindow *>(watched);
    if (mw) {
        if (type == LogLineEventType) {
            LogLineEvent *evt = dynamic_cast<LogLineEvent *>(event);
            if (evt && mw->textEdit()) {
                QTextEdit *te = mw->textEdit();
                QColor origcolor = te->textColor();
                te->setTextColor(evt->color);
                te->append(evt->str);

                // make sure the log textedit doesn't grow forever
                // so prune old lines when a threshold is hit
                nLinesInLog += evt->str.split("\n").size();
                if (nLinesInLog > nLinesInLogMax) {
                    const int n2del = MAX(nLinesInLogMax/10, nLinesInLog-nLinesInLogMax);
                    QTextCursor cursor = te->textCursor();
                    cursor.movePosition(QTextCursor::Start);
                    for (int i = 0; i < n2del; ++i) {
                        cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor);
                    }
                    cursor.removeSelectedText(); // deletes the lines, leaves a blank line
                    nLinesInLog -= n2del;
                }

                te->setTextColor(origcolor);
                te->moveCursor(QTextCursor::End);
                te->ensureCursorVisible();
                return true;
            } else {
                return false;
            }
        } else if (type == StatusMsgEventType) {
            StatusMsgEvent *evt = dynamic_cast<StatusMsgEvent *>(event);
            if (evt && mw->statusBar()) {
                mw->statusBar()->showMessage(evt->msg, evt->timeout);
                return true;
            } else {
                return false;
            }
        }
    }
    if (watched == this) {
        if (type == QuitEventType) {
            quit();
            return true;
        }
        if (type == SoundTrigEventType) {
            SoundTrigEvent *evt = dynamic_cast<SoundTrigEvent *>(event);
            if (evt) trigSound(evt->trig);
            if (evt->listener) evt->listener->triggered(evt->trig);
            return true;
        }
        if (type == SoundEventType) {
            SoundEvent *evt = dynamic_cast<SoundEvent *>(event);
            if (evt) gotSound(evt->id, evt->name, evt->loops);
            if (evt->listener) evt->listener->gotSound(evt->id);
            return true;
        }
    }
    // otherwise do default action for event which probably means
    // propagate it down
    return QApplication::eventFilter(watched, event);
}

void EmulApp::logLine(const QString & line, const QColor & c)
{
    qApp->postEvent(mainwin, new LogLineEvent(line, c.isValid() ? c : defaultLogColor));
}


void EmulApp::loadSettings()
{
    mut.lock();
    QSettings settings("brodylab.princeton.edu", "FSMEmulator");
    settings.beginGroup("EmulApp");

    const Emul::ModuleParmMap & m = Emul::getModuleParms();
    for (Emul::ModuleParmMap::const_iterator it = m.begin(); it != m.end(); ++it) {
        switch(it->second.type) {
        case Emul::ModuleParm::Int: {
            int & i = *const_cast<int *>(reinterpret_cast<const int *>(it->second.p));
            int def = i;
            if (it->first == "task_rate") def = 60;
            if (it->first == "debug") def = true;
            i = settings.value(QString("mp_") + it->first.c_str(), def).toInt();
            if (it->first == "debug") debug = i;
            break;
        }
        default: break;
        }
    }
    mut.unlock();
}

void EmulApp::saveSettings()
{
    mut.lock();
    QSettings settings("brodylab.princeton.edu", "FSMEmulator");
    settings.beginGroup("EmulApp");
    const Emul::ModuleParmMap & m = Emul::getModuleParms();
    for (Emul::ModuleParmMap::const_iterator it = m.begin(); it != m.end(); ++it) {
        switch(it->second.type) {
        case Emul::ModuleParm::Int: {
            const int & i = *reinterpret_cast<const int *>(it->second.p);
            settings.setValue(QString("mp_") + it->first.c_str(), i);
            if (it->first == "debug") debug = i;
            break;
        }
        default: break;
        }
    }
    mut.unlock();
}


void EmulApp::statusMsg(const QString &msg, int timeout)
{
    qApp->postEvent(mainwin, new StatusMsgEvent(msg, timeout));
}

void EmulApp::updateStatusBar()
{
}

/** \brief A helper class that helps prevent reentrancy into certain functions.

    Mainly EmulApp::loadStim(), EmulApp::unloadStim(), and EmulApp::pickOutputDir() make use of this class to prevent recursive calls into themselves. 

    Functions that want to be mutually exclusive with respect to each other
    and non-reentrant with respect to themselves need merely construct an 
    instance of this class as a local variable, and
    then reentrancy into the function can be guarded by checking against
    this class's operator bool() function.
*/
struct ReentrancyPreventer
{
    static volatile int ct;
    /// Increments a global counter.
    /// The global counter is 1 if only 1 instance of this class exists throughout the application, and >1 otherwise.
    ReentrancyPreventer() { ++ct; }
    /// Decrements the global counter.  
    /// If it reaches 0 this was the last instance of this class and there are no other ones active globally.
    ~ReentrancyPreventer() {--ct; }
    /// Returns true if the global counter is 1 (that is, only one globally active instance of this class exists throughout the application), and false otherwise.  If false is returned, you can then abort your function early as a reentrancy condition has been detected.
    operator bool() const { return ct == 1; }
};
volatile int ReentrancyPreventer::ct = 0;


void EmulApp::about()
{
    QMessageBox::about(mainwin, "About FSM Emulator", 
                       "FSM Emulator v." VersionSTR 
                       "\n\n(C) 2008 Calin A. Culianu <cculianu@yahoo.com>\n\n"
                       "Developed for the Carlos Brody Lab at\n"
                       "Princeton University, Princeton, NJ\n\n"
                       "Software License: GPL v2 or later");
}


void EmulApp::setupAppIcon()
{
#include "FSM.xpm"    
#include "FSM_smaller.xpm"    
#include "FSM_med.xpm"
    QPixmap lg(FSM_xpm), sm(FSM_smaller_xpm), med(FSM_med_xpm);
    QIcon ic(med);

    ic.addPixmap(lg);
    ic.addPixmap(sm);
    
    mainwin->setWindowIcon(ic);    
    controlwin->setWindowIcon(ic);
    pfv->setWindowIcon(ic);
    mpv->setWindowIcon(ic);
}

struct EmulApp::FSMPrintFunctor : public Emul::PrintFunctor
{
    void operator()(const char *line) {
        Log() << line;        
    }
};

void EmulApp::startFSM()
{
    if (!fsmPrintFunctor)  {
        fsmPrintFunctor = new FSMPrintFunctor;
    }
    Emul::setPrintFunctor(fsmPrintFunctor);

    stopFSM();
    fsm_init_module_parms();
    loadSettings(); // makes sure that we re-apply module params from settings to FSM

    Log() << "Starting FSM...\n";    

    int ret = fsm_entry_func();
    if (ret) {
        stopFSM();
        std::ostringstream os;
        os << "Could not start FSM, retval was: " << ret;
        throw Exception(os.str());
    }
    else  fsmRunning = true;
}

void EmulApp::stopFSM()
{
    if (fsmRunning) {
        setLatchTimeNanos(0);        
        fsm_exit_func();
        fsmRunning = false;
    }
}

void EmulApp::restartFSMAndServer()
{
    try {
        stopFSMServer();
        stopSoundServer();
        stopFSM();
        startFSM();
        startFSMServer();
        startSoundServer();
    } catch (const Exception & e) {
        Error() << e.what() << "\n";
        QMessageBox::critical(0, "FSM Emulator - Exception Caught", e.what());
        quit();        
    }
}

void EmulApp::fsmServerHasOutput()
{
    if (!fsmServerProc) return;
    QString str = fsmServerProc->readAllStandardOutput();
    QStringList lines = str.split(QRegExp("[\n\r]"));
    for (QStringList::iterator it = lines.begin(); it != lines.end(); ++it) {
        str = *it;
        while (str.endsWith('\n') || str.endsWith('\r')) str = str.left(str.length()-1);
        if (str.length()) // suppress empties?
            logLine(QString("[FSMServer] ") + str, QColor(0x99, 0x00, 0x99));
    }
}

void EmulApp::soundServerHasOutput()
{
    if (!soundServerProc) return;
    QString str = soundServerProc->readAllStandardOutput();
    QStringList lines = str.split(QRegExp("[\n\r]"));
    for (QStringList::iterator it = lines.begin(); it != lines.end(); ++it) {
        str = *it;
        while (str.endsWith('\n') || str.endsWith('\r')) str = str.left(str.length()-1);
        if (str.length()) // suppress empties?
            logLine(QString("[SoundServer] ") + str, QColor(0xcc, 0x33, 0x66));
    }
}

void EmulApp::startFSMServer()
{
    if (fsmServerProc) delete fsmServerProc;
    fsmServerProc = new QProcess(this);
    fsmServerProc->setProcessChannelMode(QProcess::MergedChannels);        
    connect(fsmServerProc, SIGNAL(readyRead()), this, SLOT(fsmServerHasOutput()));
    connect(fsmServerProc, SIGNAL(error(QProcess::ProcessError)), this, SLOT(processError(QProcess::ProcessError)));
    connect(fsmServerProc, SIGNAL(finished(int, QProcess::ExitStatus)), this, SLOT(processDied()));
    
    QStringList args;
    if (isDebugMode()) args.push_back("-d");
#ifdef Q_OS_WIN
    fsmServerProc->start("FSMServer.exe", args, QIODevice::ReadOnly);
#elif defined(Q_OS_DARWIN)
    if (QFile::exists("FSMEmulator.app/Contents/MacOS/FSMServer"))
        fsmServerProc->start("FSMEmulator.app/Contents/MacOS/FSMServer", args, QIODevice::ReadOnly);
    else
        fsmServerProc->start("./FSMServer", args, QIODevice::ReadOnly);
#else /* LINUX */
    fsmServerProc->start("./FSMServer", args, QIODevice::ReadOnly);
#endif
    Log() << "Starting FSMServer...\n";    
    fsmServerProc->waitForStarted(10000);
    if (fsmServerProc->state() == QProcess::Running) {
#ifdef Q_OS_WIN
        int pid = fsmServerProc->pid()->dwProcessId;
#else
        int pid = fsmServerProc->pid();
#endif
        Log() << "Started FSMServer with PID " << pid << "\n";
    } else {
        QString errStr;

        switch (fsmServerProc->error()) {
        case QProcess::FailedToStart: errStr = "Process failed to start"; break;
        case QProcess::Crashed: errStr = "Process crashed after starting"; break;
        case QProcess::Timedout: errStr = "Waiting for the process timed out"; break;
        default: errStr = "Unknown error";
        }
        throw Exception("Could not start FSMServer: " + errStr);
    }
    
}

void EmulApp::stopFSMServer()
{    
    if (!fsmServerProc) return;
    fsmServerProc->blockSignals(true);
    if (fsmServerProc && fsmServerProc->state() == QProcess::Running) {
#ifdef Q_OS_WIN
        int pid = fsmServerProc->pid()->dwProcessId;
#else
        int pid = fsmServerProc->pid();
#endif
        fsmServerProc->terminate();
        fsmServerProc->waitForFinished(1000);
        fsmServerProc->kill();
        Log() << "FSMServer process " << pid << " killed.";
    }
    if (fsmServerProc) delete fsmServerProc, fsmServerProc = 0;
}

void EmulApp::startSoundServer()
{
    if (sndThr) delete sndThr, sndThr = 0;
    sndThr = new SndThr(this);
    sndThr->start();

    if (soundServerProc) delete soundServerProc;
    soundServerProc = new QProcess(this);

    soundServerProc->setProcessChannelMode(QProcess::MergedChannels);
    connect(soundServerProc, SIGNAL(readyRead()), this, SLOT(soundServerHasOutput()));
    connect(soundServerProc, SIGNAL(error(QProcess::ProcessError)), this, SLOT(processError(QProcess::ProcessError)));
    connect(soundServerProc, SIGNAL(finished(int, QProcess::ExitStatus)), this, SLOT(processDied()));


    QStringList args;
#ifdef Q_OS_WIN
    soundServerProc->start("SoundServer.exe", args, QIODevice::ReadOnly);
#elif defined(Q_OS_DARWIN)
    if (QFile::exists("FSMEmulator.app/Contents/MacOS/SoundServer"))
        soundServerProc->start("FSMEmulator.app/Contents/MacOS/SoundServer", args, QIODevice::ReadOnly);
    else
        soundServerProc->start("./SoundServer", args, QIODevice::ReadOnly);
#else /* LINUX */
    soundServerProc->start("./SoundServer", args, QIODevice::ReadOnly);
#endif
    Log() << "Starting SoundServer...\n";    
    soundServerProc->waitForStarted(10000);
    if (soundServerProc->state() == QProcess::Running) {
#ifdef Q_OS_WIN
        int pid = soundServerProc->pid()->dwProcessId;
#else
        int pid = soundServerProc->pid();
#endif
        Log() << "Started SoundServer with PID " << pid << "\n";
    } else {
        QString errStr;

        switch (soundServerProc->error()) {
        case QProcess::FailedToStart: errStr = "Process failed to start"; break;
        case QProcess::Crashed: errStr = "Process crashed after starting"; break;
        case QProcess::Timedout: errStr = "Waiting for the process timed out"; break;
        default: errStr = "Unknown error";
        }
        throw Exception("Could not start SoundServer: " + errStr);
    }
    
}

void EmulApp::destroyAllSounds()
{
    SoundPlayerMap::iterator it;
    for (it = soundPlayerMap.begin(); it != soundPlayerMap.end(); ++it) {
        it->second->stop();
        delete it->second;
    }
    soundPlayerMap.clear();
}

void EmulApp::stopSoundServer()
{
    destroyAllSounds();
    controlwin->resetAllSoundStats();
    if (!soundServerProc) return;
    soundServerProc->blockSignals(true);

    if (soundServerProc->state() == QProcess::Running) {
#ifdef Q_OS_WIN
        int pid = soundServerProc->pid()->dwProcessId;
#else
        int pid = soundServerProc->pid();
#endif
        soundServerProc->terminate();
        soundServerProc->waitForFinished(1000);
        soundServerProc->kill();
        Log() << "SoundServer process " << pid << " killed.";
    }
    if (soundServerProc) delete soundServerProc, soundServerProc = 0;
    if (sndThr) delete sndThr, sndThr = 0;
}

void EmulApp::showProcFileViewer()
{
    pfv->show();
    pfv->activateWindow();
    pfv->raise();
}

void EmulApp::showModParmsViewer()
{
    const Emul::ModuleParmMap & m = Emul::getModuleParms();
    if (mpv->isHidden()) mp->setModuleParms(&m);
    mpv->show();
    mpv->activateWindow();
    mpv->raise();
}

void EmulApp::showControlWin()
{
    controlwin->show();
    controlwin->activateWindow();
    controlwin->raise();
}

/// connected to applyParamsBut clicked(), applies params, restarts fsm
void EmulApp::applyModParams()
{

    stopFSM();
    mp->applyChanges();
    saveSettings();
    applyParamsBut->setEnabled(false);
    revertParamsBut->setEnabled(false);    
    restartFSMAndServer();
}


/// connected to applyParamsBut clicked(), applies params, restarts fsm
void EmulApp::revertModParams()
{
    mp->revertChanges();
    applyParamsBut->setEnabled(false);
    revertParamsBut->setEnabled(false);    
}

/// enables the applyParamsBut
void EmulApp::modParamsChanged()
{
    applyParamsBut->setEnabled(true);
    revertParamsBut->setEnabled(true);
}


void EmulApp::gotSound(unsigned id, const QString & fname, bool loop_flg)
{
    Debug() << "Got new sound " << id << " for filename `" << fname << "'";
    SoundPlayerMap::iterator it = soundPlayerMap.find(id);
    SoundPlayer *sp = 0;
    if (it != soundPlayerMap.end()) {
        delete it->second;
        soundPlayerMap.erase(it);
    }
    sp = new SoundPlayer(fname, this, loop_flg);
    soundPlayerMap[id] = sp;
}

void EmulApp::trigSound(int sndtrig)
{
    const bool dostop = sndtrig < 0;    
    const unsigned snd = dostop ? -sndtrig : sndtrig;
    lastSndEvt = sndtrig;

    SoundPlayerMap::iterator it = soundPlayerMap.find(snd);
    SoundPlayer *sp = 0;
    if (it == soundPlayerMap.end()) {
        Error() << "Triggered unknown sound " << snd;
        return;
    }
    else sp = it->second;
    Debug() << "Got sound trig " << sndtrig << " for filename `" << sp->fileName() << "'";
    if (dostop) {
        //Debug() << "sound " << snd << " stop";
        sp->stop();
        controlwin->untriggeredSound(snd);
    } else {
        //Debug() << "sound " << snd << " play";
        sp->stop();
        sp->play();
        controlwin->triggeredSound(snd);
    }    
}

/*static*/ int EmulApp::soundTrigFunc(unsigned card, int trig)
{
    (void)card; // ignored..
    //Debug() << "EmulApp::soundTrigFunc(" << card << ", " << trig << ")";
    QCoreApplication::postEvent(EmulApp::instance(), new SoundTrigEvent(trig));
    return 1;
}

#if defined(Q_OS_WIN) || defined(Q_OS_CYGWIN)
static  QString GetLastErrorMessage(DWORD *err_out = 0)
{
    DWORD err = GetLastError();
    if (err_out) *err_out = err;
    char buf[256];
    buf[0] = 0;
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, 0, err, 0, buf, 256, 0);
    return buf;
}
void EmulApp::killProcs(QString name)
{
    if (!name.endsWith(".exe", Qt::CaseInsensitive)) name += ".exe";
    DWORD pids[8192];
    DWORD bret = 0;
    DWORD WINAPI (*fGetProcessImageFileNameA)(HANDLE, const char *, DWORD) = 0;
    HMODULE psapi_handle = LoadLibraryA("psapi.dll");
    // first see if we are on WinXP or Windows Vista by seeing if we can open psapi.dll and then fund the GetProcessImageFileNameA function
    // if not, resort to calling the command-line program 'taskkill'
    if (psapi_handle) {
        fGetProcessImageFileNameA = (DWORD WINAPI(*)(HANDLE, const char *, DWORD))GetProcAddress(psapi_handle, "GetProcessImageFileNameA");
    } else {
        DWORD err;
        QString msg = GetLastErrorMessage(&err);
        Warning() << "LoadLibrary(\"psapi.dll\") failed, error was: (" << err << ") " << msg;
    }
    if (fGetProcessImageFileNameA) {
        Debug() << "Got handle to psapi.dll and found GetProcessImageFileNameA at address " << ((void *)fGetProcessImageFileNameA);
        int n, i;
        if (!EnumProcesses(pids, sizeof(pids), &bret)) {
            Error() << "Could not enumerate processes for killProcs(" << name << ")";
            return;
            }
        n = bret / sizeof(*pids);
        for (i = 0; i < n; ++i) {
            HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, 0, pids[i]);
            if (h) {
                char strbuf[1024];
                DWORD size = sizeof strbuf;
                if (fGetProcessImageFileNameA(h, strbuf, size)) {
                    strbuf[sizeof(strbuf)-1] = 0;
                    QString exename = strbuf;
                    QStringList pathcomps = exename.split("\\");
                    if (pathcomps.size() > 1) {
                        exename = pathcomps.back();
                    }
                    Debug() << "Found process " << pids[i] << " named " << exename;
                    if (exename.toLower() == name.toLower()) {
                        TerminateProcess(h, 0);
                        Log() << "Killing process " << pids[i] << " named `" << exename << "'";
                    }
                } else {
                    Warning() << "Could not grab .exe name for PID " << pids[i];
                }
                CloseHandle(h);
            } else {
                Debug() << "Could not open process with PID " << pids[i];
            }
        }
        FreeLibrary(psapi_handle);
        psapi_handle = 0;
    } else {
        Warning() << "killProcs() failed to use PSAPI for enumerating processes, reverting to taskkill.exe method";
        QStringList args;
        args.push_back("/F");
        args.push_back("/IM");
        args.push_back("/T");
        args.push_back(name);
        QProcess::execute("taskkill", args);
    }
}
#elif defined(Q_OS_LINUX) || defined(Q_OS_DARWIN) /* Unixey */
void EmulApp::killProcs(QString name)
{
    std::system((QString("killall -TERM '") + name + "' >/dev/null 2>&1").toUtf8().constData());
    usleep(250000);
    std::system((QString("killall -KILL '") + name + "' >/dev/null 2>&1").toUtf8().constData());
}
#else
#  error need to implement EmulApp::killProcs() for this platform!
#endif

void EmulApp::processError(QProcess::ProcessError err)
{
    if (closingDown()) return;
    QString msg, name;
    if (sender() == fsmServerProc) name = "FSMServer";
    else if (sender() == fsmServerProc) name = "SoundServer";
    else return; // spurious error signal? //name = "(unknown)";
    switch (err) {
    case QProcess::FailedToStart:
        msg = QString("A required process `") + name + "' failed to start.";
        break;
    case QProcess::Crashed:
        msg = QString("The required process `") + name + "' exited unexpectedly.";
        break;
    case QProcess::Timedout:
        msg = QString("A required process `") + name + "' timed out.";
        break;
    case QProcess::WriteError:
    case QProcess::ReadError:
        msg = QString("Read/Write error communicating with process `") + name + "'.";
        break;
    case QProcess::UnknownError:
    default:
        msg = QString("An unknown error occurred with the `") + name + "' process.";
        break;
    }
    QMessageBox::critical(mainwin, "FSM Emulator - Process Error", msg);
    postEvent(this, new QuitEvent);
}

void EmulApp::processDied()
{    
    processError(QProcess::Crashed);
}


void EmulApp::buildAppMenus()
{
#ifdef Q_OS_DARWIN
    QMenuBar *mb = new QMenuBar(0); // on OSX we make the 'default menubar' (a parentless menubar) here.. so that it's app-global
#else
    QMenuBar *mb = mainwin->menuBar(); // on other platforms it's just part of the mainwin
#endif
    QMenu *m = mb->addMenu("&File");
    m->addAction("&Restart FSM", this, SLOT(restartFSMAndServer()));
    m->addSeparator();
    m->addAction("&Quit", this, SLOT(quit()));
    m = mb->addMenu("&Window");
    m->addAction("&Control Window", this, SLOT(showControlWin()));
    m->addSeparator();
    m->addAction("&ProcFile Viewer Window", this, SLOT(showProcFileViewer()));
    m->addAction("&ModParms Viewer Window", this, SLOT(showModParmsViewer()));

    m = mb->addMenu("&Help");
    m->addAction("&About", this, SLOT(about()));
    m->addAction("About &Qt", this, SLOT(aboutQt()));
}
