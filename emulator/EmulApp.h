#ifndef EmulApp_H
#define EmulApp_H

#include <QApplication>
#include <QColor>
#include <QMutex>
#include <map>
#include <QProcess>
class QWidget;
class QTextEdit;
class MainWindow;
class ProcFileViewer;
class ModuleParmView;
class QPushButton;
class ControlWin;
class QSound;
struct FSMExtTrigShm;
struct SndShm;
class SndThr;
class SoundPlayer;

/**
   \brief The central class to the program that more-or-less encapsulates most objects and data in the program.

   This class inherits from QApplication for simplicity.  It is sort of a 
   central place that other parts of the program use to find settings,
   pointers to other windows, and various other application-wide data.
*/   
class EmulApp : public QApplication
{
    Q_OBJECT

    friend int main(int, char **);
    friend class SndThr;

    EmulApp(int & argc, char ** argv);  ///< only main can construct us
public:

    /// Returns a pointer to the singleton instance of this class, if one exists, otherwise returns 0
    static EmulApp *instance() { return singleton; }

    virtual ~EmulApp();
    
    /// Returns a pointer to the application-wide ConsoleWindow instance
    MainWindow *mainWindow() const { return const_cast<MainWindow *>(mainwin); }

    /// Returns true iff the application's console window has debug output printing enabled
    bool isDebugMode() const;

    /// Returns the directory under which all plugin data files are to be saved.
     /// Thread-safe logging -- logs a line to the log window in a thread-safe manner
    void logLine(const QString & line, const QColor & = QColor());

    /// Display a message to the status bar
    void statusMsg(const QString & message, int timeout_msecs = 0);

    /// Used to catch various events from other threads, etc
    bool eventFilter ( QObject * watched, QEvent * event );

    enum EventsTypes {
        LogLineEventType = QEvent::User, ///< used to catch log line events see EmulApp.cpp 
        StatusMsgEventType, ///< used to indicate the event contains a status message for the status bar
        QuitEventType, ///< so we can post quit events..
        SoundTrigEventType, ///< so we can post sound trigger events...        
        SoundEventType,
    };
    /// Returns true if and only if the application is still initializing and not done with its startup.  This is mainly used by the socket connection code to make incoming connections stall until the application is finished initializing.
    bool busy() const { return initializing; }

public slots:    
    /// Set/unset the application-wide 'debug' mode setting.  If the application is in debug mode, Debug() messages are printed to the console window, otherwise they are not
    void setDebugMode(bool); 
    void about();
    void restartFSMAndServer();
    void showProcFileViewer();
    void showModParmsViewer();
    void showControlWin();

protected:

protected slots:
    /// Called from a timer every ~250 ms to update the status bar at the bottom of the console window
    void updateStatusBar();
   
    /// reads all stdout from fsm server and posts it to the log
    void fsmServerHasOutput(); 

    /// reads all stdout from sound server and posts it to the log
    void soundServerHasOutput(); 

    /// connected to applyParamsBut clicked(), applies params, restarts fsm
    void applyModParams();
    /// connected to revertParamsBut clicked(), reverts params, disables buttons
    void revertModParams();
    /// enables the applyParamsBut
    void modParamsChanged();

    void processError(QProcess::ProcessError);
    void processDied();

private:
    void loadSettings();
    void saveSettings();
    struct FSMPrintFunctor;
    FSMPrintFunctor *fsmPrintFunctor;
    void startFSM();
    void stopFSM();
    void startFSMServer();
    void stopFSMServer();
    void startSoundServer();
    void stopSoundServer();
    static int soundTrigFunc(unsigned, int);
    void trigSound(int sndtrig);
    void killProcs(const QString & name);
    void destroyAllSounds();
    void gotSound(unsigned id, const QString & fname);
    //void createAppIcon();

    mutable QMutex mut; ///< used to lock outDir param for now
    MainWindow *mainwin;
    bool debug;
    volatile bool initializing;
    QColor defaultLogColor;
    static EmulApp *singleton;
    unsigned nLinesInLog, nLinesInLogMax;
    bool fsmRunning;
    QProcess *fsmServerProc, *soundServerProc;
    ProcFileViewer *pfv;
    QWidget *mpv; ///< moduleParmViewer 
    ModuleParmView *mp;
    QPushButton *applyParamsBut, *revertParamsBut;
    ControlWin *controlwin;
    typedef std::map<unsigned, SoundPlayer *> SoundPlayerMap;
    SoundPlayerMap soundPlayerMap;
    FSMExtTrigShm *soundTrigShm;
    SndThr *sndThr;

protected: 
    /// read/written by class SndThr and also by EmulApp::trigSound(int)
    volatile int lastSndEvt;
};

#endif
