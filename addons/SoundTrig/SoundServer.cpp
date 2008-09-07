#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _REENTRANT
#define _REENTRANT
#endif

#ifdef OS_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winsock2.h>
#  include <mmsystem.h>
#  define SHUT_RDWR SD_BOTH
#  define MSG_NOSIGNAL 0
   typedef int socklen_t;
   static int inet_aton(const char *cp, struct in_addr *inp);
   static const char * WSAGetLastErrorStr(int err_num = -1);
#  define GetLastNetErrStr() (WSAGetLastErrorStr(WSAGetLastError()))
#else
#  include <sys/socket.h>
#  include <sys/ioctl.h>
#  include <netinet/in.h>
#  include <netinet/ip.h> /* superset of previous */
#  include <netinet/tcp.h> 
#  include <netdb.h> /* for gethostbyname, etc */
#  include <arpa/inet.h>
#  include <sys/wait.h>
#  define closesocket(x) close(x)
#  define GetLastNetErrStr() (strerror(errno))
#endif
#include "SoundTrig.h"
#include "rtos_utility.h"
#include "Version.h"
#include "Mutex.h"
#include "UserspaceExtTrig.h"
#include "WavFile.h"
#include "Util.h"
#if defined(OS_LINUX) || defined(OS_OSX)
#include "scanproc.h"
#endif
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <ctype.h>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <cstring>
#include <list>
#include <map>
#include <vector>
#include <memory>
#include <new>
#include <iomanip>
#include <pthread.h>
#include <semaphore.h>

class ConnectedThread;
struct Matrix;
struct SoundBuffer;
class SoundMachine;

int listen_fd = -1, /* Our listen socket.. */  
    debugLvl = 0; 
unsigned short listenPort = 3334;
volatile bool pleaseStop = false;
SoundMachine *sm = 0;

typedef std::list<ConnectedThread> ChildThreads;
ChildThreads *childThreads = 0;
pthread_mutex_t childThreadsMut = PTHREAD_MUTEX_INITIALIZER;

class SoundMachine
{
public:
    enum Mode
    {
        Kernelspace,
        Userspace,
        Emulator
    };

    Mode mode() const { return pm; }
    virtual void reset(unsigned card) = 0;
    virtual void halt(unsigned card) = 0;
    virtual void run(unsigned card) = 0;
    virtual void trigger(unsigned card, int trig) = 0;
    virtual bool isRunning(unsigned card) const = 0;
    virtual double getTime(unsigned card) const = 0;
    virtual int getLastEvent(unsigned card) const = 0;
    virtual unsigned getNCards() const = 0;
        
    virtual bool setSound(unsigned card, SoundBuffer & buf) = 0;
    
    static SoundMachine *attach();
    
    virtual ~SoundMachine();
    
protected:
    Mode pm;
    void *shm_notype;
    SoundMachine(Mode m, void *shm) : pm(m), shm_notype(shm) {}
    virtual void allocSBMem(SoundBuffer &sb) = 0;
    virtual void freeSBMem(SoundBuffer &sb) const = 0;
    friend class SoundBuffer;
};

SoundMachine::~SoundMachine() 
{ 
    if (shm_notype) RTOS::shmDetach(shm_notype); shm_notype=0;
}

class KernelSM : public SoundMachine
{
    RTOS::FIFO fifo_in[MAX_CARDS], fifo_out[MAX_CARDS];
    SndShm *shm;
    mutable Mutex mut;
    
    // Functions to send commands to the realtime process via the rt-fifos
    void sendToRT(unsigned card, SndFifoMsgID cmd) const; // send a simple command, one of RESET, PAUSEUNPAUSE, INVALIDATE. Upon return we know the command completed.
    void sendToRT(unsigned card, SndFifoMsg & msg) const; // send a complex command, wait for a reply which gets put back into 'msg'.  Upon return we know the command completed.
    
    friend class SoundMachine;
    
protected:    
    KernelSM(void *shm_);
    
    void allocSBMem(SoundBuffer &sb);
    void freeSBMem(SoundBuffer &sb) const;

public:    
    virtual ~KernelSM();
    void reset(unsigned card);
    void halt(unsigned card);
    void run(unsigned card);
    void trigger(unsigned card, int trig);
    bool isRunning(unsigned card) const;
    double getTime(unsigned card) const;
    int getLastEvent(unsigned card) const;
    unsigned getNCards() const { return shm->num_cards; }
    
    bool setSound(unsigned card, SoundBuffer &);
};

class AbstractUserSM : public SoundMachine
{
public:
    ~AbstractUserSM();

    struct SoundFile {
        std::string filename;
        volatile int pid; ///< used only in UserSM
        volatile unsigned playct;
        bool loops;
        SoundFile() : pid(0), playct(0), loops(false) {}
    };
    typedef std::map<unsigned, SoundFile> SoundFileMap;

    bool soundExists(unsigned id) const;

    void reset(unsigned card);
    void halt(unsigned card);
    void run(unsigned card);
    void trigger(unsigned card, int trig);
    bool isRunning(unsigned card) const;
    double getTime(unsigned card) const;
    int getLastEvent(unsigned card) const;
    unsigned getNCards() const;
    
    bool setSound(unsigned card, SoundBuffer &);

protected:
    AbstractUserSM(Mode m, void *shm);

    void allocSBMem(SoundBuffer &sb);
    void freeSBMem(SoundBuffer &sb) const;
    
    virtual void trigger_nolock(unsigned card, int trig) = 0;
    

    SoundFileMap soundFileMap;
    mutable Mutex mut;
    Timer timer;
    volatile bool cardRunning;
    volatile mutable int lastEvent, ctr;

};

class UserSM : public AbstractUserSM
{
    UTShm *shm;
    RTOS::FIFO fifo;
    pthread_t thr, chldThr;
    volatile bool threadRunning, chldThreadRunning, chldPleaseStop;
    sem_t child_sem;
    static UserSM * volatile instance;

   
    static void *thrWrapFRT(void *arg) { static_cast<UserSM *>(arg)->fifoReadThr(); return 0; }
#ifndef OS_WINDOWS
    static void *thrWrapChildReaper(void *arg) { static_cast<UserSM *>(arg)->childReaper(); return 0; }
    static void childSH(int);
    void childReaper();
#endif
    
    void stopThreads();
    void fifoReadThr(); 
    
    friend class SoundMachine;
    
protected:
    UserSM(void *s);   
    void trigger_nolock(unsigned card, int trig);

public:    
    virtual ~UserSM();        
};

class EmulSM : public AbstractUserSM
{
    SndShm *sndShm;
    mutable RTOS::FIFO fifo, fifo2;
    static EmulSM * volatile instance;
        
    bool soundExists(unsigned id) const;
    
    friend class SoundMachine;
    
protected:
    EmulSM(void *shm);

    void trigger_nolock(unsigned card, int trig);
    
public:    
    virtual ~EmulSM();

    /// reimplemented -- overrides from AbstraceUserSM
    int getLastEvent(unsigned card) const;        
};
        


class Debug : public Log
{
public:  
    Debug(bool print = (::debugLvl > 0)) : Log() { suppress = !print; (*this) << "DEBUG: "; }
    
};

class Warning : public Log
{
public:  
    Warning() : Log() { (*this) << "[" << TimeText() << " **WARNING**] "; }
};

class Error : public Log
{
public:  
    Error() : Log() { (*this) << "[" << TimeText() << " ******ERROR******] "; }
};

class ConnectedThread
{
public:
  ConnectedThread();
  ~ConnectedThread();

  int start(int socket_fd, const std::string & remoteHost = "unknown");
  void stop();
  int id() const { return myid; }

private:
  int sock, myid, mycard;
  volatile bool running, pleaseStop;
  std::string remoteHost;
  static pthread_mutex_t mut;
  static void *thrFunc(void *arg);

  ::Log Log() 
  { 
    return ::Log() << "[" << TimeText() << " Thread " << myid << " (" << remoteHost << ")] "; 
  }

  int doConnection();

  // Functions to send commands to the realtime process via the rt-fifos
  int sockSend(const std::string & str) ;
  int sockSend(const void *buf, size_t len, bool is_binary = false, int flags = 0);
  int sockReceiveData(void *buf, int size, bool is_binary = true);
  std::string sockReceiveLine();
};

pthread_mutex_t ConnectedThread::mut = PTHREAD_MUTEX_INITIALIZER;

ConnectedThread::ConnectedThread() : sock(-1), myid(0), mycard(0), running(false), pleaseStop(false), remoteHost("Unknown") {}
ConnectedThread::~ConnectedThread()
{ 
  if (running) {
  	stop();	
  	Log() <<  "deleted." << std::endl;
    running = false;
    pleaseStop = true;
  }
}

void ConnectedThread::stop()
{
  pleaseStop = true;
  if (sock > -1)  ::shutdown(sock, SHUT_RDWR), ::close(sock), sock = -1;
}

void *ConnectedThread::thrFunc(void *arg)
{
  long ret = 0;
  ConnectedThread *self = reinterpret_cast<ConnectedThread *>(arg);
  try {
    pthread_detach(pthread_self());
    static Mutex mut;
    static volatile int thread_id_ctr = 0;
    mut.lock();
    self->myid = thread_id_ctr++;
    mut.unlock();
    self->running = true;
    self->pleaseStop = false;
    ret = self->doConnection();
  } catch (const FatalException & e) {
    Error() << "ConnectedThread " << self->myid << " caught FATAL exception: " << e.why() << " --  Exiting program...\n";      
    std::abort();
  } catch (const Exception & e) {
    Error() << "ConnectedThread " << self->myid << " caught exception: " << e.why() << " -- Exiting thread...\n";
  }
  if (childThreads) {
      MutexLocker ml(childThreadsMut);
      for (ChildThreads::iterator it = childThreads->begin(); it != childThreads->end(); ++it)
      if (&(*it) == self)  { childThreads->erase(it); break; }
  }
  return (void *)ret;
}

int ConnectedThread::start(int sock_fd, const std::string & rhost)
{
  sock = sock_fd;  
  remoteHost = rhost;
  int ret;
  pthread_t thr;
  ret = pthread_create(&thr, NULL, &thrFunc, this);
  return ret;
}

/* NOTE that all functions in this program have the potential to throw
 * Exception on failure (which is why they seem not to haev error status return values) */

SoundMachine * SoundMachine::attach()
{
  // first, connect to the shm buffer..
  RTOS::ShmStatus shmStatus;
  void *shm_notype;


#ifndef EMULATOR
  shm_notype = RTOS::shmAttach(UT_SHM_NAME, UT_SHM_SIZE, &shmStatus);

  if (shm_notype) {
      UTShm *shm_ut = static_cast<UTShm *>(shm_notype);
      if (shm_ut->magic != UT_SHM_MAGIC) {
      // magic is invalid -- probably a dummy shm
      RTOS::shmDetach(shm_notype);
      shm_ut = 0;
      shm_notype = 0;
      } else // otherwise, great success!
      return new UserSM(shm_notype);
  }
#endif
  shm_notype = RTOS::shmAttach(SND_SHM_NAME, SND_SHM_SIZE, &shmStatus);
  if (!shm_notype)
      throw Exception(std::string("Cannot attach to shm ") + SND_SHM_NAME
                        + ", error was: " + RTOS::statusString(shmStatus));
    
  SndShm *shm = static_cast<SndShm *>(shm_notype);
  if (shm->magic != SND_SHM_MAGIC) {
      std::stringstream s;
                  s << "Attached to shared memory buffer at " << shm_notype << " but the magic number is invaid! (" << reinterpret_cast<void *>(shm->magic) << " != " << reinterpret_cast<void *>(SND_SHM_MAGIC) << ")\n";
      s << "SndShm Dump:\n";
      char *ptr = reinterpret_cast<char *>(shm_notype);
      for (unsigned i = 0; i < 16; ++i) {
        s << reinterpret_cast<void *>(ptr[i]) << " ";
        if (i && !(i%30)) s << "\n";
      }
      s << "\n";
      RTOS::shmDetach(shm_notype);
      throw Exception(s.str());
  }
#ifdef EMULATOR
  return new EmulSM(shm);
#else
  return new KernelSM(shm_notype);
#endif
}

#ifdef OS_WINDOWS
void doWsaStartup()
{
    WORD wVersionRequested;
    WSADATA wsaData;
    int err;

/* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
    wVersionRequested = MAKEWORD(2, 2);

    err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
        /* Tell the user that we could not find a usable */
        /* Winsock DLL.                                  */
        printf("WSAStartup failed with error: %d\n", err);
        _exit(1);
    }
}
#endif

static void cleanupTmpFiles()
{
  std::cerr << "Deleting old/crufty sounds from TEMP dir..\n";
#ifdef OS_WINDOWS
  std::system((std::string("del /F /Q \"") + TmpPath() + "\"\\SoundServerSound_*.wav").c_str());
  std::system((std::string("del /F /Q \"") + TmpPath() + "\"\\SoundServerSound_*.wav.loops").c_str());
#else
  std::system((std::string("rm -fr '") + TmpPath() + "'/SoundServerSound_*.wav*").c_str());
#endif
}

static bool isSingleInstance()
{
#if defined(OS_LINUX) || defined(OS_OSX)
    return ::num_procs_of_my_exe_no_children() <= 1;
#elif defined(OS_WINDOWS)
    HANDLE mut = CreateMutexA(NULL, FALSE, "Global\\SoundServer.exe.Mutex");
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


static void init()
{
  if (!isSingleInstance())
      throw Exception("It appears another copy of this program is already running!\n");
#ifdef OS_WINDOWS
  doWsaStartup();
#endif
  cleanupTmpFiles();
  sm = SoundMachine::attach();
}

static void cleanup(void)
{
    delete sm; sm = 0;

  if (listen_fd >= 0) ::close(listen_fd);
  listen_fd = -1;
  if (childThreads) {
    MutexLocker ml(childThreadsMut);
    for (ChildThreads::iterator it = childThreads->begin(); it != childThreads->end(); ++it) {
      (*it).stop();
    }
    // NB: don't clear or delete childThreads as they may still be running!
    // let them 'reap' themselves..
  }
#ifdef OS_WINDOWS
  WSACleanup();
#endif
}


static void handleArgs(int argc, const char *argv[])
{
    if (argc == 2) {
      // listenport override
      listenPort = atoi(argv[1]);
      if (! listenPort) throw Exception ("Could not parse listen port.");
    } else if (argc != 1) {
      throw Exception(std::string("Unknown command line parameters.  Usage: ")
                      + argv[0] + " [listenPort]"); 
      
    }
}

extern "C" void sighandler(int sig)
{  
  switch (sig) {

#ifndef OS_WINDOWS
  case SIGPIPE: 
    //std::cerr << "PID " << ::getpid() << " GOT SIGPIPE" << std::endl;
    break; // ignore SIGPIPE..
#endif

  default:
    std::cerr << "Caught signal " << sig << " cleaning up..." << std::endl;     
    pleaseStop = true;
    ::close(listen_fd);
    listen_fd = -1;
    break;
  }
}

struct Matrix
{
public:
  Matrix(int m, int n) : m(m), n(n) { d = new double[m*n]; }
  ~Matrix() { delete [] d; }
  double & at(int r, int c, bool = false)  { return *const_cast<double *>(d + c*m + r); }
  const double & at(int r, int c) const { return const_cast<Matrix *>(this)->at(r,c, true); }
  int rows() const { return m; }
  int cols() const { return n; }
  unsigned bufSize() const { return sizeof(*d)*m*n; }
  void * buf() const { return static_cast<void *>(const_cast<Matrix *>(this)->d); }
private:
  double *d;
  int m, n;
};

struct SoundBuffer
{
  SoundBuffer(unsigned long size,
              int id, int n_chans, int sample_size, int rate, int stop_ramp_tau_ms, int loop_flg, int card);
  ~SoundBuffer();
  
  unsigned long size() const { return len_bytes; }
  
  int id, chans, sample_size, rate, stop_ramp_tau_ms, loop_flg, card;
  const char *name() const { return nam.c_str(); }
  
  unsigned char & operator[](int i) { return mem[i]; }
  const unsigned char & operator[](int i) const { return mem[i]; }
 private:
  unsigned long len_bytes;
  unsigned char *mem;
  std::string nam;
  
  friend class KernelSM;
  friend class AbstractUserSM;

  bool sentOk;
  
};

void KernelSM::allocSBMem(SoundBuffer & sb)
{
      
    mut.lock();
    unsigned n = ++shm->ctr;
    mut.unlock();
    n = n % 10000;
    std::ostringstream s;
    s << std::setw(4) << std::setfill('0') << std::right << n << "au";
    sb.nam = s.str();
    s.str("");
    { // workaround for 64-bit breakage of userspace shm alloc.  We need to request the shm in the kernel
      std::auto_ptr<SndFifoMsg> msg(new SndFifoMsg);
      msg->id = ALLOCSOUND;
      strncpy(msg->u.sound.name, sb.name(), SNDNAME_SZ);
      msg->u.sound.name[SNDNAME_SZ-1] = 0;
      msg->u.sound.size = sb.size();
      sendToRT(sb.card, *msg);
      if (!msg->u.sound.transfer_ok) {
        s << "Kernel failed to create a new soundbuf of length " << sb.len_bytes << " named " << sb.name() << " for sound " << sb.id << ".  Out of memory in kernel audio buffers?";
        throw Exception(s.str());
      }
    }
    sb.mem = new unsigned char[sb.len_bytes];
}

void AbstractUserSM::allocSBMem(SoundBuffer & sb)
{
    std::ostringstream s;
    s << "sb";
    mut.lock();
    s << ++ctr;
    mut.unlock();
    sb.nam = s.str();
    try {
        sb.mem = new unsigned char[sb.len_bytes];
    } catch (const std::bad_alloc & e) {
        throw Exception(std::string("Could not allocate memory for sound buffer: ") + e.what());
    }
}

SoundBuffer::SoundBuffer(unsigned long size,
                         int id, int n_chans, int sample_size, int rate, int stop_ramp_tau_ms, 
                         int loop_flg, int card) 
    : id(id), chans(n_chans), sample_size(sample_size), rate(rate), stop_ramp_tau_ms(stop_ramp_tau_ms), loop_flg(loop_flg), card(card), len_bytes(size), sentOk(false)
{
    mem = 0;
    if (!sm) throw Exception("Global variable 'sm' needs to be set and point to a SoundMachine object!");
    sm->allocSBMem(*this);
}

void AbstractUserSM::freeSBMem(SoundBuffer & sb) const
{
    delete [] sb.mem;
    sb.mem = 0;
}

void KernelSM::freeSBMem(SoundBuffer & sb) const
{
    if (sb.mem) {
        if (!sb.sentOk) { // issue a FREESOUND to kernel if the sound was not transferred ok to kernel
            std::auto_ptr<SndFifoMsg> msg(new SndFifoMsg);
            msg->id = FREESOUND;
            strncpy(msg->u.sound.name, sb.name(), SNDNAME_SZ);
            msg->u.sound.name[SNDNAME_SZ-1] = 0;
            msg->u.sound.size = sb.size();
            sendToRT(sb.card, *msg);
        }
        delete [] sb.mem;
        sb.mem = 0;
    }
}

SoundBuffer::~SoundBuffer()
{
    if (!mem) return;
    if (!sm) throw Exception("Global variable 'sm' needs to be set and point to a SoundMachine object!");
    sm->freeSBMem(*this);
}

void doServer(void)
{
  //do the server listen, etc..
    
  listen_fd = ::socket(PF_INET, SOCK_STREAM, 0);

  if (listen_fd < 0) 
    throw Exception(std::string("socket: ") + GetLastNetErrStr());

  struct sockaddr_in inaddr;
  socklen_t addr_sz = sizeof(inaddr);
  inaddr.sin_family = AF_INET;
  inaddr.sin_port = htons(listenPort);
  inet_aton("0.0.0.0", &inaddr.sin_addr);

  Log() << "Sound Server version " << VersionSTR << std::endl;
  std::string soundplaystr("");
  switch(sm->mode()) {
  case SoundMachine::Kernelspace: soundplaystr = "Kernel (Hard RT)"; break;
  case SoundMachine::Userspace: soundplaystr = "Userspace (non-RT)"; break;
  case SoundMachine::Emulator: soundplaystr = "Emulator"; break;
  default: soundplaystr = "(unknown)"; break;
  }
  Log() << "Sound play mode: " << soundplaystr << std::endl;
  Log() << "Listening for connections on port: " << listenPort << std::endl; 

  const int parm_int = 1, parmsz = sizeof(parm_int);
  const char *parm = (const char *)&parm_int;

  if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, parm, parmsz) )
    Log() << "Error: setsockopt returned " << GetLastNetErrStr() << std::endl; 
  
  if ( ::bind(listen_fd, (struct sockaddr *)&inaddr, addr_sz) != 0 ) 
    throw Exception(std::string("bind: ") + GetLastNetErrStr());
  
  if ( ::listen(listen_fd, 1) != 0 ) 
    throw Exception(std::string("listen: ") + GetLastNetErrStr());

  childThreads = new ChildThreads;

  while (!pleaseStop) {
    int sock;
    if ( (sock = ::accept(listen_fd, (struct sockaddr *)&inaddr, &addr_sz)) < 0 ) {
      if (errno == EINTR) continue;
      if (listen_fd == -1) /* sighandler closed our fd */ break;
      throw Exception(std::string("accept: ") + GetLastNetErrStr());
    }

    if (::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, parm, parmsz) )
      Log() << "Error: setsockopt returned " << GetLastNetErrStr() << std::endl; 
    
    childThreads->push_back(ConnectedThread());
    ConnectedThread & conn = childThreads->back();
    if ( conn.start(sock, inet_ntoa(inaddr.sin_addr)) == 0 ) {
      Log() << "Started new thread, now have " << childThreads->size() << " total." 
             << std::endl; 
    } else {
      Log() << "Error starting connection thread!\n";
      childThreads->pop_back();
    }
  }
}

int main(int argc, const char *argv[]) 
{
  int ret = 0;

  // install our signal handler that tries to pause the rt-process and quit the program 
  signal(SIGINT, sighandler);
#ifndef OS_WINDOWS
  signal(SIGPIPE, sighandler);
  signal(SIGQUIT, sighandler);
  signal(SIGHUP, sighandler);
#endif
  signal(SIGTERM, sighandler);

  try {
  
    handleArgs(argc, argv);
  
    init();

    // keeps listening on a port, waiting for commands to give to the rt-proc
    doServer();    

  } catch (const Exception & e) {

    Log() << e.why() << std::endl; 
    ret = 1;

  }

  cleanup();

  return ret;
}

int ConnectedThread::doConnection(void)
{
    Timer connectionTimer;

    Log() << "Connection received from host " << remoteHost << std::endl;
   
    std::string line;
    int count;

    while ( !pleaseStop && (line = sockReceiveLine()).length() > 0) {
      
      bool cmd_error = true;

      if (line.find("SET SOUND") == 0) {
        /* Sound Upload.. */
        
        // determine bytes chans samplesize(in bytes) rate 
        
        std::string::size_type pos = line.find_first_of("0123456789");
        int id = 0, bytes = 0, chans = 0, samplesize = 0, rate = 0, stop_ramp_tau_ms = 0, loop = 0;
        double bytes_dbl = 0.0, rate_dbl = 0.0;

        if (pos != std::string::npos) {
          std::stringstream s(line.substr(pos));
          s >> id >> bytes_dbl >> chans >> samplesize >> rate_dbl >> stop_ramp_tau_ms >> loop;
          bytes = static_cast<int>(bytes_dbl);
          rate = static_cast<int>(rate_dbl);
          if (id && bytes && chans && samplesize && rate) {
            if ( (count = sockSend("READY\n")) <= 0 ) {
              Log() << "Send error..." << std::endl;
              break;
            }
            if (bytes > (128*1024*1024) && (bytes = (128*1024*1024)) )
              Log() << "Warning soundfile of " << bytes << " truncated to 128MB!" << std::endl; 

            SoundBuffer sound(bytes, id, chans, samplesize, rate, stop_ramp_tau_ms, loop, mycard);
            
            Log() << "Getting ready to receive sound  " << sound.id << " length " << sound.size() << std::endl; 
            Timer xferTimer;
            count = sockReceiveData(&sound[0], sound.size());
            double xt = xferTimer.elapsed();
            if (count == (int)sound.size()) {
              Log() << "Received " << count << " bytes in " << xt << " seconds (" << ((count*8)/1e6)/xt << " mbit/s)" << std::endl; 
              try {
                cmd_error = !sm->setSound(mycard, sound);
              } catch (const Exception & e) {
                Log() << "Caught exception from sm->setSound(): " << e.why() << "\n";
                cmd_error = true;
              }
            } else if (count <= 0) {
              break;
            }
          }
        }
      } else if (line.find("INITIALIZE") == 0) {
        sm->reset(mycard);
        cmd_error = false;
      } else if (line.find("HALT") == 0) {
        sm->halt(mycard);
        cmd_error = false;
      } else if (line.find("RUN") == 0) {
        sm->run(mycard);
        cmd_error = false;        
      } else if (line.find("TRIGGER") == 0 ) { 
        int trig = 0;
        std::string::size_type pos = line.find_first_of("0123456789-");
        if (pos != std::string::npos) {
          std::stringstream s(line.substr(pos));
          s >> trig;
          sm->trigger(mycard, trig);
          cmd_error = false;
        }
      } else if (line.find("IS RUNNING") == 0) {
        std::stringstream s;
        s << ( sm->isRunning(mycard) ? 1 : 0 ) << std::endl;
        sockSend(s.str());
        cmd_error = false;
      } else if (line.find("GET TIME") == 0) {
        std::stringstream s;
        s << sm->getTime(mycard) << std::endl;
        sockSend(s.str());
        cmd_error = false;        
      } else if (line.find("GET LAST EVENT") == 0) {
        std::stringstream s;
        s << sm->getLastEvent(mycard) << std::endl;
        sockSend(s.str());
        cmd_error = false;        
      } else if (line.find("GET NCARDS") == 0) {
        std::stringstream s;
        s << sm->getNCards() << std::endl;
        sockSend(s.str());
        cmd_error = false;        
      } else if (line.find("GET CARD") == 0) {
        std::stringstream s;
        s << mycard << std::endl;
        sockSend(s.str());
        cmd_error = false;
      } else if (line.find("SET CARD") == 0) {
        std::string::size_type pos = line.find_first_of("0123456789");
        if (pos != std::string::npos) {
          int c = -1;
          std::stringstream s(line.substr(pos));
          s >> c;
          if (c >= 0 && c < (int)sm->getNCards()) {
            mycard = c;
            cmd_error = false;
          }
        }
      } else if ( line.find("EXIT") == 0 || line.find("BYE") == 0 || line.find("QUIT") == 0) {
        Log() << "Graceful exit requested." << std::endl; 
        break;
      } else if (line.find("NOOP") == 0) {
        // noop is just used to test the connection, keep it alive, etc
        // it doesn't touch the shm...
        cmd_error = false;        
      }

      if (cmd_error) {
        const char *s = "ERROR\n";
        sockSend(s, std::strlen(s)); 
      } else {
        const char *s = "OK\n";
        sockSend(s, std::strlen(s)); 
      }

    }
    
    Log() << "Connection to host " << remoteHost << " ended after " << connectionTimer.elapsed() << " seconds." << std::endl;
    
    Log() << " thread exit." << std::endl;
    return 0;
}

int ConnectedThread::sockSend(const std::string & str) 
{
  return sockSend(str.c_str(), str.length());
}

int ConnectedThread::sockSend(const void *buf, size_t len, bool is_binary, int flags)
{
  const char *charbuf = static_cast<const char *>(buf);
  if (!is_binary) {
    std::stringstream ss;
    ss << "Sending: " << charbuf; 
    if (charbuf[len-1] != '\n') ss << std::endl;
    Log() << ss.str() << std::flush;
  } else
    Log() << "Sending binary data of length " << len << std::endl; 

  int ret = ::send(sock, charbuf, len, flags);
  if (ret < 0) {
    Log() << "ERROR returned from send: " << GetLastNetErrStr() << std::endl; 
  } else if (ret != (int)len) {
    Log() << "::send() returned the wrong size; expected " << len << " got " << ret << std::endl; 
  }
  return ret;
}

// Note: trims trailing whitespace!  If string is empty, connection error!
std::string ConnectedThread::sockReceiveLine()
{
#define MAX_LINE 2048
  char buf[MAX_LINE] = { 0 };
  int ret, slen = 0;
  std::string rets = "";

  // keep looping until we fill the buffer, or we get a \n
  // eg: slen < MAXLINE and (if nread then buf[slen-1] must not equal \n)
  while ( slen < MAX_LINE && (!slen || buf[slen-1] != '\n') ) {
    ret = ::recv(sock, buf+slen, MAX_LINE-slen, 0);
    if (ret <= 0) break;
    slen += ret;
  }
  if (slen >= MAX_LINE) slen = MAX_LINE-1;
  if (slen) buf[slen] = 0; // add NUL
  // now, trim trailing spaces
  while(slen && ::isspace(buf[slen-1])) { buf[--slen] = 0; }
  rets = buf;
  Log() << "Got: " << rets << std::endl;
  return rets;
}


int ConnectedThread::sockReceiveData(void *buf, int size, bool is_binary)
{
  int nread = 0;

  while (nread < size) {
    int ret = ::recv(sock, (char *)(buf) + nread, size - nread, 0);
    
    if (ret < 0) {
      Log() << "ERROR returned from recv: " << GetLastNetErrStr() << std::endl; 
      return ret;
    } else if (ret == 0) {
      Log() << "ERROR in recv, connection probably closed." << std::endl; 
      return ret;
    } 
    nread += ret;
    if (!is_binary) break;
  }

  if (!is_binary) {
    char *charbuf = static_cast<char *>(buf);
    charbuf[size-1] = 0;
    Log() << "Got: " << charbuf << std::endl;
  } else {
    Log() << "Got: " << nread << " bytes." << std::endl; 
    if (nread != size) {
      Log() << "INFO ::recv() returned the wrong size; expected " << size << " got " << nread << std::endl; 
    }
  }
  return nread;
}

bool KernelSM::setSound(unsigned card, SoundBuffer & s)
{
  Log() << "Soundfile is: bytes: " << s.size() << " chans: " << s.chans << "  sample_size: " << s.sample_size << "  rate: " << s.rate << std::endl; 

  std::auto_ptr<SndFifoMsg> msg(new SndFifoMsg);
  msg->id = SOUND;

  Timer timer;
  unsigned n2copy;

  strncpy(msg->u.sound.name, s.name(), SNDNAME_SZ);
  msg->u.sound.name[SNDNAME_SZ-1] = 0;
  msg->u.sound.size = s.size();
  msg->u.sound.bits_per_sample = s.sample_size;
  msg->u.sound.id = s.id;
  msg->u.sound.chans = s.chans;
  msg->u.sound.rate = s.rate;
  msg->u.sound.stop_ramp_tau_ms = s.stop_ramp_tau_ms;
  msg->u.sound.is_looped = s.loop_flg;
  n2copy = MIN(SND_FIFO_DATA_SZ, s.size());
  msg->u.sound.datalen = n2copy;
  memcpy(msg->u.sound.databuf, &s[0], n2copy);
  sendToRT(card, *msg);
  
  // todo: check msg->u.transfer_ok and report errors..
  if (!msg->u.sound.transfer_ok) return false;
  unsigned ncopied = n2copy;
  while (ncopied < s.size()) {
      n2copy = MIN(SND_FIFO_DATA_SZ, s.size()-ncopied);
      memcpy(msg->u.sound.databuf, &s[ncopied], n2copy);
      msg->u.sound.id = s.id;
      msg->id = SOUNDXFER;
      msg->u.sound.datalen = n2copy;
      sendToRT(card, *msg);
      if (!msg->u.sound.transfer_ok) return false;
      ncopied += n2copy;
  }
  Log() << "Sent sound to kernel in " << unsigned(timer.elapsed()*1000) << " millisecs." << std::endl; 

  s.sentOk = true;
  
  return true;
}

KernelSM::KernelSM(void *s)
    : SoundMachine(Kernelspace, s), shm(static_cast<SndShm *>(s))
{
   for (unsigned i = 0; i < shm->num_cards; ++i) {
     fifo_in[i] = RTOS::openFifo(shm->fifo_out[i]);
     if (!fifo_in[i]) throw Exception ("Could not open RTF fifo_in for reading");
     fifo_out[i] = RTOS::openFifo(shm->fifo_in[i], RTOS::Write);
     if (!fifo_out[i]) throw Exception ("Could not open RTF fifo_out for writing");
   }
}

KernelSM::~KernelSM()
{
  for (unsigned i = 0; i < shm->num_cards; ++i) {
    if (fifo_in[i]) RTOS::closeFifo(fifo_in[i]); 
    fifo_in[i] = RTOS::INVALID_FIFO;
    if (fifo_out[i]) RTOS::closeFifo(fifo_out[i]); 
    fifo_out[i] = RTOS::INVALID_FIFO;
  }
  shm = 0;
}

void KernelSM::sendToRT(unsigned card, SndFifoMsg & msg) const
{
  // lock SHM here??
  MutexLocker ml(mut);
  
  if (shm->magic != SND_SHM_MAGIC)
      throw FatalException("ARGH! The rt-shm was cleared from underneath us!  Did we lose the kernel module?");
  
  std::memcpy(const_cast<SndFifoMsg *>(&shm->msg[card]), &msg, sizeof(SndFifoMsg));

  SndFifoNotify_t dummy = 1;    
    
  if ( RTOS::writeFifo(fifo_out[card], &dummy, sizeof(dummy)) != sizeof(dummy) )
    throw Exception("INTERNAL ERROR: Could not write a complete message to the fifo!");
    
  int err; 
  // now wait synchronously for a reply from the rt-process.. 
  if ( (err = RTOS::readFifo(fifo_in[card], &dummy, sizeof(dummy))) == sizeof(dummy) ) { 
    /* copy the reply from the shm back to the user-supplied msg buffer.. */
    std::memcpy(&msg, const_cast<struct SndFifoMsg *>(&shm->msg[card]), sizeof(SndFifoMsg));
  } else if (err < 0) { 
    throw Exception(std::string("INTERNAL ERROR: Reading of input fifo got an error: ") + strerror(errno));
  } else {
    throw Exception("INTERNAL ERROR: Could not read a complete message from the fifo!");
  }

  // unlock SHM here??
}

void KernelSM::sendToRT(unsigned card, SndFifoMsgID cmd) const
{
  std::auto_ptr<SndFifoMsg> msg(new SndFifoMsg);

  switch (cmd) {
  case INITIALIZE:
  case PAUSEUNPAUSE:
  case INVALIDATE:
    msg->id = cmd;
    sendToRT(card, *msg);
    break;
  default:
    throw Exception("INTERNAL ERRROR: sendToRT(unsigned, SndFifoMsgID) called with an inappropriate command ID!");
    break;
  }
}

void KernelSM::reset(unsigned card)
{
    sendToRT(card, INITIALIZE);
}

void KernelSM::halt(unsigned card)
{
    if (isRunning(card))
        sendToRT(card, PAUSEUNPAUSE);
}

void KernelSM::run(unsigned card)
{
    if (!isRunning(card))
        sendToRT(card, PAUSEUNPAUSE);
}

void KernelSM::trigger(unsigned card, int trig)
{
    std::auto_ptr<SndFifoMsg> msg(new SndFifoMsg);
    msg->id = FORCEEVENT;
    msg->u.forced_event = trig;
    sendToRT(card, *msg);
}

bool KernelSM::isRunning(unsigned card) const
{
    std::auto_ptr<SndFifoMsg> msg(new SndFifoMsg);
    msg->id = GETPAUSE;
    sendToRT(card, *msg);
    return !msg->u.is_paused;
}

double KernelSM::getTime(unsigned card) const
{
    std::auto_ptr<SndFifoMsg> msg(new SndFifoMsg);
    msg->id = GETRUNTIME;
    sendToRT(card, *msg);
    return double(msg->u.runtime_us)/1e6;
}

int KernelSM::getLastEvent(unsigned card) const
{
    std::auto_ptr<SndFifoMsg> msg(new SndFifoMsg);
    msg->id = GETLASTEVENT;
    sendToRT(card, *msg);
    return msg->u.last_event;
}

#ifdef OS_WINDOWS
static int inet_aton(const char *cp, struct in_addr *inp)
{
    unsigned long addr = inet_addr(cp);
    inp->s_addr = addr;
    return 0;
}

//// Statics ///////////////////////////////////////////////////////////

// List of Winsock error constants mapped to an interpretation string.
// Note that this list must remain sorted by the error constants'
// values, because we do a binary search on the list when looking up
// items.
static struct ErrorEntry {
    int nID;
    const char * const pcMessage;

  ErrorEntry(int id, const char* pc = 0) :  nID(id),  pcMessage(pc)  {}

  bool operator<(const ErrorEntry& rhs)  {  return nID < rhs.nID;  }
} gaErrorList[] = {
    ErrorEntry(0,                  "No error"),
    ErrorEntry(WSAEINTR,           "Interrupted system call"),
    ErrorEntry(WSAEBADF,           "Bad file number"),
    ErrorEntry(WSAEACCES,          "Permission denied"),
    ErrorEntry(WSAEFAULT,          "Bad address"),
    ErrorEntry(WSAEINVAL,          "Invalid argument"),
    ErrorEntry(WSAEMFILE,          "Too many open sockets"),
    ErrorEntry(WSAEWOULDBLOCK,     "Operation would block"),
    ErrorEntry(WSAEINPROGRESS,     "Operation now in progress"),
    ErrorEntry(WSAEALREADY,        "Operation already in progress"),
    ErrorEntry(WSAENOTSOCK,        "Socket operation on non-socket"),
    ErrorEntry(WSAEDESTADDRREQ,    "Destination address required"),
    ErrorEntry(WSAEMSGSIZE,        "Message too long"),
    ErrorEntry(WSAEPROTOTYPE,      "Protocol wrong type for socket"),
    ErrorEntry(WSAENOPROTOOPT,     "Bad protocol option"),
    ErrorEntry(WSAEPROTONOSUPPORT, "Protocol not supported"),
    ErrorEntry(WSAESOCKTNOSUPPORT, "Socket type not supported"),
    ErrorEntry(WSAEOPNOTSUPP,      "Operation not supported on socket"),
    ErrorEntry(WSAEPFNOSUPPORT,    "Protocol family not supported"),
    ErrorEntry(WSAEAFNOSUPPORT,    "Address family not supported"),
    ErrorEntry(WSAEADDRINUSE,      "Address already in use"),
    ErrorEntry(WSAEADDRNOTAVAIL,   "Can't assign requested address"),
    ErrorEntry(WSAENETDOWN,        "Network is down"),
    ErrorEntry(WSAENETUNREACH,     "Network is unreachable"),
    ErrorEntry(WSAENETRESET,       "Net connection reset"),
    ErrorEntry(WSAECONNABORTED,    "Software caused connection abort"),
    ErrorEntry(WSAECONNRESET,      "Connection reset by peer"),
    ErrorEntry(WSAENOBUFS,         "No buffer space available"),
    ErrorEntry(WSAEISCONN,         "Socket is already connected"),
    ErrorEntry(WSAENOTCONN,        "Socket is not connected"),
    ErrorEntry(WSAESHUTDOWN,       "Can't send after socket shutdown"),
    ErrorEntry(WSAETOOMANYREFS,    "Too many references, can't splice"),
    ErrorEntry(WSAETIMEDOUT,       "Connection timed out"),
    ErrorEntry(WSAECONNREFUSED,    "Connection refused"),
    ErrorEntry(WSAELOOP,           "Too many levels of symbolic links"),
    ErrorEntry(WSAENAMETOOLONG,    "File name too long"),
    ErrorEntry(WSAEHOSTDOWN,       "Host is down"),
    ErrorEntry(WSAEHOSTUNREACH,    "No route to host"),
    ErrorEntry(WSAENOTEMPTY,       "Directory not empty"),
    ErrorEntry(WSAEPROCLIM,        "Too many processes"),
    ErrorEntry(WSAEUSERS,          "Too many users"),
    ErrorEntry(WSAEDQUOT,          "Disc quota exceeded"),
    ErrorEntry(WSAESTALE,          "Stale NFS file handle"),
    ErrorEntry(WSAEREMOTE,         "Too many levels of remote in path"),
    ErrorEntry(WSASYSNOTREADY,     "Network system is unavailable"),
    ErrorEntry(WSAVERNOTSUPPORTED, "Winsock version out of range"),
    ErrorEntry(WSANOTINITIALISED,  "WSAStartup not yet called"),
    ErrorEntry(WSAEDISCON,         "Graceful shutdown in progress"),
    ErrorEntry(WSAHOST_NOT_FOUND,  "Host not found"),
    ErrorEntry(WSANO_DATA,         "No host data of that type was found")
};

static const int kNumMessages = sizeof(gaErrorList) / sizeof(ErrorEntry);

static const char * WSAGetLastErrorStr(int err)
{
    if (err < 0) err = WSAGetLastError();
    for (int i = 0; i < kNumMessages; ++i) {
        if (err == gaErrorList[i].nID) return gaErrorList[i].pcMessage;
    }
    return "(unknown error)";
}

#endif

// ---------------------------------------------------------------------------
// UserSM Stuff...
// ---------------------------------------------------------------------------

// static member var
UserSM * volatile UserSM::instance = 0;

UserSM::UserSM(void *s)
    : AbstractUserSM(Userspace, s), 
      shm(static_cast<UTShm *>(s)), 
      fifo(RTOS::INVALID_FIFO),
      threadRunning(false), chldThreadRunning(false), chldPleaseStop(false)
{
    if (instance) 
        throw Exception("Multiple instances of class UserSM have been constructed.  Only 1 instance allowed globally! Argh!");

    sem_init(&child_sem, 0, 0);

    if (shm) { /* !shm typically only in Emulator mode */
        if (::access("/usr/bin/play", X_OK))
            throw Exception("Required program /usr/bin/play is missing!");
        fifo = RTOS::openFifo(shm->fifo_out);
        if (!fifo) throw Exception("Could not open RTF fifo_ut for reading");
        if ( pthread_create(&thr, 0, &thrWrapFRT, (void *)this) ) {
            throw Exception(std::string("Could not create the fifo read thread: ") + strerror(errno));
        }
        threadRunning = true;
#ifndef OS_WINDOWS
        signal(SIGCHLD, childSH);
        if ( pthread_create(&chldThr, 0, &thrWrapChildReaper, (void *)this ) ) {
            stopThreads();
            int err = errno;
            throw Exception(std::string("Could not create child reaper thread: ") + strerror(err)); 
        }
        chldThreadRunning = true;
#endif
    }
    instance = this;
}

void UserSM::stopThreads()
{
    if (threadRunning) {
        pthread_cancel(thr);
        pthread_join(thr, 0);
        threadRunning = false;
    }
    if (chldThreadRunning) {
        chldPleaseStop = true;
        sem_post(&child_sem);
        pthread_join(chldThr, 0);
        chldThreadRunning = false;
    }
}

UserSM::~UserSM()
{
    if (instance == this) instance = 0;
#ifndef OS_WINDOWS
    signal(SIGCHLD, SIG_DFL);
#endif
    stopThreads();
    if (fifo != RTOS::INVALID_FIFO) RTOS::closeFifo(fifo);
    fifo = RTOS::INVALID_FIFO;
    sem_destroy(&child_sem);
}

void UserSM::fifoReadThr()
{
    UTFifoMsg msg;
    int ret;
    
    threadRunning = true;
    
    // first, clear out possibly stale data in fifo
    {
        int n = RTOS::fifoNReadyForReading(fifo), tot = 0;
        char buf[128];
        while (n > 0) {
            int bytes = sizeof(buf);
            if (n < bytes) bytes = n;
            ret = RTOS::readFifo(fifo, buf, bytes);
            if (ret > 0) { n-=ret; tot += ret; }
            else break; // hmm.. weird error from fifo read..
        }
        //Log() << "(Discaded " << tot << " bytes of stale data from fifo)" << std::endl;
    }
    
    // now, just loop indefinitely reading the fifo with a blocking read
    while ( (ret = RTOS::readFifo(fifo, &msg, sizeof(msg))) == sizeof(msg) || ret == 0 ) {
        pthread_testcancel(); 
        if (ret > 0) trigger(msg.target, msg.data); // ret == 0 when we catch a signal...?
    }
    Debug() << "exiting fifoReadThr with ret " << ret << " errno: " << strerror(errno) << "\n";
}


void UserSM::trigger_nolock(unsigned card, int trig)
{
    Debug() << "in trigger_nolock with card " << card << " trig " << trig << "\n";
#ifndef OS_WINDOWS

    if (card < getNCards() && soundExists(ABS(trig))) {
        lastEvent = trig;
        SoundFileMap::iterator it = soundFileMap.find(ABS(trig));
        if (it->second.pid) { // untrig always!
            Debug() << "untriggering (killing proc)  " << it->second.pid << "\n";
            kill(it->second.pid, SIGTERM);
            it->second.pid = 0;
        } 
        if (trig > 0) { // (re)trigger
            Debug() << "forking..\n";
            int pid = fork();
            if (pid > 0) { // parent
                it->second.pid = pid;
            } else if (pid == 0) { // child
                int ret = execl("/usr/bin/play", "/usr/bin/play", it->second.filename.c_str(), (char *)NULL);
                if (ret)
                    Log() << "Error executing command: /usr/bin/play " << it->second.filename << ": " << strerror(errno) << std::endl;
                std::exit(-1);
            } else {
                Log() << "Could not trigger sound #" << trig << ", fork error: " << strerror(errno) << std::endl;
            }
        }
    } else {
        if (trig > 0)
            Warning() << "Kernel told us to play sound id (" << card << "," << ABS(trig) << ") which doesn't seem to exist!\n";
        else
            Warning() << "Kernel told us to stop sound id (" << card << "," << ABS(trig) << ") which doesn't seem to exist!\n";            
    }
#else
    Error() << "trigger_nolock() unsupported for UserSM\n";
#endif
}

#ifndef OS_WINDOWS
void UserSM::childSH(int sig)
{
    if (!instance) return;
    Debug() << "in UserSM::childSH, posting to sem\n";
    if (sig == SIGCHLD) sem_post(&instance->child_sem);
}

void UserSM::childReaper()
{
    int status, pid;
    chldThreadRunning = true;
    // reap dead children... until we receive a cancellation request which I *think* should cause us to return with EINTR out of wait()?
    while ( !chldPleaseStop && sem_wait(&child_sem) == 0 ) {
        Debug() << "in UserSM::childReaper woke up from sem\n";
        while ( !chldPleaseStop && (pid = wait(&status)) > 0 ) {
            if ( chldPleaseStop ) break;
            Debug() << "in UserSM::childReaper got pid: " << pid << " status " << status << "\n";
            MutexLocker ml(mut);
            SoundFileMap::iterator it;
            for (it = soundFileMap.begin(); it != soundFileMap.end(); ++it)
                // found it! mark it as done!
                if (pid == it->second.pid) {
                    Debug() << "in UserSM::childReaper found running sound, clearing its status\n";
                    it->second.pid = 0;
                    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                        ++it->second.playct;
                    if (it->second.loops && it->second.playct)
                        // re-trigger it since it's a looping sound!
                        trigger_nolock(0, it->first);
                    break;
                }
        }
        Debug() << "wait() returned " << pid << " errno is: " << strerror(errno) << "\n";
        Debug() << "in UserSM::childReaeper gunna wait on sem again\n";        
    }
    if (chldPleaseStop)
        Debug() << "childReaper got chldPleaseStop request, ending thread..\n";
}
#endif

// ---------------------------------------------------------------------------
// EmulSM Stuff...
// ---------------------------------------------------------------------------

// static member var
EmulSM * volatile EmulSM::instance = 0;

EmulSM::EmulSM(void *s)
    : AbstractUserSM(Emulator, s), 
      sndShm(static_cast<SndShm *>(s)), 
      fifo(RTOS::INVALID_FIFO), fifo2(RTOS::INVALID_FIFO)      
{
    if (instance) 
        throw Exception("Multiple instances of class EmulSM have been constructed.  Only 1 instance allowed globally! Argh!");

    if (sndShm) {
        fifo = RTOS::openFifo(sndShm->fifo_in[0], RTOS::Write);
        fifo2 = RTOS::openFifo(sndShm->fifo_out[0], RTOS::Read);
    }

    instance = this;
}

EmulSM::~EmulSM()
{
    if (instance == this) instance = 0;
    if (fifo != RTOS::INVALID_FIFO) RTOS::closeFifo(fifo);
    if (fifo2 != RTOS::INVALID_FIFO) RTOS::closeFifo(fifo2);
    fifo = fifo2 = RTOS::INVALID_FIFO;
    //RTOS::shmDetach(sndShm); // implicitly called by SoundMachine::~SoundMachine()
}

void EmulSM::trigger_nolock(unsigned card, int trig)
{
    if (sndShm) {
        if (card < sndShm->num_cards) {
            lastEvent = trig;
            RTOS::FIFO & out = fifo;
            RTOS::FIFO & in = fifo2;
            if (out != RTOS::INVALID_FIFO && in != RTOS::INVALID_FIFO) {
                SndFifoNotify_t dummy = 1;
                sndShm->msg[card].id = FORCEEVENT;
                sndShm->msg[card].u.forced_event = trig;
                if ( RTOS::writeFifo(out, &dummy, sizeof(dummy), true) == sizeof(dummy) ) 
                    RTOS::readFifo(in, &dummy, sizeof(dummy), true);
            }
        }
    }
}

int EmulSM::getLastEvent(unsigned card) const
{
    if (card >= getNCards()) return 0;
    MutexLocker ml(mut);
    if (sndShm) {
        if (card < sndShm->num_cards) {            
            RTOS::FIFO & out = fifo;
            RTOS::FIFO & in = fifo2;
            if (out != RTOS::INVALID_FIFO && in != RTOS::INVALID_FIFO) {
                SndFifoNotify_t dummy = 1;
                sndShm->msg[card].id = GETLASTEVENT;
                sndShm->msg[card].u.last_event = 0;
                if ( RTOS::writeFifo(out, &dummy, sizeof(dummy), true) == sizeof(dummy)  &&  RTOS::readFifo(in, &dummy, sizeof(dummy), true) == sizeof(dummy) )
                    lastEvent = sndShm->msg[card].u.last_event;
            }
        }
    }
    return lastEvent;
}

// ---------------------------------------------------------------------------
// AbstraceUserSM Stuff...
// ---------------------------------------------------------------------------
AbstractUserSM::AbstractUserSM(Mode m, void *shm)
    : SoundMachine(m,shm), cardRunning(true), lastEvent(0), ctr(0)
{}

AbstractUserSM::~AbstractUserSM()
{
    for (unsigned c = 0; c < getNCards(); ++c)
        reset(c);
}

bool AbstractUserSM::soundExists(unsigned id) const
{
    return soundFileMap.count(id) > 0;
}

void AbstractUserSM::trigger(unsigned card, int trig)
{
    MutexLocker ml(mut);
    trigger_nolock(card, trig);
}


void AbstractUserSM::reset(unsigned card)
{
    if (card >= getNCards()) return;
    MutexLocker ml(mut);
    SoundFileMap::iterator it;
    for (it = soundFileMap.begin(); it != soundFileMap.end(); ++it) {
        ::remove(it->second.filename.c_str());
        std::string loopfile = it->second.filename + ".loops";
        ::remove(loopfile.c_str());
    }
    soundFileMap.clear();
    timer.reset();
}

void AbstractUserSM::halt(unsigned card)
{
    if (card >= getNCards()) return;
    MutexLocker ml(mut);
    cardRunning = false;
}

void AbstractUserSM::run(unsigned card)
{
    if (card >= getNCards()) return;
    MutexLocker ml(mut);
    cardRunning = true;
}

bool AbstractUserSM::isRunning(unsigned card) const
{
    if (card >= getNCards()) return false;
    MutexLocker ml(mut);
    return cardRunning;
}

double AbstractUserSM::getTime(unsigned card) const
{
    if (card >= getNCards()) return 0.;
    MutexLocker ml(mut);
    return timer.elapsed();
}

int AbstractUserSM::getLastEvent(unsigned card) const
{
    if (card >= getNCards()) return 0;
    MutexLocker ml(mut);
    return lastEvent;
}

unsigned AbstractUserSM::getNCards()  const { return 1; }

bool AbstractUserSM::setSound(unsigned card, SoundBuffer & buf)
{
    if (card >= getNCards()) return false;
    MutexLocker ml(mut);
    SoundFileMap::iterator it = soundFileMap.find(buf.id);
    if (it != soundFileMap.end()) {
        ::remove(it->second.filename.c_str());
        soundFileMap.erase(it);
    }
    OWavFile wav;
    std::ostringstream s;
    s << TmpPath() << "SoundServerSound_" << getpid() << "_" << card << "_" << buf.id << ".wav";
    if (!wav.create(s.str().c_str())) {
        Log() << "Error creating wav file: " << s.str() << std::endl;
        return false;
    }
    if (!wav.write(&buf[0], buf.size()/(buf.sample_size/8), buf.sample_size, buf.rate))
        return false;
    SoundFile f;
    f.filename = s.str();
    f.loops = buf.loop_flg;
    soundFileMap[buf.id] = f;
    wav.close();
    std::string loopfile = s.str() + ".loops";
    if (f.loops) {
        // if it loops, create a special file that tells the play process 
        // to loop the sound
        std::ofstream of(loopfile.c_str(), std::ios::out|std::ios::trunc);
        int c = 1;
        of << c << "\n";
        of.close();
    } else {
        ::remove(loopfile.c_str());
    }
    return true;
}

