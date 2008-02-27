#include "SoundTrig.h"
#include "rtos_utility.h"
#include "Version.h"
#include "Mutex.h"
#include "UserspaceExtTrig.h"
#include "WavFile.h"
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <netinet/tcp.h> 
#include <signal.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <ctype.h>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <list>
#include <map>
#include <vector>
#include <memory>
#include <pthread.h>

#ifndef ABS
#define ABS(a) ( (a) < 0 ? -(a) : (a) )
#endif

class ConnectedThread;
struct Matrix;
struct SoundBuffer;
class SoundMachine;

int listen_fd = -1; /* Our listen socket.. */
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
        Userspace
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
        
    virtual bool setSound(unsigned card, const SoundBuffer & buf) = 0;
    
    static SoundMachine *attach();
    
    virtual ~SoundMachine();
    
protected:
    Mode pm;
    void *shm_notype;
    SoundMachine(Mode m, void *shm) : pm(m), shm_notype(shm) {}
};

SoundMachine::~SoundMachine() 
{ 
    if (shm_notype) RTOS::shmDetach(shm_notype); shm_notype=0;
}

class KernelSM : public SoundMachine
{
    RTOS::FIFO fifo_in[MAX_CARDS], fifo_out[MAX_CARDS];
    Shm *shm;
    mutable Mutex mut;
    
    // Functions to send commands to the realtime process via the rt-fifos
    void sendToRT(unsigned card, FifoMsgID cmd) const; // send a simple command, one of RESET, PAUSEUNPAUSE, INVALIDATE. Upon return we know the command completed.
    void sendToRT(unsigned card, FifoMsg & msg) const; // send a complex command, wait for a reply which gets put back into 'msg'.  Upon return we know the command completed.
    
public:    
    KernelSM(void *shm_);
    virtual ~KernelSM();
    
    void reset(unsigned card);
    void halt(unsigned card);
    void run(unsigned card);
    void trigger(unsigned card, int trig);
    bool isRunning(unsigned card) const;
    double getTime(unsigned card) const;
    int getLastEvent(unsigned card) const;
    unsigned getNCards() const { return shm->num_cards; }
    
    bool setSound(unsigned card, const SoundBuffer &);
};

class Timer
{
public:
  Timer() { reset(); }
  void reset();
  double elapsed() const; // returns number of seconds since ctor or reset() was called 
private:
  struct timeval ts;
};

class UserSM : public SoundMachine
{
    UTShm *shm;
    RTOS::FIFO fifo;
    pthread_t thr;
    mutable Mutex mut;
    Timer timer;
    volatile bool threadRunning;
    volatile bool cardRunning;
    volatile int lastEvent;
    
    struct SoundFile {
        std::string filename;
        volatile int pid;
        SoundFile() : pid(0) {}
    };
    typedef std::map<unsigned, SoundFile> SoundFileMap;
    SoundFileMap soundFileMap;
    
    static void *threadwrapFRT(void *arg) { static_cast<UserSM *>(arg)->fifoReadThr(); return 0; }
    struct CDArgs;
    static void *thrWrapChildDone(void *arg);

    void fifoReadThr(); 
    bool soundExists(unsigned id);
    static void childReaper(int sig);
    void childDone(int pid);
    
public:    
    UserSM(void *s);
    virtual ~UserSM();
    
    void reset(unsigned card);
    void halt(unsigned card);
    void run(unsigned card);
    void trigger(unsigned card, int trig);
    bool isRunning(unsigned card) const;
    double getTime(unsigned card) const;
    int getLastEvent(unsigned card) const;
    unsigned getNCards() const;
    
    bool setSound(unsigned card, const SoundBuffer &);
};
        


static std::ostream & log(int ignored = 0)
{
  (void)ignored;
  return std::cerr;
}

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

  std::ostream &log(int i = -1) 
  { 
    if (i == 0) return ::log(i); // unlock, so don't output anything extra...
    return ::log(i) << "[Process " << myid << " (" << remoteHost << ")] "; 
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
  	log(1) <<  "deleted." << std::endl;
  	log(0);
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
  ConnectedThread *self = reinterpret_cast<ConnectedThread *>(arg);
  pthread_detach(pthread_self());
  self->myid = (int)pthread_self();
  self->running = true;
  self->pleaseStop = false;
  int ret = self->doConnection();
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
class Exception
{
public:
  Exception(const std::string & reason = "") : reason(reason) {}
  virtual ~Exception() {}

  const std::string & why() const { return reason; }

private:
  std::string reason;
};

SoundMachine * SoundMachine::attach()
{
  // first, connect to the shm buffer..
  RTOS::ShmStatus shmStatus;
  void *shm_notype;

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

  shm_notype = RTOS::shmAttach(SHM_NAME, SHM_SIZE, &shmStatus);

  if (!shm_notype)
      throw Exception(std::string("Cannot attach to shm ") + SHM_NAME
                        + ", error was: " + RTOS::statusString(shmStatus));
    
  Shm *shm = static_cast<Shm *>(shm_notype);
  if (shm->magic != SHM_MAGIC) {
      std::stringstream s;
                  s << "Attached to shared memory buffer at " << shm_notype << " but the magic number is invaid! (" << reinterpret_cast<void *>(shm->magic) << " != " << reinterpret_cast<void *>(SHM_MAGIC) << ")\n";
      s << "Shm Dump:\n";
      char *ptr = reinterpret_cast<char *>(shm_notype);
      for (unsigned i = 0; i < 16; ++i) {
        s << reinterpret_cast<void *>(ptr[i]) << " ";
        if (i && !(i%30)) s << "\n";
      }
      s << "\n";
      RTOS::shmDetach(shm_notype);
      throw Exception(s.str());
  }
  return new KernelSM(shm_notype);
}

static void init()
{
  sm = SoundMachine::attach();
}

static void cleanup(void)
{
  delete sm;

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

  case SIGPIPE: 
    //std::cerr << "PID " << ::getpid() << " GOT SIGPIPE" << std::endl;
    break; // ignore SIGPIPE..

  default:
    log() << "Caught signal " << sig << " cleaning up..." << std::endl;     
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

struct SoundBuffer : public std::vector<char>
{
  SoundBuffer(unsigned long size,
              int id, int n_chans, int sample_size, int rate, int stop_ramp_tau_ms = 0, int loop_flg = 0) 
    : std::vector<char>(size),
      id(id), chans(n_chans), sample_size(sample_size), rate(rate), stop_ramp_tau_ms(stop_ramp_tau_ms), loop_flg(loop_flg)
  { resize(size); }
  int id, chans, sample_size, rate, stop_ramp_tau_ms, loop_flg;
};

void Timer::reset()
{
  ::gettimeofday(&ts, NULL);
}

double Timer::elapsed() const
{
  if (ts.tv_sec == 0) return 0.0;
  struct timeval tv;
  ::gettimeofday(&tv, NULL);
  return tv.tv_sec - ts.tv_sec + (tv.tv_usec - ts.tv_usec)/1000000.0;
}

void doServer(void)
{
  //do the server listen, etc..
    
  listen_fd = ::socket(PF_INET, SOCK_STREAM, 0);

  if (listen_fd < 0) 
    throw Exception(std::string("socket: ") + strerror(errno));

  struct sockaddr_in inaddr;
  socklen_t addr_sz = sizeof(inaddr);
  inaddr.sin_family = AF_INET;
  inaddr.sin_port = htons(listenPort);
  inet_aton("0.0.0.0", &inaddr.sin_addr);

  log() << "Sound Server version " << VersionSTR << std::endl;
  log() << "Sound play mode: " << (sm->mode() == SoundMachine::Kernelspace ? "Kernel (Hard RT)" : "Userspace (non-RT)") << std::endl;
  log() << "Listening for connections on port: " << listenPort << std::endl; 

  int parm = 1;
  if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &parm, sizeof(parm)) )
    log() << "Error: setsockopt returned " << ::strerror(errno) << std::endl; 
  
  if ( ::bind(listen_fd, (struct sockaddr *)&inaddr, addr_sz) != 0 ) 
    throw Exception(std::string("bind: ") + strerror(errno));
  
  if ( ::listen(listen_fd, 1) != 0 ) 
    throw Exception(std::string("listen: ") + strerror(errno));

  childThreads = new ChildThreads;

  while (!pleaseStop) {
    int sock;
    if ( (sock = ::accept(listen_fd, (struct sockaddr *)&inaddr, &addr_sz)) < 0 ) {
      if (errno == EINTR) continue;
      if (listen_fd == -1) /* sighandler closed our fd */ break;
      throw Exception(std::string("accept: ") + strerror(errno));
    }

    if (::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &parm, sizeof(parm)) )
      log() << "Error: setsockopt returned " << ::strerror(errno) << std::endl; 
    
    childThreads->push_back(ConnectedThread());
    ConnectedThread & conn = childThreads->back();
    if ( conn.start(sock, inet_ntoa(inaddr.sin_addr)) == 0 ) {
      log(1) << "Started new thread, now have " << childThreads->size() << " total." 
             << std::endl; log(0);
    } else {
      log(1) << "Error starting connection thread!\n"; log(0);
      childThreads->pop_back();
    }
  }
}

int main(int argc, const char *argv[]) 
{
  int ret = 0;

  // install our signal handler that tries to pause the rt-process and quit the program 
  signal(SIGINT, sighandler);
  signal(SIGPIPE, sighandler);
  signal(SIGQUIT, sighandler);
  signal(SIGHUP, sighandler);
  signal(SIGTERM, sighandler);

  try {
  
    init();

    handleArgs(argc, argv);
  
    // keeps listening on a port, waiting for commands to give to the rt-proc
    doServer();    

  } catch (const Exception & e) {

    log(1) << e.why() << std::endl; log(0);
    ret = 1;

  }

  cleanup();

  return ret;
}

int ConnectedThread::doConnection(void)
{
    Timer connectionTimer;

    log(1) << "Connection received from host " << remoteHost << std::endl; log(0);
   
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
              log(1) << "Send error..." << std::endl; log(0);
              break;
            }
            if (bytes > (128*1024*1024) && (bytes = (128*1024*1024)) )
              log(1) << "Warning soundfile of " << bytes << " truncated to 128MB!" << std::endl; log(0);

            SoundBuffer sound(bytes, id, chans, samplesize, rate, stop_ramp_tau_ms, loop);

            log(1) << "Getting ready to receive sound  " << sound.id << " length " << sound.size() << std::endl; log(0);
            Timer xferTimer;
            count = sockReceiveData(&sound[0], sound.size());	    
            double xt = xferTimer.elapsed();
            if (count == (int)sound.size()) {
              log(1) << "Received " << count << " bytes in " << xt << " seconds (" << ((count*8)/1e6)/xt << " mbit/s)" << std::endl; log(0);
              cmd_error = !sm->setSound(mycard, sound);
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
        log(1) << "Graceful exit requested." << std::endl; log(0);
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
    
    log(1) << "Connection to host " << remoteHost << " ended after " << connectionTimer.elapsed() << " seconds." << std::endl; log(0);
    
    log(1) << " thread exit." << std::endl; log(0);
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
    log(1) << ss.str() << std::flush;
  } else
    log(1) << "Sending binary data of length " << len << std::endl; log(0);  

  int ret = ::send(sock, buf, len, flags);
  if (ret < 0) {
    log(1) << "ERROR returned from send: " << strerror(errno) << std::endl; log(0);
  } else if (ret != (int)len) {
    log(1) << "::send() returned the wrong size; expected " << len << " got " << ret << std::endl; log(0);
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
  log(1) << "Got: " << rets << std::endl; log(0);
  return rets;
}


int ConnectedThread::sockReceiveData(void *buf, int size, bool is_binary)
{
  int nread = 0;

  while (nread < size) {
    int ret = ::recv(sock, (char *)(buf) + nread, size - nread, 0);
    
    if (ret < 0) {
      log(1) << "ERROR returned from recv: " << strerror(errno) << std::endl; log(0);
      return ret;
    } else if (ret == 0) {
      log(1) << "ERROR in recv, connection probably closed." << std::endl; log(0);
      return ret;
    } 
    nread += ret;
    if (!is_binary) break;
  }

  if (!is_binary) {
    char *charbuf = static_cast<char *>(buf);
    charbuf[size-1] = 0;
    log(1) << "Got: " << charbuf << std::endl; log(0);
  } else {
    log(1) << "Got: " << nread << " bytes." << std::endl; log(0);
    if (nread != size) {
      log(1) << "INFO ::recv() returned the wrong size; expected " << size << " got " << nread << std::endl; log(0);
    }
  }
  return nread;
}

bool KernelSM::setSound(unsigned card, const SoundBuffer & s)
{
  log(1) << "Soundfile is: bytes: " << s.size() << " chans: " << s.chans << "  sample_size: " << s.sample_size << "  rate: " << s.rate << std::endl; log(0);

  std::auto_ptr<FifoMsg> msg(new FifoMsg);
  msg->id = SOUND;

  Timer timer;
  
  unsigned long sent = 0;
  while(sent < s.size()) {

    // setup matrix here..
    msg->u.sound.bits_per_sample = s.sample_size;
    msg->u.sound.id = s.id;
    msg->u.sound.chans = s.chans;
    msg->u.sound.rate = s.rate;
    msg->u.sound.total_size = s.size();
    msg->u.sound.stop_ramp_tau_ms = s.stop_ramp_tau_ms;
    msg->u.sound.is_looped = s.loop_flg;

    unsigned bytes = s.size() - sent;
    msg->u.sound.append = sent ? 1 : 0;
    if (bytes > MSG_SND_SZ) bytes = MSG_SND_SZ;
    msg->u.sound.size = bytes;
    memcpy(msg->u.sound.snd, &s[0] + sent, bytes);

    sendToRT(card, *msg);

    // todo: check msg->u.transfer_ok and report errors..
    if (!msg->u.sound.transfer_ok) return false;

    sent += bytes;
  }

  log(1) << "Sent sound to kernel in " << unsigned(timer.elapsed()*1000) << " millisecs." << std::endl; log(0);

  return true;
}

KernelSM::KernelSM(void *s)
    : SoundMachine(Kernelspace, s), shm(static_cast<Shm *>(s))
{
   for (unsigned i = 0; i < shm->num_cards; ++i) {
     fifo_in[i] = RTOS::openFifo(shm->fifo_out[i]);
     if (!fifo_in[i]) throw Exception ("Could not open RTF fifo_in for reading");
     fifo_out[i] = RTOS::openFifo(shm->fifo_in[i], RTOS::Write);
     if (!fifo_out[i]) throw Exception ("Could not open RTF fifo_in for writing");
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

void KernelSM::sendToRT(unsigned card, FifoMsg & msg) const
{
  // lock SHM here??
  MutexLocker ml(mut);

  std::memcpy(const_cast<FifoMsg *>(&shm->msg[card]), &msg, sizeof(FifoMsg));

  FifoNotify_t dummy = 1;    
    
  if ( RTOS::writeFifo(fifo_out[card], &dummy, sizeof(dummy)) != sizeof(dummy) )
    throw Exception("INTERNAL ERROR: Could not write a complete message to the fifo!");
    
  int err; 
  // now wait synchronously for a reply from the rt-process.. 
  if ( (err = RTOS::readFifo(fifo_in[card], &dummy, sizeof(dummy))) == sizeof(dummy) ) { 
    /* copy the reply from the shm back to the user-supplied msg buffer.. */
    std::memcpy(&msg, const_cast<struct FifoMsg *>(&shm->msg[card]), sizeof(FifoMsg));
  } else if (err < 0) { 
    throw Exception(std::string("INTERNAL ERROR: Reading of input fifo got an error: ") + strerror(errno));
  } else {
    throw Exception("INTERNAL ERROR: Could not read a complete message from the fifo!");
  }

  // unlock SHM here??
}

void KernelSM::sendToRT(unsigned card, FifoMsgID cmd) const
{
  std::auto_ptr<FifoMsg> msg(new FifoMsg);

  switch (cmd) {
  case RESET:
  case PAUSEUNPAUSE:
  case INVALIDATE:
    msg->id = cmd;
    sendToRT(card, *msg);
    break;
  default:
    throw Exception("INTERNAL ERRROR: sendToRT(unsigned, FifoMsgID) called with an inappropriate command ID!");
    break;
  }
}

void KernelSM::reset(unsigned card)
{
    sendToRT(card, RESET);
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
    std::auto_ptr<FifoMsg> msg(new FifoMsg);
    msg->id = FORCEEVENT;
    msg->u.forced_event = trig;
    sendToRT(card, *msg);
}

bool KernelSM::isRunning(unsigned card) const
{
    std::auto_ptr<FifoMsg> msg(new FifoMsg);
    msg->id = GETPAUSE;
    sendToRT(card, *msg);
    return !msg->u.is_paused;
}

double KernelSM::getTime(unsigned card) const
{
    std::auto_ptr<FifoMsg> msg(new FifoMsg);
    msg->id = GETRUNTIME;
    sendToRT(card, *msg);
    return double(msg->u.runtime_us)/1e6;
}

int KernelSM::getLastEvent(unsigned card) const
{
    std::auto_ptr<FifoMsg> msg(new FifoMsg);
    msg->id = GETLASTEVENT;
    sendToRT(card, *msg);
    return msg->u.last_event;
}

UserSM::UserSM(void *s)
    : SoundMachine(Userspace, s), shm(static_cast<UTShm *>(s)),
      threadRunning(false), cardRunning(true), lastEvent(0)
{
    if (::access("/usr/bin/play", X_OK))
        throw Exception("Required program /usr/bin/play is missing!");
    fifo = RTOS::openFifo(shm->fifo_out);
    if (!fifo) throw Exception("Could not open RTF fifo_ut for reading");
    signal(SIGCHLD, childReaper);
}

UserSM::~UserSM()
{
    signal(SIGCHLD, SIG_DFL);
    if (threadRunning) {
      pthread_cancel(thr);
      pthread_join(thr, 0);
      threadRunning = false;
    }
    if (fifo) RTOS::closeFifo(fifo);
    fifo = RTOS::INVALID_FIFO;
    reset(0);
}

void UserSM::fifoReadThr()
{
    UTFifoMsg msg;
    int ret;
    
    threadRunning = true;
    
    while ( (ret = RTOS::readFifo(fifo, &msg, sizeof(msg))) == sizeof(msg) ) {
        pthread_testcancel();
        trigger(msg.target, msg.data);
    }
}

bool UserSM::soundExists(unsigned id)
{
    return soundFileMap.count(id) > 0;
}

void UserSM::trigger(unsigned card, int trig)
{
    MutexLocker ml(mut);
    if (card < getNCards() && soundExists(ABS(trig))) {
        SoundFileMap::iterator it = soundFileMap.find(ABS(trig));
        if (it->second.pid) { // untrig always!
            ::kill(it->second.pid, SIGTERM);
            it->second.pid = 0;
        } 
        if (trig > 0) { // (re)trigger
            int pid = fork();
            if (pid > 0) { // parent
                it->second.pid = pid;
            } else if (pid == 0) { // child
                int ret = execl("/usr/bin/play", it->second.filename.c_str(), (char *)NULL);
                if (ret)
                    log() << "Error executing command: /usr/bin/play " << it->second.filename << ": " << strerror(errno) << std::endl;
                std::exit(ret);
            } else {
                log() << "Could not trigger sound #" << trig << ", fork error: " << strerror(errno) << std::endl;
            }
        }
    } else {
        log() << "WARNING: Kernel told us to play sound id (" << card << "," << ABS(trig) << ") which doesn't seem to exist!\n";
    }
}

struct UserSM::CDArgs
{
    UserSM *sm;
    int pid, status;
    pthread_cond_t cond;
    pthread_mutex_t mut;
    CDArgs() : sm(0), pid(0), status(0) { pthread_cond_init(&cond, 0); pthread_mutex_init(&mut, 0); }
    ~CDArgs() { pthread_cond_destroy(&cond); pthread_mutex_destroy(&mut); }
};

void UserSM::childReaper(int sig)
{
    int status, pid;
    if (sig != SIGCHLD) return;
    // reap dead children...?
    while ( (pid = waitpid(-1, &status, WNOHANG)) > 0 ) {
        UserSM *usm = dynamic_cast<UserSM *>(sm);
        if (usm) {
            CDArgs args;
            args.sm = usm;
            args.pid = pid;
            args.status = status;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            pthread_t t;
            if ( !pthread_create(&t, &attr, thrWrapChildDone, (void *)pid) ) {
                pthread_mutex_lock(&args.mut);
                pthread_cond_wait(&args.cond, &args.mut);
                pthread_mutex_unlock(&args.mut);
            }
            pthread_attr_destroy(&attr);
        }
    }
}

void UserSM::childDone(int pid)
{
    MutexLocker ml(mut);
    SoundFileMap::iterator it;
    for (it = soundFileMap.begin(); it != soundFileMap.end(); ++it)
        // found it! mark it as done!
        if (pid == it->second.pid) {
            it->second.pid = 0;
            break;
        }
}

void *UserSM::thrWrapChildDone(void *arg)
{
    CDArgs *a = (CDArgs *)arg;
    UserSM *sm = a->sm;
    int pid = a->pid;
    pthread_cond_signal(&a->cond);
    sm->childDone(pid);
    return 0;
}

void UserSM::reset(unsigned card)
{
    if (card >= getNCards()) return;
    MutexLocker ml(mut);
    SoundFileMap::iterator it;
    for (it = soundFileMap.begin(); it != soundFileMap.end(); ++it) {
        ::remove(it->second.filename.c_str());
    }
    soundFileMap.clear();
    timer.reset();
}

void UserSM::halt(unsigned card)
{
    if (card >= getNCards()) return;
    MutexLocker ml(mut);
    cardRunning = false;
}

void UserSM::run(unsigned card)
{
    if (card >= getNCards()) return;
    MutexLocker ml(mut);
    cardRunning = true;
}

bool UserSM::isRunning(unsigned card) const
{
    if (card >= getNCards()) return false;
    MutexLocker ml(mut);
    return cardRunning;
}

double UserSM::getTime(unsigned card) const
{
    if (card >= getNCards()) return 0.;
    MutexLocker ml(mut);
    return timer.elapsed();
}

int UserSM::getLastEvent(unsigned card) const
{
    if (card >= getNCards()) return 0;
    MutexLocker ml(mut);
    return lastEvent;
}

unsigned UserSM::getNCards()  const { return 1; }

bool UserSM::setSound(unsigned card, const SoundBuffer & buf)
{
    if (card >= getNCards()) return false;
    OWavFile wav;
    std::ostringstream s;
    s << "/tmp/" << "SoundServerSound_" << getpid() << "_" << card << "_" << buf.id << ".wav";
    if (!wav.create(s.str().c_str())) {
        log() << "Error creating wav file: " << s.str() << std::endl;
        return false;
    }
    if (!wav.write(&buf[0], buf.sample_size/8, buf.sample_size, buf.rate))
        return false;
    MutexLocker ml(mut);
    SoundFileMap::iterator it = soundFileMap.find(buf.id);
    if (it != soundFileMap.end()) {
        ::remove(it->second.filename.c_str());
        soundFileMap.erase(it);
    }
    SoundFile f;
    f.filename = s.str();
    soundFileMap[buf.id] = f;
    wav.close();
    return true;
}
