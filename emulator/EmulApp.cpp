#if defined(Q_OS_WIN) || defined(Q_OS_CYGWIN)
#  define _WIN32_WINNT 0x0600
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
#include <QSound>
#include <QProcess>
#if defined(OS_LINUX) || defined(OS_OSX)
#include "scanproc.h"
#endif

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

    class SoundTrigEvent : public QEvent
    {
    public:
        SoundTrigEvent(int trig, const QString & f)
            : QEvent((QEvent::Type)EmulApp::SoundTrigEventType), trig(trig), fname(f) {}
        int trig;
        QString fname;
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

EmulApp * EmulApp::singleton = 0;

EmulApp::EmulApp(int & argc, char ** argv)
    : QApplication(argc, argv, true), fsmPrintFunctor(0), debug(false), initializing(true), nLinesInLog(0), nLinesInLogMax(1000), fsmRunning(false), fsmServerProc(0), soundServerProc(0), soundTrigShm(0)
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

        //createAppIcon();
        
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
            if (evt) trigSound(evt->trig, evt->fname);
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


/*
void EmulApp::createAppIcon()
{
    QPixmap pm(QSize(128, 128));
    pm.fill(Qt::transparent);
    QRadialGradient gradient(50, 50, 50, 50, 50);
    gradient.setColorAt(0, QColor::fromRgbF(1, 0, 0, 1));
    gradient.setColorAt(1, QColor::fromRgbF(0, 0, 0, 0));
    QPainter painter(&pm);
    painter.fillRect(0, 0, 128, 128, gradient);
    painter.end();
    mainwin->setWindowIcon(QIcon(pm));
    
    pm.fill(Qt::transparent);
    painter.begin(&pm);
    
    gradient.setColorAt(0, QColor::fromRgbF(0, 0, 1, 1));
    gradient.setColorAt(1, QColor::fromRgbF(0, 0, 0, 0));

    painter.fillRect(0, 0, 128, 128, gradient);

    glWindow->setWindowIcon(QIcon(pm));
}
*/

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
#else /* LINUX/DARWIN */
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
    if (soundServerProc) delete soundServerProc;
    soundServerProc = new QProcess(this);

    soundServerProc->setProcessChannelMode(QProcess::MergedChannels);
    connect(soundServerProc, SIGNAL(readyRead()), this, SLOT(soundServerHasOutput()));
    connect(soundServerProc, SIGNAL(error(QProcess::ProcessError)), this, SLOT(processError(QProcess::ProcessError)));
    connect(soundServerProc, SIGNAL(finished(int, QProcess::ExitStatus)), this, SLOT(processDied()));


    QStringList args;
#ifdef Q_OS_WIN
    soundServerProc->start("SoundServer.exe", args, QIODevice::ReadOnly);
#else /* LINUX/DARWIN */
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

void EmulApp::stopSoundServer()
{
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


void EmulApp::trigSound(int sndtrig, const QString & fname)
{
    if (isDebugMode()) {
        Debug() << "Got sound trig " << sndtrig << " for filename `" << fname << "'";
    }
    bool dostop = sndtrig < 0;    
    unsigned snd = dostop ? -sndtrig : sndtrig;
    SoundPlayerMap::iterator it = soundPlayerMap.find(snd);
    if (dostop && it != soundPlayerMap.end()) {
        it->second->stop();
        controlwin->untriggeredSound(snd);
    } else {
        QSound *qs = 0;
        if (it == soundPlayerMap.end() 
            || (qs = it->second)->fileName() != fname ) {
            // filename has to match.. if not.. there was a restart and the filename was reset, so delete and re-create the QSound object..
            if (qs) delete qs;
            qs = new QSound(fname, this);
            soundPlayerMap[snd] = qs;
        } 
        int loopct = 1;
        if (QFile::exists(fname + ".loops")) loopct = -1;
        qs->setLoops(loopct);
        qs->play();
        controlwin->triggeredSound(snd);
    }
    
}

/*static*/ int EmulApp::soundTrigFunc(unsigned card, int trig)
{
    (void)card; // ignored..
    unsigned snd = trig < 0 ? -trig : trig;
#ifdef Q_OS_WIN
    int pid = instance()->soundServerProc->pid()->dwProcessId;
#else
    int pid = instance()->soundServerProc->pid();
#endif
    QString fname = TmpPath() + "SoundServerSound_" + QString::number(pid) + "_" + QString::number(0) + "_" + QString::number(snd) + ".wav";

    QCoreApplication::postEvent(EmulApp::instance(), new SoundTrigEvent(trig, fname));
    return QFile::exists(fname);
}

#if defined(Q_OS_WIN) || defined(Q_OS_CYGWIN)
void EmulApp::killProcs(const QString & name)
{
#  if defined (__GNUC__) || defined (__GNUC)
#    warning killProcs for GNUC/MinGW is not implemented yet!
    (void)name;
#  else
    DWORD pids[8192];
    DWORD bret = 0;
    int n;
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
            if (GetProcessImageFileNameA(h, strbuf, size)) {
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
                Error() << "Could not grab .exe name for PID " << pids[i];
            }
            CloseHandle(h);
        } else {
            Warning() << "Could not open process with PID " << pids[i];
        }
    }
#  endif
}
#elif defined(Q_OS_LINUX) || defined(Q_OS_DARWIN) /* Unixey */
void EmulApp::killProcs(const QString & name)
{
    std::system((QString("killall -TERM '") + name + "' >/dev/null 2>&1").toUtf8().data());
    usleep(250000);
    std::system((QString("killall -KILL '") + name + "' >/dev/null 2>&1").toUtf8().data());
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
