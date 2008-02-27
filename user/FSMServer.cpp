#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _REENTRANT
#define _REENTRANT
#endif
#include "FSM.h"
#include "rtos_utility.h"
#include "deflate_helper.h"
#include "scanproc.h"
#include "Version.h"
#include "Mutex.h"

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <netinet/tcp.h> 
#include <netdb.h> /* for gethostbyname, etc */
#include <signal.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <ctype.h>
#include <pthread.h>
#include <limits.h>

#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <cstring>
#include <list>
#include <map>
#include <vector>
#include <memory>
#include <algorithm>
#include <set>
#include <fstream>

#if defined(OS_CYGWIN) || defined(OS_OSX)
namespace {
  /* Cygwin (and OSX?) lacks this function, so we will emulate it using 
     a static mutex */
  int gethostbyname2_r(const char *name, int af,
                       struct hostent *ret, char *buf, size_t buflen,
                       struct hostent **result, int *h_errnop);
}
#endif

#ifndef MIN
#  define MIN(a,b) ( (a) < (b) ? (a) : (b) )
#endif
#ifndef MAX
#  define MAX(a,b) ( (a) > (b) ? (a) : (b) )
#endif
#ifndef SOL_TCP 
#  ifndef IPPROTO_TCP
#    define IPPROTO_TCP 6
#  endif
#  define SOL_TCP IPPROTO_TCP
#endif
#if defined(OSX) || defined(OS_OSX)
#  define MSG_NOSIGNAL 0 /* no support for MSG_NOSIGNAL :( */ 
#endif

class StringMatrix;
class SchedWaveSpec;
typedef std::map<int, std::string> IntStringMap;
class ConnectionThread;

template <class T> std::string ToString(const T & t)
{
  std::ostringstream o;
  o << t;
  return o.str();
}

template <class T> T FromString(const std::string & s, bool *ok = 0)
{
  std::istringstream is(s);
  T t;
  is >> t;
  if (ok) *ok = !is.fail();
  return t;
}

std::string TimeText () 
{
  char buf[1024];
  time_t t = time(0);
  struct tm tms;
  localtime_r(&t, &tms);
  

  size_t sz = strftime(buf, sizeof(buf)-1, "%b %d %H:%M:%S", &tms);
  buf[sz] = 0;
  return buf;
}

struct Matrix
{
public:
  Matrix(const Matrix & mat) : d(0) { *this = mat; }
  Matrix(int m, int n) : m(m), n(n) { d = new double[m*n]; }
  ~Matrix() { delete [] d; }
  Matrix & operator=(const Matrix &rhs);
  Matrix & vertCat(const Matrix &rhs); // modified this
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

Matrix & Matrix::operator=(const Matrix &rhs)
{
  if (d) delete [] d;
  m = rhs.m; n = rhs.n;
  d = new double[m*n];
  std::memcpy(d, rhs.d, m*n*sizeof(double));
  return *this;
}

Matrix & Matrix::vertCat(const Matrix &rhs)
{
  Matrix mat(m+rhs.m, n);
  int i, j, i_rhs;
  for (i_rhs = 0, i = 0; i < mat.rows(); ++i) {
    for (j = 0; j < mat.cols(); ++j)
      if (i < rows() && j < cols()) 
        mat.at(i,j) = at(i,j);  
      else if (i >= rows() && i_rhs < rhs.rows() && j < rhs.cols())
        mat.at(i,j) = rhs.at(i_rhs, j);
      else 
        mat.at(i,j) = 0; // pad with zeroes?
    if (i >= rows()) ++i_rhs;
  }  
  return *this = mat;  
}

template <class T> struct CircBuf
{
  CircBuf(size_t n_elems) : n(n_elems ? n_elems : 1), ct(0) { buf.resize(n_elems); }
  ~CircBuf() { buf.clear(); }
  T & operator[](unsigned long long i) { return buf[(unsigned)(i%n)]; }
  void push(const T & t) { (*this)[ct++] = t;  }
  // use this to increment counter with the data already in the buf
  void push() { ct++; }
  T & next() { return (*this)[count()]; }
  unsigned long long count() const { return ct; }
  unsigned long countNormalized() const { return ct > n ? ct%n : ct; }
  size_t size() const { return n; }
  void clear() { ct = 0; }
private:
  std::vector<T> buf;
  size_t n;
  unsigned long long ct;
};

namespace 
{ // anonymous namespaced globales
  volatile struct Shm *shm = 0;
  int listen_fd = -1; /* Our listen socket.. */
  unsigned short listenPort = 3333;
  typedef std::map<ConnectionThread *, ConnectionThread *> ConnectedThreadsList;

  bool debug = false; // set with -d command-line switch to enable debug mode.

  unsigned n_threads;
  ConnectedThreadsList connectedThreads;
  pthread_mutex_t connectedThreadsLock = PTHREAD_MUTEX_INITIALIZER;

  std::string UrlEncode(const std::string &);
  std::string UrlDecode(const std::string &);
  std::string FormatPacketText(const std::string &, const NRTOutput *);


  const std::string & KernelVersion();
  bool IsKernel24() { return KernelVersion().substr(0, 4) == "2.4."; }
  bool IsKernel26() { return KernelVersion().substr(0, 4) == "2.6."; }

  std::string CompilerPath() { return "tcc/tcc"; }
  std::string IncludePath() { char buf[128]; return std::string(getcwd(buf, 128)) + "/include"; }
  std::string ModWrapperPath() { return IsKernel24() ? "./embc_mod_wrapper.o" : "runtime/embc_mod_wrapper.c"; }
  std::string MakefileTemplatePath() { return IsKernel26() ? "runtime/Kernel2.6MakefileTemplate" : ""; }
  std::string LdPath() { return "ld"; }
  std::string TmpPath() { return "/tmp/"; } // TODO handle Win32 tmp path..?
  std::string InsMod() { return "/sbin/insmod"; }
  std::string RmProg() { return "rm -f"; }
  bool FileExists(const std::string &f);
};  



struct DAQScanVec : public DAQScan
{
  std::vector<double> samples;
    
  void assign(const DAQScan *ds, unsigned maxData, double rangeMin, double rangeMax) { 
    static_cast<DAQScan &>(*this) = *ds; 
    samples.resize(nsamps);
    for (unsigned i = 0; i < nsamps; ++i)
      samples[i] = (ds->samps[i]/double(maxData) * (rangeMax-rangeMin)) + rangeMin;
  }
};

struct Matrix;

struct FSMSpecific
{
  FSMSpecific() 
    : fifo_in(RTOS::INVALID_FIFO), 
      fifo_out(RTOS::INVALID_FIFO), 
      fifo_trans(RTOS::INVALID_FIFO), 
      fifo_daq(RTOS::INVALID_FIFO), 
      fifo_nrt_output(RTOS::INVALID_FIFO),
      transBuf(2048), // store 2048 strate transitions in memory from transNotify thread
      daqBuf(128*2048), // store 128000 scans in memory from daq thread
      transNotifyThread(0), daqReadThread(0),
      daqNumChans(0), daqMaxData(1), aoMaxData(1),
      daqRangeMin(0.), daqRangeMax(5.)
  {
    pthread_mutex_init(&msgFifoLock, 0);
    pthread_mutex_init(&transNotifyLock, 0);
    pthread_mutex_init(&daqLock, 0);
    pthread_cond_init(&transNotifyCond, 0);
  }
  ~FSMSpecific() 
  {
    pthread_cond_destroy(&transNotifyCond);
    pthread_mutex_destroy(&transNotifyLock);
    pthread_mutex_destroy(&daqLock);
    pthread_mutex_destroy(&msgFifoLock);
  }

  Matrix getDAQScans(); /**< returns an MxN matrix, where each row is a scan 
                           ideal for sending to Matlab.. */


  RTOS::FIFO fifo_in, fifo_out, fifo_trans, fifo_daq, fifo_nrt_output;
  pthread_mutex_t msgFifoLock, transNotifyLock, daqLock;
  pthread_cond_t transNotifyCond;
  CircBuf<StateTransition> transBuf;
  CircBuf<DAQScanVec> daqBuf;
  pthread_t transNotifyThread, daqReadThread, nrtReadThread;
  unsigned daqNumChans, daqMaxData, aoMaxData;
  double daqRangeMin, daqRangeMax;

  void *transNotifyThrFun();
  void *daqThrFun();
  void *nrtThrFun();
  void doNRT_IP(const NRTOutput *, bool isUDP) const;
};

static void *transNotifyThrWrapper(void *);
static void *daqThrWrapper(void *);
static void *nrtThrWrapper(void *);
  
static FSMSpecific fsms[NUM_STATE_MACHINES];

static std::vector<double> splitNumericString(const std::string & str,
                                              const std::string &delims = ",",
                                              bool allowEmpties = true);
static std::vector<std::string> splitString(const std::string &str,
                                            const std::string &delim = ",",
                                            bool trim_whitespace = true,
                                            bool skip_empties = true);

template <typename T> class shallow_copy
{
  struct Impl
  {
    T *t;
    volatile int nrefs;
    Impl() : t(0), nrefs(0) {}
    ~Impl() {}
  };
  mutable Impl *p;

  void ref_incr() const { ++p->nrefs; }
  void ref_decr() const { 
    if (!--p->nrefs) { 
      delete p->t; p->t = 0; delete p; 
    } 
  }

public:

  shallow_copy(T * t = new T) {  p = new Impl; p->t = t; ref_incr(); }
  ~shallow_copy() { ref_decr(); p = 0; }
  shallow_copy(const shallow_copy &r) { p = r.p; ref_incr();  }
  shallow_copy & operator=(const shallow_copy & r) { ref_decr(); p = r.p; ref_incr();  }
  // makes a private copy
  void duplicate() { Impl * i = new Impl(*p);  i->nrefs = 1; i->t = new T(*p->t); ref_decr(); p = i; }

  T & get() { return *p->t; }
  const T & get() const { return *p->t; }

  unsigned nrefs() const { return p->nrefs; }
};

/// A thread-safe logger class.  On destruction
/// the singleton logstream is written-to, in a thread-safe manner.
/// Note it uses shallow copy of data so it's possible to pass this by
/// value in and out of functions.  
class Log : protected shallow_copy<std::string>
{
  static pthread_mutex_t mut; // this is what makes it thread safe
  static std::ostream * volatile logstream;
public:
  static void setLogStream(std::ostream &);

  Log();
  ~Log() { if (nrefs() == 1) { MutexLocker m(mut); (*logstream) << get(); get() = ""; } }
  
  template <typename T> Log & operator<<(const T & t) 
  {
    MutexLocker m(mut);
    std::ostringstream os;
    os << get() << t;  
    get() = os.str();
    return *this;
  }

  Log & operator << (std::ostream & (*pf)(std::ostream &)) {
    MutexLocker m(mut);
    std::ostringstream os;
    os << get() << pf;  
    get() = os.str();
    return *this;
  }
}; 

pthread_mutex_t Log::mut = PTHREAD_MUTEX_INITIALIZER;
std::ostream * volatile Log::logstream = 0; // in case we want to log other than std::cerr..

Log::Log() 
{ 
  MutexLocker m(mut);
  if (!logstream) logstream = &std::cerr; 
}

void Log::setLogStream(std::ostream &os) 
{
  MutexLocker ml(mut);
  logstream  = &os;
}

extern "C" { static void * threadfunc_wrapper(void *arg); }

class ConnectionThread
{
public:
  ConnectionThread();
  ~ConnectionThread();
  bool isRunning() const {return thread_running;}
  bool hasRun() const {return thread_ran;}
  bool start(int socket_fd, const std::string & remoteHost = "unknown");
private:
  static int id;

  // FSM/Globals mutex lock around global data so threads don't stomp on each other
  pthread_t handle;
  int sock, myid, fsm_id;
  unsigned shm_num;
  std::string remoteHost;
  volatile bool thread_running, thread_ran;


  struct TLog : public ::Log
  {
  public:
    TLog(int myid, const std::string & remoteHost, int fsm_id) { (*this) << "[" << TimeText() << " FSM " << fsm_id << " Thread " << myid << "  (" << remoteHost << ")] "; }
  };
  
  TLog Log() const { return TLog(myid, remoteHost, fsm_id); }
 
  ShmMsg msg; //< needed to put this in class data because it broke the stack it's so freakin' big now

  // Functions to send commands to the realtime process via the rt-fifos
  void sendToRT(ShmMsgID cmd); // send a simple command, one of RESET_, PAUSEUNPAUSE, INVALIDATE. Upon return we know the command completed.
  void sendToRT(ShmMsg & msg); // send a complex command, wait for a reply which gets put back into 'msg'.  Upon return we know the command completed.
  void getFSMSizeFromRT(unsigned & rows_out, unsigned & cols_out);
  unsigned getNumInputEventsFromRT(void);
  int sockSend(const std::string & str) ;
  int sockSend(const void *buf, size_t len, bool is_binary = false, int flags = 0);
  int sockReceiveData(void *buf, int size, bool is_binary = true);
  std::string sockReceiveLine();
  bool matrixToRT(const Matrix & m, unsigned numEvents, unsigned numSchedWaves, const std::string & inChanType, unsigned readyForTrialState,  const std::string & outputSpecStr, bool state0_fsm_swap_flg);
  bool matrixToRT(const StringMatrix & m,
                  const std::string & globals,
                  const std::string & initfunc,
                  const std::string & cleanupfunc,
                  const std::string & transitionfunc,
                  const std::string & tickfunc,
                  const IntStringMap & entryfuncs,
                  const IntStringMap & exitfuncs, 
                  const IntStringMap & entrycodes,
                  const IntStringMap & exitcodes,
                  const std::string & inChanType,
                  const std::vector<int> & inputSpec,
                  const std::vector<OutputSpec> & outputSpec,
                  const std::vector<SchedWaveSpec> & schedWaveSpec,
                  unsigned readyForTrialJumpState, 
                  bool state0_fsm_swap_flg);
  StringMatrix matrixFromRT(); ///< get the state matrix from the FSM, note it may contain embedded C
  void doNotifyEvents(bool full = false); ///< if in full mode, actually write out the text of the event, one per line, otherwise write only the character 'e' with no newline
  std::vector<OutputSpec> parseOutputSpecStr(const std::string & str);
  std::vector<int>  parseInputSpecStr(const std::string &);
  std::vector<SchedWaveSpec> parseSchedWaveSpecStr(const std::string &);
  bool sendStringMatrix(const StringMatrix &m);
  bool doSetStateProgram();
  std::string newShmName();
  bool compileProgram(const std::string & prog_name, const std::string & prog_txt) const;
  bool loadModule(const std::string & prog_name) const;
  bool unlinkModule(const std::string & program_name) const;
  bool doCompileLoadProgram(const std::string &prog_name, const std::string & program_text) const;
  bool System(const std::string &cmd) const;

  Matrix doGetTransitionsFromRT(int & first, int & last, int & state);
  Matrix doGetTransitionsFromRT(int & first, int & last) { int dummy; return doGetTransitionsFromRT(first, last, dummy); }
  Matrix doGetTransitionsFromRT(int & first) { int dummy = -1; return doGetTransitionsFromRT(first, dummy); }

  IntStringMap parseIntStringMapBlock(const std::string &); ///< not static because it needs object for logging errors
  static void parseStringTable(const char *stable, StringMatrix &m);
  static std::string genStringTable(const StringMatrix &m);
  static std::string genStringTable(const Matrix &m);
  

  // the extern "C" wrapper func passed to pthread_create
  friend void *threadfunc_wrapper(void *);
  void *threadFunc();
};


/* some statics for class ConnectionThread */
int ConnectionThread::id = 0;

ConnectionThread::ConnectionThread() : sock(-1), thread_running(false), thread_ran(false) { myid = id++; fsm_id = 0; shm_num = 0; }
ConnectionThread::~ConnectionThread()
{ 
  if (sock > -1)  ::shutdown(sock, SHUT_RDWR), ::close(sock), sock = -1;
  if (isRunning())  pthread_join(handle, NULL), thread_running = false;
  Log() <<  "deleted." << std::endl;
}
extern "C" 
{
  static void * threadfunc_wrapper(void *arg)   
  { 
    pthread_detach(pthread_self());
    ConnectionThread *me = static_cast<ConnectionThread *>(arg);
    void * ret = me->threadFunc();  
    // now, try and reap myself..
    MutexLocker ml(connectedThreadsLock);
    ConnectedThreadsList::iterator it;
    if ( (it = connectedThreads.find(me)) != connectedThreads.end() ) {
        delete me;
        connectedThreads.erase(it);
        n_threads--;
        Log() << "Auto-reaped dead thread, have " << n_threads << " still running." << std::endl; 
    }
    return ret;
  }
}
bool ConnectionThread::start(int sock_fd, const std::string & rhost)
{
  if (isRunning()) return false;
  sock = sock_fd;  
  remoteHost = rhost;
  return 0 == pthread_create(&handle, NULL, threadfunc_wrapper, static_cast<void *>(this));
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

static void attachShm()
{
  // first, connect to the shm buffer..
  RTOS::ShmStatus shmStatus;
  void *shm_notype = RTOS::shmAttach(FSM_SHM_NAME, FSM_SHM_SIZE, &shmStatus);
    
  if (!shm_notype)
    throw Exception(std::string("Cannot connect to shared memory region named `") + FSM_SHM_NAME + "', error was: " + RTOS::statusString(shmStatus));
  shm = const_cast<volatile Shm *>(static_cast<Shm *>(shm_notype));
    
  if (shm->magic != FSM_SHM_MAGIC)
    throw Exception("Attached to shared memory buffer, but the magic number is invalid.  Is the FSM kernel module loaded?\n");
}
  
static void fifoReadAllAvail(RTOS::FIFO f)
{
  int numStale = 0, numLeft = 0;
  numStale = RTOS::fifoNReadyForReading(f);
  if (numStale == -1)  // argh, ioctl returned error!
    throw Exception(std::string("fifoReadAllAvail: ") + strerror(errno));
    
  numLeft = numStale;
  while (numLeft > 0) { // keep consuming up to 1kb of data per iteration..
    char dummyBuf[1024];
    int nread = RTOS::readFifo(f, dummyBuf, MIN(sizeof(dummyBuf), (unsigned)numLeft));
    if (nread <= 0)
      throw Exception(std::string("fifoReadAllAvail: ") + strerror(errno));
    numLeft -= nread;
  }
    
  if (numStale) 
    Log() << "Cleared " << numStale << " old bytes from input fifo " 
          << f << "." << std::endl; 
}

static void openFifos()
{
  for (int f = 0; f < NUM_STATE_MACHINES; ++f) {
    FSMSpecific & fsm = fsms[f];
    fsm.fifo_in = RTOS::openFifo(shm->fifo_out[f]);
    if (fsm.fifo_in == RTOS::INVALID_FIFO) throw Exception ("Could not open RTF fifo_in for reading");
    // Clear any pending/stale data in case we crashed last time and
    // couldn't read all data off reply fifo
    fifoReadAllAvail(fsm.fifo_in);
    fsm.fifo_out = RTOS::openFifo(shm->fifo_in[f], RTOS::Write);
    if (fsm.fifo_out == RTOS::INVALID_FIFO) throw Exception ("Could not open RTF fifo_out for writing");
    fsm.fifo_trans = RTOS::openFifo(shm->fifo_trans[f], RTOS::Read);
    if (fsm.fifo_trans == RTOS::INVALID_FIFO) throw Exception ("Could not open RTF fifo_trans for reading");
    fsm.fifo_daq = RTOS::openFifo(shm->fifo_daq[f], RTOS::Read);
    if (fsm.fifo_daq == RTOS::INVALID_FIFO) throw Exception ("Could not open RTF fifo_daq for reading");
    fsm.fifo_nrt_output = RTOS::openFifo(shm->fifo_nrt_output[f], RTOS::Read);
    if (fsm.fifo_nrt_output == RTOS::INVALID_FIFO) throw Exception ("Could not open RTF fifo_nrt_output for reading");
  }
}

static void closeFifos()
{
  for (int f = 0; f < NUM_STATE_MACHINES; ++f) {
    FSMSpecific & fsm = fsms[f];
    if (fsm.fifo_in != RTOS::INVALID_FIFO) RTOS::closeFifo(fsm.fifo_in);
    if (fsm.fifo_out != RTOS::INVALID_FIFO) RTOS::closeFifo(fsm.fifo_out);
    if (fsm.fifo_trans != RTOS::INVALID_FIFO) RTOS::closeFifo(fsm.fifo_trans);
    if (fsm.fifo_daq != RTOS::INVALID_FIFO) RTOS::closeFifo(fsm.fifo_daq);
    if (fsm.fifo_nrt_output != RTOS::INVALID_FIFO) RTOS::closeFifo(fsm.fifo_nrt_output);
    fsm.fifo_nrt_output = fsm.fifo_daq = fsm.fifo_trans = fsm.fifo_in = fsm.fifo_out = RTOS::INVALID_FIFO;
  }
}

static void createTransNotifyThreads()
{
  for (long f = 0; f < NUM_STATE_MACHINES; ++f) {
    int ret = pthread_create(&fsms[f].transNotifyThread, NULL, transNotifyThrWrapper, reinterpret_cast<void *>(f));
    if (ret) 
      throw Exception("Could not create a required thread, 'state transition notify thread'!");
  }
}
  
static void createDAQReadThreads()
{
  for (long f = 0; f < NUM_STATE_MACHINES; ++f) {
    int ret = pthread_create(&fsms[f].daqReadThread, NULL, daqThrWrapper, reinterpret_cast<void *>(f));
    if (ret) 
      throw Exception("Could not create a required thread, 'daq fifo read thread'!");
  }
}

static void createNRTReadThreads()
{
  for (long f = 0; f < NUM_STATE_MACHINES; ++f) {
    int ret = pthread_create(&fsms[f].nrtReadThread, NULL, nrtThrWrapper, reinterpret_cast<void *>(f));
    if (ret) 
      throw Exception("Could not create a required thread, 'nrt output fifo read thread'!");
  }
}

static void init()
{
  if (::num_procs_of_my_exe_no_children() > 1) 
    throw Exception("It appears another copy of this program is already running!\n");
#if !defined(EMULATOR)  
  if (::geteuid() != 0)
    throw Exception("Need to be root or setuid-root to run this program as it requires the ability to load kernel modules!");
  std::string missing;
  if (IsKernel24()) {
    if ( !FileExists(missing = CompilerPath()) || !FileExists(missing = ModWrapperPath()) ) 
      throw Exception(std::string("Required file or program '") + missing + "' is not found!");
  } else if (IsKernel26()) {
    if ( !FileExists(missing = MakefileTemplatePath()) || !FileExists(missing = ModWrapperPath()) )
      throw Exception(std::string("Required file or program '") + missing + "' is not found!");
  } else 
      throw Exception("Could not determine Linux kernel version!  Is this Linux??");
#endif
  attachShm();
  openFifos();
  createTransNotifyThreads();
  createDAQReadThreads();
  createNRTReadThreads();
}
  
static void cleanup(void)
{
  if (listen_fd >= 0) { ::close(listen_fd);  listen_fd = -1; }
  closeFifos();
  if (shm) { RTOS::shmDetach((void *)shm); shm = 0; }
}


static void handleArgs(int argc, char *argv[])
{
  int opt;
  while ( ( opt = getopt(argc, argv, "dl:")) != -1 ) {
    switch(opt) {
    case 'd': debug = true; break;
    case 'l': 
      listenPort = atoi(optarg);
      if (! listenPort) throw Exception ("Could not parse listen port.");
      break;
    default:
      throw Exception(std::string("Unknown command line parameters.  Usage: ")
                      + argv[0] + " [-l listenPort] [-d]");
      break;
    }
  }
}

static void sighandler(int sig)
{
    
  if (sig == SIGPIPE) return; // ignore SIGPIPE..
    
  /*try {
    sendToRT(PAUSEUNPAUSE);
    } catch (...) {}*/
    
  Log() << "Caught signal " << sig << " cleaning up..." << std::endl; 
    
  cleanup();
    
  //::kill(0, SIGKILL); /* suicide!! this is to avoid other threads from getting this signal */
  _exit(1);
}


struct StringMatrix : public std::vector<std::vector<std::string> >
{
public:
  StringMatrix(unsigned m, unsigned n);
  unsigned rows() const { return size(); }
  unsigned cols() const { return rows() ? front().size() : 0; }
  std::string &at(unsigned r, unsigned c) { return (*this)[r][c]; }
  const std::string &at(unsigned r, unsigned c) const { return (*this)[r][c]; }
};

StringMatrix::StringMatrix(unsigned m, unsigned n)
{
  resize(m, value_type(n));
}

struct SchedWaveSpec : public SchedWave
{
  int id;
  int in_evt_col;
  int out_evt_col;
  int dio_line;
};

class Timer
{
public:
  Timer() { reset(); }
  void reset();
  double elapsed() const; // returns number of seconds since ctor or reset() was called 
  static double now() { struct timeval ts; ::gettimeofday(&ts, 0); return double(ts.tv_sec) + ts.tv_usec/1e6; }
private:
  struct timeval ts;
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

static void doServer(void)
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

  Log() << "FSM Server version " << VersionSTR << std::endl;
  Log() << "Listening for connections on port: " << listenPort << std::endl; 

  int parm = 1;
  if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &parm, sizeof(parm)) )
    Log() << "Error: setsockopt returned " << ::strerror(errno) << std::endl; 
  
  if ( ::bind(listen_fd, (struct sockaddr *)&inaddr, addr_sz) != 0 ) 
    throw Exception(std::string("bind: ") + strerror(errno));
  
  if ( ::listen(listen_fd, 1) != 0 ) 
    throw Exception(std::string("listen: ") + strerror(errno));

  while (1) {
    int sock;
    if ( (sock = ::accept(listen_fd, (struct sockaddr *)&inaddr, &addr_sz)) < 0 ) 
      throw Exception(std::string("accept: ") + strerror(errno));

    if (::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &parm, sizeof(parm)) )
      Log() << "Error: setsockopt returned " << ::strerror(errno) << std::endl; 
    
    ConnectionThread *conn = new ConnectionThread;
    pthread_mutex_lock(&connectedThreadsLock);
    if ( conn->start(sock, inet_ntoa(inaddr.sin_addr)) ) {
      ++n_threads;
      connectedThreads[conn] = conn;
      Log() << "Started new thread, now have " << n_threads << " total." 
            << std::endl;
    } else 
      delete conn;
    pthread_mutex_unlock(&connectedThreadsLock);
  }
}

int main(int argc, char *argv[])
{
  int ret = 0;

  // install our signal handler that tries to pause the rt-process and
  // quit the program
  signal(SIGINT, sighandler);
  signal(SIGPIPE, sighandler);
  signal(SIGQUIT, sighandler);
  signal(SIGHUP, sighandler);
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

void *ConnectionThread::threadFunc(void)
{
  Timer connectionTimer;

  Log() << "Connection received from host " << remoteHost << std::endl;

  thread_running = true;
  thread_ran = true;
    
  std::string line;
  int count;

  while ( (line = sockReceiveLine()).length() > 0) {
      
    bool cmd_error = true;
    if (line.find("SET STATE PROGRAM") == 0) {
      cmd_error = !doSetStateProgram();
    } else if (line.find("SET STATE MATRIX") == 0) {
      /* FSM Upload.. */
        
      // determine M and N
      std::string::size_type pos = line.find_first_of("0123456789");

      if (pos != std::string::npos) {
        unsigned m = 0, n = 0, num_Events = 0, num_SchedWaves = 0, readyForTrialState = 0, num_ContChans = 0, num_TrigChans = 0, num_Vtrigs = 0, state0_fsm_swap_flg;
        std::string outputSpecStr = "";
        std::string inChanType = "ERROR";
        std::stringstream s(line.substr(pos));
        s >> m >> n >> num_Events >> num_SchedWaves >> inChanType >> readyForTrialState >> num_ContChans >> num_TrigChans >> num_Vtrigs >> outputSpecStr >> state0_fsm_swap_flg;
        if (m && n) {
          if (outputSpecStr.length() == 0 && (num_ContChans || num_TrigChans || num_Vtrigs || num_SchedWaves)) {
            // old FSM client, so build an output spec string for them
            Log() << "Client appears to use old SET STATE MATRIX interface, trying to create an output spec string that matches.\n"; 
            std::stringstream s("");
            if (num_ContChans+num_TrigChans) 
              s << "\1" << "dout" << "\2" << "0-" << (num_ContChans+num_TrigChans);
            if (num_Vtrigs)
              s << "\1" << "sound" << "\2" << fsm_id;
            if (num_SchedWaves)
              s << "\1" << "sched_wave" << "\2" << "Ignored";
            outputSpecStr = s.str();
          } else
            outputSpecStr = UrlDecode(outputSpecStr);

          // guard against memory hogging DoS
          if (m*n*sizeof(double) > FSM_MEMORY_BYTES) {
            Log() << "Error, incoming matrix would exceed cell limit of " << FSM_MEMORY_BYTES/sizeof(double) << std::endl; 
            break;
          }
          if ( (count = sockSend("READY\n")) <= 0 ) {
            Log() << "Send error..." << std::endl; 
            break;
          }
          Log() << "Getting ready to receive matrix of  " << m << "x" << n << " with one row for (" << num_Events << ") event mapping and row(s) for " <<  num_SchedWaves << " scheduled waves " << std::endl; 
            
          Matrix mat (m, n);
          count = sockReceiveData(mat.buf(), mat.bufSize());
          if (count == (int)mat.bufSize()) {
            cmd_error = !matrixToRT(mat, num_Events, num_SchedWaves, inChanType, readyForTrialState, outputSpecStr, state0_fsm_swap_flg);
          } else if (count <= 0) {
            break;
          }
        } 
      }
    } else if (line.find("GET STATE MATRIX") == 0) {
      cmd_error = !sendStringMatrix(matrixFromRT());
    } else if (line.find("GET STATE PROGRAM") == 0) {
      msg.id = GETFSM;
      sendToRT(msg);
      std::auto_ptr<char> program(new char[msg.u.fsm.program_len+1]);
      memcpy(program.get(), msg.u.fsm.program_z, msg.u.fsm.program_z_len);
      inflateInplace(program.get(), msg.u.fsm.program_z_len, msg.u.fsm.program_len);
      program.get()[msg.u.fsm.program_len] = 0;
      std::vector<std::string> lines = splitString(program.get(), "\n", false, false);
      std::stringstream s;
      s << "LINES " << lines.size() << "\n" << program.get();
      std::string str = s.str();
      if (str[str.length()-1] != '\n') str = str + "\n";
      sockSend(str);
      cmd_error = false;
    } else if (line.find("INITIALIZE") == 0) {
      sendToRT(RESET_);
      cmd_error = false;
    } else if (line.find("HALT") == 0) {
      msg.id = GETPAUSE;
      sendToRT(msg);
      if ( ! msg.u.is_paused ) 
        sendToRT(PAUSEUNPAUSE);
      cmd_error = false;
    } else if (line.find("RUN") == 0) {
      msg.id = GETPAUSE;
      sendToRT(msg);
      if ( msg.u.is_paused ) 
        sendToRT(PAUSEUNPAUSE);
      cmd_error = false;        
    } else if (line.find("FORCE TIME UP") == 0 ) { 
      sendToRT(FORCETIMESUP);
      cmd_error = false;
    } else if (line.find("READY TO START TRIAL") == 0 ) { 
      sendToRT(READYFORTRIAL);
      cmd_error = false;
    } else if (line.find("TRIGEXT") == 0 ) { 
      int trigmask = 0;
      std::string::size_type pos = line.find_first_of("-0123456789");
      if (pos != std::string::npos) {
        std::stringstream s(line.substr(pos));
        s >> trigmask;
        msg.id = FORCEEXT;
        msg.u.forced_triggers = trigmask;
        sendToRT(msg);
        cmd_error = false;
      }
    } else if (line.find("BYPASS DOUT") == 0) {
      int bypassmask = 0;
      std::string::size_type pos = line.find_first_of("-0123456789");
      if (pos != std::string::npos) {
        std::stringstream s(line.substr(pos));
        s >> bypassmask;
        msg.id = FORCEOUTPUT;
        msg.u.forced_outputs = bypassmask;
        sendToRT(msg);
        cmd_error = false;
      }
    } else if (line.find("FORCE STATE") == 0 ) { 
      unsigned forced_state = 0;
      std::string::size_type pos = line.find_first_of("0123456789");
      if (pos != std::string::npos) {
        unsigned rows, cols;
        getFSMSizeFromRT(rows, cols);
        std::stringstream s(line.substr(pos));
        s >> forced_state;
        if (forced_state < rows) { // ensure legal state here..
          msg.id = FORCESTATE;
          msg.u.forced_state = forced_state;
          sendToRT(msg);
          cmd_error = false;
        }
      }
    } else if (line.find("GET EVENT COUNTER") == 0) {
      msg.id = TRANSITIONCOUNT;
      sendToRT(msg);
      std::stringstream s;
      s << msg.u.transition_count << std::endl;
      sockSend(s.str());
      cmd_error = false;
    } else if (line.find("GET VARLOG COUNTER") == 0) {
      msg.id = LOGITEMCOUNT;
      sendToRT(msg);
      std::stringstream s;
      s << msg.u.log_item_count << std::endl;
      sockSend(s.str());
      cmd_error = false;
    } else if (line.find("IS RUNNING") == 0) {
      msg.id = GETPAUSE;
      sendToRT(msg);
      std::stringstream s;
      s << !msg.u.is_paused << std::endl;
      sockSend(s.str());
      cmd_error = false;
    } else if (line.find("GET TIME") == 0 && line.find(", EVENTS, AND STATE") == std::string::npos) {
      msg.id = GETRUNTIME;
      sendToRT(msg);
      std::stringstream s;
      s << static_cast<double>(msg.u.runtime_us)/1000000.0 << std::endl;
      sockSend(s.str());
      cmd_error = false;        
    } else if (line.find("GET CURRENT STATE") == 0) {
      msg.id = GETCURRENTSTATE;
      sendToRT(msg);
      std::stringstream s;
      s << msg.u.current_state << std::endl;
      sockSend(s.str());
      cmd_error = false;        
    } else if (line.find("GET EVENTS") == 0) {
      std::string::size_type pos = line.find_first_of("0123456789");
      if (pos != std::string::npos) {
        std::stringstream s(line.substr(pos));
        int first = -1, last = -1;
        s >> first >> last;
        Matrix mat = doGetTransitionsFromRT(first, last);

        std::ostringstream os;
        os << "MATRIX " << mat.rows() << " " << mat.cols() << std::endl;
        sockSend(os.str());

        line = sockReceiveLine(); // wait for "READY" from client

        if (line.find("READY") != std::string::npos) {
            sockSend(mat.buf(), mat.bufSize(), true);
            cmd_error = false;
        }
      }
    } else if (line.find("GET VARLOG") == 0) {
      std::string::size_type pos = line.find_first_of("0123456789");
      if (pos != std::string::npos) {
        std::stringstream s(line.substr(pos));
        int first = -1, last = -1, n_items = 0;
        s >> first >> last;

        // query the count first to check sanity
        msg.id = LOGITEMCOUNT;
        sendToRT(msg);
          
        n_items = msg.u.log_item_count;
        if (first < 0) first = 0;
        if (last < 0) last = 0;
        if (last > n_items) last = n_items;
        int desired = last-first+1;
        if (first <= last && desired <= n_items) {
          int received = 0, ct = 0;
          StringMatrix mat(desired, 3);
            

          // keep 'downloading' the matrix from RT until we get all the transitions we require
          while (received < desired) {
            msg.id = LOGITEMS;
            msg.u.log_items.num = desired - received;
            msg.u.log_items.from = first + received;
            sendToRT(msg);
            received += (int)msg.u.log_items.num;              
            for (int i = 0; i < (int)msg.u.log_items.num; ++i, ++ct) {
              struct VarLogItem & v = msg.u.log_items.items[i];
              mat.at(ct,0) = ToString(v.ts);
              mat.at(ct,1) = v.name;
              mat.at(ct,2) = ToString(v.value);
            }
          }
          cmd_error = !sendStringMatrix(mat);
        } else {
          cmd_error = sendStringMatrix(StringMatrix(0,3));
        }
      }
    } else if ( line.find("EXIT") == 0 || line.find("BYE") == 0 || line.find("QUIT") == 0) {
      Log() << "Graceful exit requested." << std::endl;
      break;
    } else if (line.find("NOTIFY EVENTS") == 0) {
      std::string::size_type pos = line.find("VERBOSE");
      bool verbose = pos != std::string::npos;
      sockSend("OK\n"); // tell them we accept the command..
      doNotifyEvents(verbose); // this doesn't return for a LONG time normally..
      break; // if we return from the above, means there is a socket error
    } else if (line.find("NOOP") == 0) {
      // noop is just used to test the connection, keep it alive, etc
      // it doesn't touch the shm...
      cmd_error = false;        
    } else if (line.find("START DAQ") == 0) { // START DAQ
      // determine chans and range
      std::string::size_type pos = line.find_first_of("0123456789");

      if (pos != std::string::npos) {
        std::string chanstr, rangestr;
        std::stringstream s(line.substr(pos));
        s >> chanstr >> rangestr;
        std::vector<double> chans = splitNumericString(chanstr);
        std::vector<double> ranges = splitNumericString(rangestr);
        unsigned chanMask = 0, nChans = 0;
        for (unsigned i = 0; i < chans.size(); ++i) {
          unsigned ch = i;
          if (ch < sizeof(int)*8 && !(chanMask&(0x1<<ch)))
            (chanMask |= 0x1<<ch), nChans++;
        }
        if (!chanMask || !nChans || ranges.size() != 2) {
          Log() << "Chan or range spec for START DAQ has invalid chanspec or rangespec" << std::endl; 
        } else {
          msg.id = STARTDAQ;
          msg.u.start_daq.chan_mask = chanMask;
          msg.u.start_daq.range_min = int(ranges[0]*1e6);
          msg.u.start_daq.range_max = int(ranges[1]*1e6);
          msg.u.start_daq.started_ok = 0;
          sendToRT(msg);
          if (msg.u.start_daq.started_ok) {
            cmd_error = false;        
            pthread_mutex_lock(&fsms[fsm_id].daqLock);
            fsms[fsm_id].daqNumChans = nChans;
            fsms[fsm_id].daqMaxData = msg.u.start_daq.maxdata;
            //fsms[fsm_id].daqRangeMin = msg.u.start_daq.range_min/1e6;
            //fsms[fsm_id].daqRangeMax = msg.u.start_daq.range_max/1e6;
            pthread_mutex_unlock(&fsms[fsm_id].daqLock);
          } else { 
            Log() << "RT Task refused to do start a DAQ task -- probably invalid parameters are to blame" << std::endl;
          }
        }
      }
    } else if (line.find("STOP DAQ") == 0) { // STOP DAQ
      sendToRT(STOPDAQ);
      //fifoReadAllAvail(fifo_daq); // discard fifo data for stopped scan
      // FIXME avoid race coditions with daq thread
      pthread_mutex_lock(&fsms[fsm_id].daqLock);
      fsms[fsm_id].daqBuf.clear();
      pthread_mutex_unlock(&fsms[fsm_id].daqLock);
      cmd_error = false;        
    } else if (line.find("GET DAQ SCANS") == 0) { // GET DAQ SCANS

      Matrix mat = fsms[fsm_id].getDAQScans();
      std::ostringstream os;
      os << "MATRIX " << mat.rows() << " " << mat.cols() << std::endl; 
      sockSend(os.str());

      line = sockReceiveLine(); // wait for "READY" from client
            
      if (line.find("READY") != std::string::npos) {
        sockSend(mat.buf(), mat.bufSize(), true);
        cmd_error = false;
      }
 
    } else if (line.find("SET AO WAVE") == 0) { // SET AO WAVE

      // determine M N id aoline loop
      std::string::size_type pos = line.find_first_of("0123456789");

      if (pos != std::string::npos) {
        unsigned m = 0, n = 0, id = 0, aoline = 0, loop = 0;
        std::stringstream s(line.substr(pos));
        s >> m >> n >> id >> aoline >> loop;
        if (m && n) {
          // guard against memory hogging DoS
          if (m*n*sizeof(double) > FSM_MEMORY_BYTES) {
            Log() << "Error, incoming matrix would exceed cell limit of " << FSM_MEMORY_BYTES/sizeof(double) << std::endl; 
            break;
          }
          if ( (count = sockSend("READY\n")) <= 0 ) {
            Log() << "Send error..." << std::endl; 
            break;
          }
          Log() << "Getting ready to receive AO wave matrix sized " << m << "x" << n << std::endl; 
            
          Matrix mat (m, n);
          count = sockReceiveData(mat.buf(), mat.bufSize());
          if (count == (int)mat.bufSize()) {
            msg.id = GETAOMAXDATA;
            sendToRT(msg);
            fsms[fsm_id].aoMaxData = msg.u.ao_maxdata;
            msg.id = AOWAVE;
            msg.u.aowave.id = id;
            msg.u.aowave.aoline = aoline;
            msg.u.aowave.loop = loop;
            // scale data from [-1,1] -> [0,aoMaxData]
            for (unsigned i = 0; i < n && i < AOWAVE_MAX_SAMPLES; ++i) {
              msg.u.aowave.samples[i] = static_cast<unsigned short>(((mat.at(0, i) + 1.0) / 2.0) * fsms[fsm_id].aoMaxData);
              if (m > 1)
                msg.u.aowave.evt_cols[i] = static_cast<signed char>(mat.at(1, i));
              else
                msg.u.aowave.evt_cols[i] = -1;
            }
            msg.u.aowave.nsamples = n;
            // send to kernel
            sendToRT(msg);
          } else if (count <= 0) {
            break;
          }
        } else {
          // indicates we are clearing an existing wave
          msg.id = AOWAVE;
          msg.u.aowave.id = id;
          msg.u.aowave.nsamples = 0;
          sendToRT(msg);
        }
        cmd_error = false;        
      }
    } else if (line.find("GET STATE MACHINE") == 0) { // GET STATE MACHINE
      std::ostringstream s;
      s << fsm_id << "\n";      
      sockSend(s.str());
      cmd_error = false;
    } else if (line.find("GET NUM STATE MACHINES") == 0) { // GET NUM STATE MACHINES
      std::ostringstream s;
      s << NUM_STATE_MACHINES << "\n";      
      sockSend(s.str());
      cmd_error = false;
    } else if (line.find("SET STATE MACHINE") == 0) { // SET STATE MACHINE
      // determine param
      std::string::size_type pos = line.find_first_of("0123456789");
      if (pos != std::string::npos) {
        std::istringstream s(line.substr(pos));
        unsigned in_id = NUM_STATE_MACHINES;
        s >> in_id;
        if (in_id < NUM_STATE_MACHINES) {
          cmd_error = false;
          fsm_id = in_id;
        }
      }
    } else if (line.find("GET LASTERROR") == 0) { // GET LASTERROR
      msg.id = GETERROR;
      msg.u.error[0] = 0;
      sendToRT(msg);
      if (!strlen(msg.u.error)) {
        sockSend("None.\n");
      } else
        sockSend(std::string(msg.u.error) + "\n");
      cmd_error = false;
    } else if (line.find("GET TIME, EVENTS, AND STATE") == 0) { // GET TIME AND EVENTS   takes 1 param, a state id to start from 
      msg.id = GETRUNTIME;
      sendToRT(msg);
      double currentTime  = static_cast<double>(msg.u.runtime_us)/1000000.0;
      std::string::size_type pos = line.find_first_of("0123456789");
      if (pos != std::string::npos) {
        std::stringstream s(line.substr(pos));
        int first = -1, last = -1, state = 0;
        s >> first;
        Matrix mat = doGetTransitionsFromRT(first, last, state);

        std::ostringstream os;
        os << "TIME " << currentTime << "\n";
        os << "STATE " << state << "\n";
        os << "EVENT COUNTER " << (last-first+1) << "\n";
        os << "MATRIX " << mat.rows() << " " << mat.cols() << std::endl;
        sockSend(os.str());

        line = sockReceiveLine(); // wait for "READY" from client

        if (line.find("READY") != std::string::npos) {
            sockSend(mat.buf(), mat.bufSize(), true);
            cmd_error = false;
        }
      }
    }

    if (cmd_error) {
      sockSend("ERROR\n"); 
    } else {
      sockSend("OK\n"); 
    }

  }
    
  Log() << "Connection to host " << remoteHost << " ended after " << connectionTimer.elapsed() << " seconds." << std::endl; 
    

  ::shutdown(sock, SHUT_RDWR);
  ::close(sock);
  sock = -1;
  thread_running = false;
  Log() << " thread exit." << std::endl; 
  return 0;
}

void ConnectionThread::sendToRT(ShmMsg & msg) // note param name masks class member
{
  MutexLocker locker(fsms[fsm_id].msgFifoLock);

  std::memcpy(const_cast<ShmMsg *>(&shm->msg[fsm_id]), &msg, sizeof(msg));

  FifoNotify_t dummy = 1;    
    
  if ( RTOS::writeFifo(fsms[fsm_id].fifo_out, &dummy, sizeof(dummy)) != sizeof(dummy) )
    throw Exception("INTERNAL ERROR: Could not write a complete message to the fifo!");

  int err; 
  // now wait synchronously for a reply from the rt-process.. 
  if ( (err = RTOS::readFifo(fsms[fsm_id].fifo_in, &dummy, sizeof(dummy))) == sizeof(dummy) ) { 
    /* copy the reply from the shm back to the user-supplied msg buffer.. */
    std::memcpy(&msg, const_cast<struct ShmMsg *>(&shm->msg[fsm_id]), sizeof(msg));
  } else if (err < 0) { 
    throw Exception(std::string("INTERNAL ERROR: Reading of input fifo got an error: ") + strerror(errno));
  } else {
    throw Exception("INTERNAL ERROR: Could not read a complete message from the fifo!");
  }
  // NB: mutex locker destructor unlocks msgFifoLock here..
}

void ConnectionThread::sendToRT(ShmMsgID cmd)
{
  switch (cmd) {
  case RESET_:
  case PAUSEUNPAUSE:
  case INVALIDATE:
  case READYFORTRIAL:
  case FORCETIMESUP:
  case STOPDAQ:
    msg.id = cmd;
    sendToRT(msg);
    break;
  default:
    throw Exception("INTERNAL ERRROR: sendToRT(ShmMsgID) called with an inappropriate command ID!");
    break;
  }
}

void ConnectionThread::getFSMSizeFromRT(unsigned & r, unsigned & c)
{
  msg.id = GETFSMSIZE;
  sendToRT(msg);
  r = msg.u.fsm_size[0];
  c = msg.u.fsm_size[1];
}

unsigned ConnectionThread::getNumInputEventsFromRT(void)
{
  msg.id = GETNUMINPUTEVENTS;
  sendToRT(msg);
  return msg.u.num_input_events;
}

int ConnectionThread::sockSend(const std::string & str) 
{
  return sockSend(str.c_str(), str.length());
}

int ConnectionThread::sockSend(const void *buf, size_t len, bool is_binary, int flags)
{
  const char *charbuf = static_cast<const char *>(buf);
  if (!is_binary) {
    std::stringstream ss;
    ss << "Sending: " << charbuf; 
    if (charbuf[len-1] != '\n') ss << std::endl;
    Log() << ss.str() << std::flush; 
  } else {
    Log() << "Sending binary data of length " << len << std::endl;   
  }
  int ret = ::send(sock, buf, len, flags);
  if (ret < 0) {
    Log() << "ERROR returned from send: " << strerror(errno) << std::endl; 
  } else if (ret != (int)len) {
    Log() << "::send() returned the wrong size; expected " << len << " got " << ret << std::endl; 
  }
  return ret;
}

// Note: trims trailing whitespace!  If string is empty, connection error!
std::string ConnectionThread::sockReceiveLine()
{
#define MAX_LINE (16384) 
  char buf[MAX_LINE];
  int ret, slen = 0;
  std::string rets = "";
  memset(buf, 0, MAX_LINE);
  // keep looping until we fill the buffer, or we get a \n
  // eg: slen < MAXLINE and (if nread then buf[slen-1] must not equal \n)
  while ( slen < MAX_LINE && (!slen || buf[slen-1] != '\n') ) {
    ret = ::recv(sock, buf+slen, MIN((MAX_LINE-slen), 1), 0);
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


int ConnectionThread::sockReceiveData(void *buf, int size, bool is_binary)
{
  int nread = 0;

  while (nread < size) {
    int ret = ::recv(sock, (char *)(buf) + nread, size - nread, 0);
    
    if (ret < 0) {
      Log() << "ERROR returned from recv: " << strerror(errno) << std::endl; 
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

bool ConnectionThread::matrixToRT(const StringMatrix & m,
                                  const std::string & globals,
                                  const std::string & initfunc,
                                  const std::string & cleanupfunc,
                                  const std::string & transitionfunc,
                                  const std::string & tickfunc,
                                  const IntStringMap & entryfmap,
                                  const IntStringMap & exitfmap, 
                                  const IntStringMap & entrycmap,
                                  const IntStringMap & exitcmap,
                                  const std::string & inChanType,
                                  const std::vector<int> & inSpec,
                                  const std::vector<OutputSpec> & outSpec,
                                  const std::vector<SchedWaveSpec> & swSpec,
                                  unsigned readyForTrialJumpState,
                                  bool state0_fsm_swap_flg)
{
  int i,j;

  if (debug) { 
    Log() << "Matrix is:" << std::endl; 
    for (i = 0; i < (int)m.rows(); ++i) {
      TLog log = Log();
      for (j = 0; j < (int)m.cols(); ++j)
        log << m.at(i,j) << " " ;
      log << "\n";       
    }
  }

  const int numFixedCols = 2; // timeout_state and timeout_us
  const unsigned numEvents = inSpec.size();

  // validate FSM matrix size..
  const unsigned requiredCols = inSpec.size() + numFixedCols + outSpec.size();
  if (m.cols() < requiredCols ) {
    Log() << "Matrix has the wrong number of columns: " << m.cols() << " when it actually needs events(" << inSpec.size() << ") + fixed(" << numFixedCols << ") + outputs(" <<  outSpec.size() << ") = " << requiredCols << " columns! Error!" << std::endl; 
    return false;    
  }

  if (outSpec.size() > FSM_MAX_OUT_EVENTS) {
    Log() << "Matrix has too many output columns (" << outSpec.size() << ").  The maximum number of output columns is " << FSM_MAX_OUT_EVENTS << "\n"; 
    return false;
  }

  msg.id = FSM;
  ::memset(&msg.u.fsm, 0, sizeof(msg.u.fsm)); // zero memory since some code assumes unset values are zero?
  // setup matrix here..
  msg.u.fsm.n_rows = m.rows(); // note this will get set to inpRow later in this function via the alias nRows...
  msg.u.fsm.n_cols = m.cols();

  // set some misc. params
  msg.u.fsm.ready_for_trial_jumpstate = readyForTrialJumpState;
  msg.u.fsm.routing.num_evt_cols = inSpec.size();
  msg.u.fsm.routing.num_out_cols = outSpec.size();

  // setup in_chan_type
  if ( (msg.u.fsm.routing.in_chan_type = (inChanType == "ai" ? AI_TYPE : (inChanType == "dio" ? DIO_TYPE : UNKNOWN_TYPE))) == UNKNOWN_TYPE ) {
    Log() << "Matrix specification is using an unknown in_chan_type of " << inChanType << "! Error!" << std::endl; 
    return false;        
  }
  
  // put input spec in the FSM routing..
  {
    // first clear the input routing array, by setting mappings to null (-1)
    for (i = 0; i < FSM_MAX_IN_EVENTS; ++i) msg.u.fsm.routing.input_routing[i] = -1;
    // compute input mapping from input spec vector
    int maxChan = -1, minChan = INT_MAX;
    for (i = 0; i < (int)numEvents && i < (int)m.cols() && i < FSM_MAX_IN_EVENTS; ++i) {
      int chan = inSpec[i];
      if (!chan) continue; // 0 == ignored column, no mapping
      int falling_offset = chan < 0 ? 1 : 0; // if we are falling edge, index +1 into mapping array (see below)
      if (chan < 0) chan = -chan; // make it abs(chan)
      --chan; // remap it to 0-indexed channel
      maxChan = maxChan < chan ? chan : maxChan;
      minChan = minChan > chan ? chan : minChan;
      if (chan >= FSM_MAX_IN_CHANS) {
        Log() << "Matrix specification is using a channel id of " << chan << " which is out of range!  We only support up to " << FSM_MAX_IN_CHANS << " channels! Error!" << std::endl; 
        return false;
      }
      msg.u.fsm.routing.input_routing[chan*2 + falling_offset] = i;
    }
    if (!numEvents) minChan = 0, maxChan = -1;
    msg.u.fsm.routing.num_in_chans = maxChan-minChan+1;
    msg.u.fsm.routing.first_in_chan = minChan;
  }

  // put output spec in the FSM routing..
  msg.u.fsm.routing.num_out_cols = outSpec.size();
  for (unsigned it = 0; it < msg.u.fsm.routing.num_out_cols; ++it) {
    // put output spec into fsm blob..
    memcpy(reinterpret_cast<void *>(&msg.u.fsm.routing.output_routing[it]),
           reinterpret_cast<const void *>(&outSpec[it]),
           sizeof(struct OutputSpec));
  }
  
  // put sched waves in the FSM routing, etc 
  for (unsigned i = 0; i < swSpec.size(); ++i) {
    const SchedWaveSpec & w = swSpec[i];
    if (w.id >= (int)FSM_MAX_SCHED_WAVES || w.id < 0) {
      Log() << "Alarm/Sched Wave specification has invalid id: " << w.id <<"! Error!" << std::endl; 
      return false;
    }
    if (w.in_evt_col >= 0) {
      if (w.in_evt_col >= (int)numEvents) {
        Log() << "Alarm/Sched Wave specification has invalid IN event column routing: " << w.in_evt_col <<"! Error!" << std::endl; 
        return false;
      }
      msg.u.fsm.routing.sched_wave_input[w.id*2] = w.in_evt_col;
    } else {
      msg.u.fsm.routing.sched_wave_input[w.id*2] = -1;
    }
    if (w.out_evt_col >= 0) {
      if (w.out_evt_col >= (int)numEvents) {
        Log() << "Alarm/Sched Wave specification has invalid OUT event column routing: " << w.out_evt_col <<"! Error!" << std::endl; 
        return false;
      }
      msg.u.fsm.routing.sched_wave_input[w.id*2+1] = w.out_evt_col;
    } else {
      msg.u.fsm.routing.sched_wave_input[w.id*2+1] = -1;
    }
    if (w.dio_line >= 0 && w.dio_line >= FSM_MAX_OUT_CHANS) {
      Log() << "Alarm/Sched Wave specification has invalid DIO line: " << w.dio_line <<"! Error!" << std::endl; 
      return false;      
    }
    msg.u.fsm.routing.sched_wave_output[w.id] = w.dio_line;
    // now copy the struct SchedWave to the shm msg
    msg.u.fsm.sched_waves[w.id] = w;
    msg.u.fsm.sched_waves[w.id].enabled = true;
  }


  std::string fsm_prog;
  std::string fsm_shm_name = newShmName();

  // BUILD THE EMBEDDED C PROGRAM TEXT
  {
    std::ostringstream prog;
    prog << "// BEGIN PRE-DEFINED GLOBAL DECLARATIONS AND INCLUDES\n";
    prog << "#define EMBC_MOD_INTERNAL\n";
    prog << "#define EMBC_GENERATED_CODE\n";
    prog << "#include <EmbC.h>\n";
    prog << "\n// BEGIN USER-DEFINED PROGRAM\n";
    
    // prepend global variable/function decls
    prog << globals << "\n";
  
    // build __fsm_get_at function based on the matrix m
    {
   
      for (unsigned r = 0; r < m.rows(); ++r) {
        for (unsigned c = 0; c < m.cols(); ++c) {
          prog << " ulong __" << r << "_" << c << "(void) { return ({ " << m.at(r,c) << "; })";
          // make sure the timeout column is timeout_us and not timeout_s!
          if (c == TIMEOUT_TIME_COL(&msg.u.fsm)) prog << "* 1e6 /* timeout secs to usecs */";
          prog << "; };\n";
        }
      }
      prog << "typedef ulong (*__cell_fn_t)(void);\n"
           << "static __cell_fn_t __states[" << m.rows() << "][" << m.cols() << "] = {\n";
      for (unsigned r = 0; r < m.rows(); ++r) {
        prog << " { ";
        for (unsigned c = 0; c < m.cols(); ++c) {
          prog << "&__" << r << "_" << c;
          if (c+1 < m.cols()) prog << ", ";
        }
        prog << " }"; // end row
        if (r+1 < m.rows()) prog << ",";
        prog << "\n";
      }
      prog << "}; // end func ptr array\n\n";
    
      prog << 
        "unsigned long __embc_fsm_get_at(ushort __r, ushort __c)\n" 
        "{\n" 
        "if (__r < " << m.rows() << " && __c < " << m.cols() << ") return __states[__r][__c]();\n"
        "printf(\"FSM Runtime Error: no such state,column found (%hu, %hu) at time %d.%d!\\n\", __r, __c, (int)time(), (int)((time()-(double)((int)time())) * 10000));\n"
        "return 0;\n"
        "}\n";
    }
  
    // build the __fsm_do_state_entry and __finChanTypesm_do_state_exit functions.. 
    IntStringMap::const_iterator it;
    // __fsm_do_state_entry
    prog << 
      "void __embc_fsm_do_state_entry(ushort __s)\n" 
      "{\n"
      "  switch(__s) {\n";    
    for (unsigned s = 0; s < m.rows(); ++s) {
      prog << "  case " << s << ":\n";
      if ( (it = entryfmap.find(s)) != entryfmap.end() ) 
        prog << "  " << it->second << "();\n"; // call the entry function that was specified    
      if ( (it = entrycmap.find(s)) != entrycmap.end() ) 
        prog << "  {\n" << it->second << "\n}\n"; // embed the entry code that was specified
      prog << "  break;\n";    
    }
    prog << "  default: printf(\"FSM Runtime Error: no such state found (%hu) at time %d.%d!\\n\", __s, (int)time(), (int)((time()-(double)((int)time())) * 10000)); break; \n";
    prog << "  } // end switch\n";
    prog << "} // end function\n\n";
    // __fsm_do_state_exit
    prog << 
      "void __embc_fsm_do_state_exit(ushort __s)\n" 
      "{\n"
      "  switch(__s) {\n";    
    for (unsigned s = 0; s < m.rows(); ++s) {
      prog << "  case " << s << ":\n";
      if ( (it = exitfmap.find(s)) != exitfmap.end() ) 
        prog << "  " << it->second << "();\n"; // call the entry function that was specified    
      if ( (it = exitcmap.find(s)) != exitcmap.end() ) 
        prog << "  {\n" << it->second << "\n}\n"; // embed the entry code that was specified
      prog << "  break;\n";    
    }
    prog << "  default: printf(\"FSM Runtime Error: no such state found (%hu) at time %d.%d!\\n\", __s, (int)time(), (int)((time()-(double)((int)time())) * 10000)); break; \n";
    prog << "  } // end switch\n";
    prog << "} // end function\n\n";
    prog << "// REQUIRED BY INTERFACE TO embc_mod_wrapper.c\n\n";
    prog << "const char *__embc_ShmName = \"" << fsm_shm_name << "\";\n";
    prog << "void (*__embc_init)(void) = ";
    if (initfunc.length())  prog << initfunc << ";\n";
    else prog << "0;\n";
    prog << "void (*__embc_cleanup)(void) = ";
    if (cleanupfunc.length())  prog << cleanupfunc << ";\n";
    else prog << "0;\n";
    prog << "void (*__embc_transition)(void) = ";
    if (transitionfunc.length())  prog << transitionfunc << ";\n";
    else prog << "0;\n";
    prog << "void (*__embc_tick)(void) = ";
    if (tickfunc.length())  prog << tickfunc << ";\n";
    else prog << "0;\n";

    fsm_prog = prog.str();
  }
  
  //std::cerr << "---\n" << fsm_prog << "---\n"; sleep(1);// DEBUG

  // compress the fsm program text, put it in msg.u.fsm.program_z
  unsigned sz = fsm_prog.length()+1, defSz;
  std::auto_ptr<char> defBuf(deflateCpy(fsm_prog.c_str(), sz, &defSz));
  
  if (!defBuf.get()) {
    Log() << "Could not compress the generated FSM program -- an unspecified error occurred from the compression library!\n"; 
    return false;
  }
  if (defSz > FSM_PROGRAM_SIZE) {
    Log() << "The generated, compressed FSM program text is too large! Size is: " << defSz << " whereas size limit is: " << FSM_PROGRAM_SIZE << "!\n"; 
    return false;
  }
  ::memcpy(msg.u.fsm.program_z, defBuf.get(), defSz);
  msg.u.fsm.program_len = sz;
  msg.u.fsm.program_z_len = defSz;
  // compress the FSM raw matrix text as a URLEncoded stringtable, put it in msg.u.fsm.matrix_z
  std::string stringtable = genStringTable(m);  
  sz = stringtable.length()+1;
  freeDHBuf(defBuf.release());
  defBuf.reset(deflateCpy(stringtable.c_str(), sz, &defSz));
  if (!defBuf.get()) {
    Log() << "Could not compress the FSM raw matrix text -- an unspecified error occurred from the compression library!\n"; 
    return false;
  }
  if (defSz > FSM_MATRIX_SIZE) {
    Log() << "The generated, compressed raw matrix text is too large! Size is: " << defSz << " whereas size limit is: " << FSM_MATRIX_SIZE << "!\n"; 
    return false;
  }
  ::memcpy(msg.u.fsm.matrix_z, defBuf.get(), defSz);
  freeDHBuf(defBuf.release());
  msg.u.fsm.matrix_len = sz;
  msg.u.fsm.matrix_z_len = defSz;
  ::strncpy(msg.u.fsm.shm_name, fsm_shm_name.c_str(), sizeof(msg.u.fsm.shm_name));
  msg.u.fsm.shm_name[sizeof(msg.u.fsm.shm_name)-1] = 0;
  msg.u.fsm.wait_for_jump_to_state_0_to_swap_fsm = state0_fsm_swap_flg;
#ifndef EMULATOR
  if (!doCompileLoadProgram(fsm_shm_name, fsm_prog)) return false;
#endif
  sendToRT(msg);
  return true;
}

bool ConnectionThread::doCompileLoadProgram(const std::string & prog_name, const std::string & prog_text) const
{
  bool ret = true;
  if (!compileProgram(prog_name, prog_text)) {
    Log() << "Error compiling the program.\n"; 
    ret = false;
  } else if (!loadModule(prog_name)) {
    Log() << "Error loading the module.\n"; 
    ret = false;
  }
  unlinkModule(prog_name);  
  return ret;
}
                   
bool ConnectionThread::compileProgram(const std::string & fsm_name, const std::string & program_text) const
{
  if ( IsKernel24() ) { // Kernel 2.4 mechanism 
    std::string fname = TmpPath() + fsm_name + ".c", objname = TmpPath() + fsm_name + ".o", modname = TmpPath() + fsm_name;
    std::ofstream outfile(fname.c_str());
    outfile << program_text;
    outfile.close();
    if ( System(CompilerPath() + " -I'" + IncludePath() + "' -c -o '" + objname + "' '" + fname + "'") ) {
      bool status = System(LdPath() + " -r -o '" + modname + "' '" + objname + "' '" + ModWrapperPath() + "'");
      return status;
    }
  } else if ( IsKernel26() ) { // Kernel 2.6 mechanism
    std::string 
        buildDir = TmpPath() + fsm_name + ".build", 
        fname = buildDir + "/" + fsm_name + "_generated.c", 
        objname = TmpPath() + fsm_name + ".ko";
    if (System(std::string("mkdir -p '") + buildDir + "'")) {
      std::ofstream outfile(fname.c_str());
      outfile << program_text;
      outfile.close();
      return System(std::string("cp -f '") + ModWrapperPath() + "' '" + buildDir + "' && cp -f '" + MakefileTemplatePath() + "' '" + buildDir + "/Makefile' && make -C '" + buildDir + "' TARGET='" + fsm_name + "' EXTRA_INCL='" + IncludePath() + "'");
    }
  } else {
    Log() << "ERROR: Unsupported Kernel version in ConnectionThread::compileProgram()!";
  }
  return false;
}

bool ConnectionThread::loadModule(const std::string & program_name) const
{
  std::string modname;
  if ( IsKernel24() ) 
    modname = TmpPath() + program_name;
  else if ( IsKernel26() )
    modname = TmpPath() + program_name + ".build/" + program_name + ".ko";
  else {
    Log() << "ERROR: Unsupported Kernel version in ConnectionThread::loadModule()!";
    return false;
  }
  return System((InsMod() + " '" + modname + "'").c_str());
}

bool ConnectionThread::unlinkModule(const std::string & program_name) const
{
  if ( IsKernel24() ) {
    std::string modname = TmpPath() + program_name, cfile = TmpPath() + program_name + ".c", objfile = TmpPath() + program_name + ".o";
    int ret = 0;
    ret |= ::unlink(modname.c_str());
    ret |= ::unlink(cfile.c_str());
    ret |= ::unlink(objfile.c_str());
    return ret == 0;
  } else if ( IsKernel26() ) {
    std::string buildDir = TmpPath() + program_name + ".build"; 
    System("rm -fr '" + buildDir + "'");
  } else
    Log() << "ERROR: Unsupported Kernel version in ConnectionThread::unlinkModule()!";
  return false;
}

bool ConnectionThread::matrixToRT(const Matrix & m, 
                                  unsigned numEvents, 
                                  unsigned numSchedWaves, 
                                  const std::string & inChanType,
                                  unsigned readyForTrialJumpState,
                                  const std::string & outputSpecStr,
                                  bool state0_fsm_swap_flg)
{
  int i,j;
  // Matrix is XX rows by num_input_evts+4(+1) columns, cols 0-num_input_evts are inputs (cin, cout, lin, lout, rin, rout), 6 is timeout-state,  7 is a 14DIObits mask, 8 is a 7AObits, 9 is timeout-time, and 10 is the optional sched_wave

  if (debug) { 
    Log() << "Matrix is:" << std::endl; 
    for (i = 0; i < (int)m.rows(); ++i) {
      TLog log = Log();
      for (j = 0; j < (int)m.cols(); ++j)
        log << m.at(i,j) << " " ;
      log << "\n"; 
    }
  }
  
  std::vector<OutputSpec> outSpec = parseOutputSpecStr(outputSpecStr);

  const int numFixedCols = 2; // timeout_state and timeout_us

  if (m.rows() == 0 || m.cols() < numFixedCols || m.rows()*m.cols()*sizeof(double) > (int)FSM_MEMORY_BYTES) {
    Log() << "Matrix needs to be at least 1x2 and no larger than " << FSM_MEMORY_BYTES/sizeof(double) << " total elements! Error!" << std::endl; 
    return false;
  }
  const unsigned requiredCols = numEvents + numFixedCols + outSpec.size();
  if (m.cols() < (int)requiredCols ) {
    Log() << "Matrix has the wrong number of columns: " << m.cols() << " when it actually needs events(" << numEvents << ") + fixed(" << numFixedCols << ") + outputs(" <<  outSpec.size() << ") = " << requiredCols << " columns! Error!" << std::endl; 
    return false;    
  }
  if (outSpec.size() > FSM_MAX_OUT_EVENTS) {
    Log() << "Matrix has too many output columns (" << outSpec.size() << ").  The maximum number of output columns is " << FSM_MAX_OUT_EVENTS << "\n"; 
    return false;
  }
  msg.id = FSM;
  ::memset(&msg.u.fsm, 0, sizeof(msg.u.fsm)); // zero memory since some code assumes unset values are zero?
  
  // setup matrix here..
  msg.u.fsm.n_rows = m.rows(); // note this will get set to inpRow later in this function via the alias nRows...
  msg.u.fsm.n_cols = m.cols();
  unsigned short & nRows = msg.u.fsm.n_rows; // alias used for below code, will get decremented once we pop out the sched wave spec that is in our matrix and the input event spec that is in our matrix..

  // seetup in_chan_type
  if ( (msg.u.fsm.routing.in_chan_type = (inChanType == "ai" ? AI_TYPE : (inChanType == "dio" ? DIO_TYPE : UNKNOWN_TYPE))) == UNKNOWN_TYPE ) {
    Log() << "Matrix specification is using an unknown in_chan_type of " << inChanType << "! Error!" << std::endl; 
    return false;        
  }
  
  msg.u.fsm.ready_for_trial_jumpstate = readyForTrialJumpState;
  
  msg.u.fsm.routing.num_evt_cols = numEvents;
  msg.u.fsm.routing.num_out_cols = outSpec.size();
  for (unsigned it = 0; it < msg.u.fsm.routing.num_out_cols; ++it) {
    // put output spec into fsm blob..
    memcpy(reinterpret_cast<void *>(&msg.u.fsm.routing.output_routing[it]),
           reinterpret_cast<void *>(&outSpec[it]),
           sizeof(struct OutputSpec));
  }

  // compute scheduled waves cells used
  int swCells = numSchedWaves * 7; // each sched wave uses 7 cells.
  int swRows = (swCells / m.cols()) + (swCells % m.cols() ? 1 : 0);
  int swFirstRow = (int)nRows - swRows;
  int inpRow = swFirstRow - 1;
  nRows = inpRow;
  if (inpRow < 0 || swFirstRow < 0 || inpRow < 0) {
    Log() << "Matrix specification has invalid number of rows! Error!" << std::endl; 
    return false;            
  }

  // first clear the input routing array, by setting mappings to null (-1)
  for (i = 0; i < FSM_MAX_IN_EVENTS; ++i) msg.u.fsm.routing.input_routing[i] = -1;
  // compute input mapping from input spec vector
  int maxChan = -1, minChan = INT_MAX;
  for (i = 0; i < (int)numEvents && i < (int)m.cols() && i < FSM_MAX_IN_EVENTS; ++i) {
    int chan = static_cast<int>(m.at(inpRow, i));
    if (!chan) continue; // 0 == ignored column, no mapping
    int falling_offset = chan < 0 ? 1 : 0; // if we are falling edge, index +1 into mapping array (see below)
    if (chan < 0) chan = -chan; // make it abs(chan)
    --chan; // remap it to 0-indexed channel
    maxChan = maxChan < chan ? chan : maxChan;
    minChan = minChan > chan ? chan : minChan;
    if (chan >= FSM_MAX_IN_CHANS) {
      Log() << "Matrix specification is using a channel id of " << chan << " which is out of range!  We only support up to " << FSM_MAX_IN_CHANS << " channels! Error!" << std::endl; 
      return false;
    }
    msg.u.fsm.routing.input_routing[chan*2 + falling_offset] = i;
  }
  if (!numEvents) minChan = 0, maxChan = -1;
  msg.u.fsm.routing.num_in_chans = maxChan-minChan+1;
  msg.u.fsm.routing.first_in_chan = minChan;
  
  // rip out the sched wave spec in the matrix, use it to
  // populate fields in FSMBlob::Routing  
  int row = swFirstRow, col = -1;
  for (i = 0; i < (int)numSchedWaves; ++i) {
#define NEXT_COL() do { if (++col >= m.cols()) { ++row; col = 0; } } while (0)
    NEXT_COL();
    int id = (int)m.at(row, col);
    if (id >= (int)FSM_MAX_SCHED_WAVES || id < 0) {
      Log() << "Alarm/Sched Wave specification has invalid id: " << id <<"! Error!" << std::endl; 
      return false;
    }
    SchedWave &w = msg.u.fsm.sched_waves[id];
    NEXT_COL();
    int in_evt_col = (int)m.at(row, col);
    if (in_evt_col >= 0) {
      if (in_evt_col >= (int)numEvents) {
        Log() << "Alarm/Sched Wave specification has invalid IN event column routing: " << in_evt_col <<"! Error!" << std::endl; 
        return false;
      }
      msg.u.fsm.routing.sched_wave_input[id*2] = in_evt_col;
    } else {
      msg.u.fsm.routing.sched_wave_input[id*2] = -1;
    }
    NEXT_COL();
    int out_evt_col = (int)m.at(row, col);
    if (out_evt_col >= 0) {
      if (out_evt_col >= (int)numEvents) {
        Log() << "Alarm/Sched Wave specification has invalid OUT event column routing: " << out_evt_col <<"! Error!" << std::endl; 
        return false;
      }
      msg.u.fsm.routing.sched_wave_input[id*2+1] = out_evt_col;
    } else {
      msg.u.fsm.routing.sched_wave_input[id*2+1] = -1;
    }
    NEXT_COL();
    int dio_line = (int)m.at(row, col);
    if (dio_line >= 0 && dio_line >= FSM_MAX_OUT_CHANS) {
      Log() << "Alarm/Sched Wave specification has invalid DIO line: " << dio_line <<"! Error!" << std::endl; 
      return false;      
    }
    msg.u.fsm.routing.sched_wave_output[id] = dio_line;
    NEXT_COL();
    w.preamble_us = static_cast<unsigned>(m.at(row,col)*1e6);
    NEXT_COL();
    w.sustain_us = static_cast<unsigned>(m.at(row,col)*1e6);
    NEXT_COL();
    w.refraction_us = static_cast<unsigned>(m.at(row,col)*1e6);
    w.enabled = true;
  }
#undef NEXT_COL

  std::ostringstream prog;
  prog << "// BEGIN PRE-DEFINED GLOBAL DECLARATIONS\n";
  prog << "#define EMBC_MOD_INTERNAL\n";
  prog << "#define EMBC_GENERATED_CODE\n";
  prog << "#include <EmbC.h>\n";
  std::string fsm_shm_name = newShmName();
  prog << "const char *__embc_ShmName = \"" << fsm_shm_name << "\";\n";
  prog << "void (*__embc_init)(void) = 0;\n";
  prog << "void (*__embc_cleanup)(void) = 0;\n";
  prog << "void (*__embc_transition)(void) = 0;\n";
  prog << "void (*__embc_tick)(void) = 0;\n";
  prog << "void __embc_fsm_do_state_entry(ushort s) { (void)s; }\n";
  prog << "void __embc_fsm_do_state_exit(ushort s) { (void)s; }\n";
  prog << "\n// BEGIN USER-DEFINED PROGRAM\n";
  // the state matrix as a static array
  prog << "ulong __stateMatrix[" << nRows << "][" << m.cols() << "] = {\n";
  
  for (i = 0; i < (int)nRows; ++i) {
    prog << " { ";
    for (j = 0; j < (int)m.cols(); ++j) {
      double val = m.at(i, j);
      if (j == (int)TIMEOUT_TIME_COL(&msg.u.fsm)) val *= 1e6;  
      prog << std::setw(5) << static_cast<unsigned long>(val);  
      if (j+1 < m.cols()) prog << ", ";
    }
    prog << " }"; if (i+1 < m.rows()) prog << ", ";
    prog << "\n";
  }
  prog << "};\n" 
       << "unsigned long __embc_fsm_get_at(ushort __r, ushort __c)\n"
       << "{\n" 
       << "  if (__r < " << nRows << " && __c < " << m.cols() << ") return __stateMatrix[__r][__c];\n"
       << "  else printf(\"FSM Runtime Error: no such state,column found (%hu, %hu) at time %d.%d!\\n\", __r, __c, (int)time(), (int)((time()-(double)((int)time())) * 10000));\n"
       << "  return 0;\n"
       << "}\n";  
  std::string fsm_prog = prog.str();

  unsigned unc = fsm_prog.length()+1, cmp;
  
  std::auto_ptr<char> tmpBuf(deflateCpy(fsm_prog.c_str(), unc, &cmp));

  if (!tmpBuf.get()) {
    Log() << "Could not compress the FSM program text -- an unspecified error occurred from the compression library!\n"; 
    return false;
  }
  if (cmp > FSM_PROGRAM_SIZE) {
    Log() << "The generated compressed FSM program text is too large! Size is: " << cmp << " whereas size limit is: " << FSM_PROGRAM_SIZE << "!\n"; 
    return false;
  }
  ::memcpy(msg.u.fsm.program_z, tmpBuf.get(), cmp);
  msg.u.fsm.program_z_len = cmp;
  msg.u.fsm.program_len = unc;
  freeDHBuf(tmpBuf.release());
  ::strncpy(msg.u.fsm.shm_name, fsm_shm_name.c_str(), sizeof(msg.u.fsm.shm_name));
  msg.u.fsm.shm_name[sizeof(msg.u.fsm.shm_name)-1] = 0;
  
  std::string stringtable = genStringTable(m);
  unc = stringtable.length()+1;
  tmpBuf.reset(deflateCpy(stringtable.c_str(), unc, &cmp));
  if (!tmpBuf.get()) {
    Log() << "Could not compress the FSM matrix text -- an unspecified error occurred from the compression library!\n"; 
    return false;
  }
  if (cmp > FSM_MATRIX_SIZE) {
    Log() << "The generated compressed FSM matrix text is too large! Size is: " << cmp << " whereas size limit is: " << FSM_MATRIX_SIZE << "!\n"; 
    return false;
  }
  ::memcpy(msg.u.fsm.matrix_z, tmpBuf.get(), cmp);
  msg.u.fsm.matrix_z_len = cmp;
  msg.u.fsm.matrix_len = unc;
  freeDHBuf(tmpBuf.release());

  msg.u.fsm.wait_for_jump_to_state_0_to_swap_fsm = state0_fsm_swap_flg;

#ifndef EMULATOR
  if (!doCompileLoadProgram(fsm_shm_name, fsm_prog)) return false;
#endif
  sendToRT(msg);
  
  return true;
}

StringMatrix ConnectionThread::matrixFromRT()
{

  msg.id = GETVALID;

  sendToRT(msg);

  if (!msg.u.is_valid) {
    // no valid matrix defined
    unsigned rows, cols;
    getFSMSizeFromRT(rows, cols);
    StringMatrix m(rows, cols);
    unsigned int i,j;
    for (i = 0; i < m.rows(); ++i)
      for (j = 0; j < m.cols(); ++j)
        m.at(i,j) = "0";
    Log() << "FSM is invalid, so sending empty matrix for GET STATE MATRIX request." <<
std::endl; 
    return m; 
  }
  
  msg.id = GETFSM;
  sendToRT(msg);
  
  StringMatrix m(msg.u.fsm.n_rows, msg.u.fsm.n_cols);
  std::auto_ptr<char> strBuf(inflateCpy(msg.u.fsm.matrix_z, msg.u.fsm.matrix_z_len, msg.u.fsm.matrix_len, 0));
  
  parseStringTable(strBuf.get(), m);
  freeDHBuf(strBuf.release());

  if (debug) {
    Log() << "Matrix from RT is:" << std::endl;
    for (unsigned i = 0; i < m.rows(); ++i) {
      TLog log = Log();
      for (unsigned j = 0; j < m.cols(); ++j)
        log << m[i][j] << " ";
      log << std::endl; 
    }
  }

   return m;
}

static void *transNotifyThrWrapper(void *arg)
{
  pthread_detach(pthread_self());
  long myfsm = reinterpret_cast<long>(arg);
  if (myfsm < 0 || myfsm >= NUM_STATE_MACHINES) return 0;
  return fsms[myfsm].transNotifyThrFun();
}

static void *daqThrWrapper(void *arg)
{
  pthread_detach(pthread_self());
  long myfsm = reinterpret_cast<long>(arg);
  if (myfsm < 0 || myfsm >= NUM_STATE_MACHINES) return 0;
  return fsms[myfsm].daqThrFun();
}

static void *nrtThrWrapper(void *arg)
{
  pthread_detach(pthread_self());
  long myfsm = reinterpret_cast<long>(arg);
  if (myfsm < 0 || myfsm >= NUM_STATE_MACHINES) return 0;
  return fsms[myfsm].nrtThrFun();
}

void *FSMSpecific::transNotifyThrFun()
{
  static const unsigned n_buf = FIFO_TRANS_SZ/sizeof(struct StateTransition), bufsz = FIFO_TRANS_SZ;
  struct StateTransition *buf = new StateTransition[n_buf];
  int nread = 0;

  while(nread >= 0 && fifo_trans != RTOS::INVALID_FIFO) {
    nread = RTOS::readFifo(fifo_trans, buf, bufsz);
    if (nread > 0) {
      unsigned num = nread / sizeof(*buf), i;
      pthread_mutex_lock(&transNotifyLock);
      for (i = 0; i < num; ++i) {
        transBuf.push(buf[i]);
        // DEBUG...
        //::log() << "Got transition: " << 
        //    buf[i].previous_state << " " << buf[i].state << " " << buf[i].event_id << " " << buf[i].ts/1000000000.0 << std::endl; ::
      }
      pthread_cond_broadcast(&transNotifyCond);
      pthread_mutex_unlock(&transNotifyLock);
    }
  }
  delete [] buf;
  return 0;
}

void ConnectionThread::doNotifyEvents(bool verbose)
{
  unsigned long long lastct = 0, ct;
  bool err = false;
  FSMSpecific & f = fsms[fsm_id];
  pthread_mutex_lock(&f.transNotifyLock);
  lastct = f.transBuf.count();
  pthread_mutex_unlock(&f.transNotifyLock);
  while (sock > -1 && !err) {
    int nready;
    ::ioctl(sock, FIONREAD, &nready);
    if (nready > 0) {
      // socket got data! abort it all since this is outside of protocol spec!
      char buf[nready > 1024 ? 1024 : nready];
      sockReceiveData(buf, nready, true); // consume data..
      sockSend("BYE\n");
      return;
    }
    pthread_mutex_lock(&f.transNotifyLock);
    while ((ct = f.transBuf.count()) == lastct && sock > -1) {
      struct timeval now;
      struct timespec timeout;
      ::gettimeofday(&now, NULL);
      // wait 1 second on the condition
      timeout.tv_sec = now.tv_sec + 1;
      timeout.tv_nsec = now.tv_usec * 1000;
      int ret = pthread_cond_timedwait(&f.transNotifyCond, &f.transNotifyLock, &timeout);
      if (ret == ETIMEDOUT) continue;
    }
    // handle overflow condition..
    if (ct - lastct > f.transBuf.size())  lastct = ct - f.transBuf.size();
    CircBuf<StateTransition> tb(f.transBuf); // copy the tranition events locally
    pthread_mutex_unlock(&f.transNotifyLock);  // release the lock
    if (verbose) { // process each event in verbose mode
      // operate on local transition events
      std::stringstream ss;
      for (unsigned long long i = lastct; i < ct; i++) {
        struct StateTransition & t = tb[i];
        ss << t.previous_state << " " << t.state << " " << int(t.event_id) << " " << std::setprecision(12) << std::setw(12) << (static_cast<double>(t.ts)/(double)1000000000.0) << " " << (static_cast<double>(t.ext_ts)/(double)1000000000.0) << std::endl;
      }
      err = sockSend(ss.str()) < 0;
    } else {
      // do a bulk notify that an event occurred in  no-verbose mode
      err = sockSend("e") < 0;
    }
    lastct = ct;
  }
}

void *FSMSpecific::daqThrFun()
{
  std::vector<char> buf(FIFO_DAQ_SZ);
  struct DAQScan *sc;
  int nread = 0;
  
  while(nread >= 0 && fifo_daq != RTOS::INVALID_FIFO) {
    nread = RTOS::readFifo(fifo_daq, &buf[0], FIFO_DAQ_SZ);
    int nproc = 0;
    sc = reinterpret_cast<DAQScan *>(&buf[nproc]);
    while (nread > 0 && nread-nproc >= int(sizeof(*sc)) && sc->magic == DAQSCAN_MAGIC) {
      pthread_mutex_lock(&daqLock);
      DAQScanVec & vec = daqBuf.next();
      vec.assign(sc, daqMaxData, daqRangeMin, daqRangeMax);
      daqBuf.push();
      nproc += sizeof(DAQScan) + sizeof(sc->samps[0])*sc->nsamps;
      sc = reinterpret_cast<DAQScan *>(&buf[nproc]);
      pthread_mutex_unlock(&daqLock);
    }
  }
  return 0;
}

void *FSMSpecific::nrtThrFun()
{
  std::auto_ptr<struct NRTOutput> nrt(new NRTOutput);
  int nread = 0;
  
  while(nread >= 0 && fifo_nrt_output != RTOS::INVALID_FIFO) {
    nread = RTOS::readFifo(fifo_nrt_output, nrt.get(), sizeof(*nrt));
    if (nread == sizeof(*nrt) && nrt->magic == NRTOUTPUT_MAGIC) {
      switch (nrt->type) {
      case NRT_TCP: 
        doNRT_IP(nrt.get(), false);
        break;
      case NRT_UDP: 
        doNRT_IP(nrt.get(), true);
        break;
      default:
        Log() << "ERROR In nrtThrFun() got unknown NRT output type " 
               << int(nrt->type) << "\n"; 
        break;
      }
    } else if (fifo_nrt_output != RTOS::INVALID_FIFO) { // this can happen on program shutdown when the fifo is closed from under us
        Log() << "ERROR In nrtThrFun() read invalid struct NRTOutput from fifo!\n"; 
    }
  }
  return 0;
}

static
std::vector<double> splitNumericString(const std::string &str,
                                       const std::string &delims,
                                       bool allowEmpties)
{
  std::vector<double> ret;
  if (!str.length() || !delims.length()) return ret;
  std::string::size_type pos;
  for ( pos = 0; pos < str.length() && pos != std::string::npos; pos = str.find_first_of(delims, pos) ) {
      if (pos) ++pos;
      bool ok;
      double d = FromString<double>(str.substr(pos), &ok);
      if (!ok)
          // test for zero-length token
          /* break if parse error -- but conditionally allow empty tokens.. */
          if (allowEmpties && ( delims.find(str[pos]) != std::string::npos || pos >= str.length() ) ) {
              if (!pos) ++pos;
          }      
          else
              break; 
      else
          ret.push_back(d);
  }
  return ret;
}

Matrix FSMSpecific::getDAQScans()
{
  pthread_mutex_lock(&daqLock);
  Matrix mat(daqBuf.countNormalized(), daqNumChans+1);
  for (unsigned i = 0; i < daqBuf.countNormalized(); ++i) {
    mat.at(i, 0) = daqBuf[i].ts_nanos / 1e9;
    for (unsigned j = 0; j < daqNumChans; ++j)
      if (j < daqBuf[i].samples.size())
        mat.at(i, j+1) = daqBuf[i].samples[j];
      else 
        mat.at(i, j+1) = 0.;
  }
  daqBuf.clear();
  pthread_mutex_unlock(&daqLock);
  return mat;
}

void FSMSpecific::doNRT_IP(const NRTOutput *nrt, bool isUDP) const
{  
        struct hostent he, *he_result;
        int h_err;
        char hostEntAux[32768];
        std::string packetText = FormatPacketText(nrt->ip_packet_fmt, nrt);
        int ret = ::gethostbyname2_r(nrt->ip_host, AF_INET, &he, hostEntAux, sizeof(hostEntAux),&he_result, &h_err);

#ifdef EMULATOR
          Log() << "Sending to " << nrt->ip_host << ":" << nrt->ip_port << " data: '" << packetText << "'\n";
#endif

        if (ret) {
          Log() << "ERROR In doNRT_IP() got error (ret=" << ret << ") in hostname lookup for " << nrt->ip_host << ": h_errno=" << h_err << "\n"; 
          return;
        }
        int theSock;
        if (isUDP) theSock = socket(PF_INET, SOCK_DGRAM, 0);
        else theSock = socket(PF_INET, SOCK_STREAM, 0);
        if (theSock < 0) {
          Log() << "ERROR In doNRT_IP() got error (errno=" << strerror(errno) << ") in socket() call\n"; 
          return; 
        }
        if (!isUDP) {
          long flag = 1;
          setsockopt(theSock, SOL_TCP, SO_REUSEADDR, &flag, sizeof(flag));
          flag = 1;
          // turn off nagle for less latency
          setsockopt(theSock, SOL_TCP, TCP_NODELAY, &flag, sizeof(flag));
        }
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(nrt->ip_port);
        memcpy(&addr.sin_addr.s_addr, he.h_addr, sizeof(addr.sin_addr.s_addr));
        ret = connect(theSock, (struct sockaddr *)&addr, sizeof(addr)); // this call is ok for UDP as well
        if (ret) {
          Log() << "ERROR In doNRT_IP() could not connect to " << nrt->ip_host << ":" << nrt->ip_port << " got error (errno=" << strerror(errno) << ") in connect() call\n"; 
          ::close(theSock);
          return;
        }
        int datalen = packetText.length();
        if (isUDP)
          // UDP
          ret = sendto(theSock, packetText.c_str(), datalen, 0, (const struct sockaddr *)&addr, sizeof(addr));
        else
          // TCP
          ret = send(theSock, packetText.c_str(), datalen, MSG_NOSIGNAL);
        if (ret != datalen) {
          Log() << "ERROR In doNRT_IP() sending " << datalen << " bytes to " << nrt->ip_host << ":" << nrt->ip_port << " got error (errno=" << strerror(errno) << ") in send() call\n"; 
        }
        ::close(theSock);
}

namespace {

  std::string UrlEncode(const std::string & str)
  {
	std::string ret = "";
    const int len = str.length();
	for (int i = 0; i < len; ++i) {
      char c = str[i];
      if ( ((c >= 65) && (c <= 90)) || ((c <= 57) && (c >= 48)) || ((c <= 122) && (c >= 97)) ) {
        ret +=  c;
      } else {
        char buf[64];
        sprintf(buf, "%%%02X", unsigned(c));
        ret += buf;
      }
	}
	return ret;
  }

  std::string UrlDecode(const std::string & str)
  {
    std::string ret = "";
    const int len = str.length();
	for (int i = 0; i < len; ++i) {      
      if (str[i] == '%' && i+2 < len && isxdigit(str[i+1]) && isxdigit(str[i+2]) ) {
        ret += char('\0' + strtol(str.substr(i+1, 2).c_str(), 0, 16)); // parse the hex number 
        i+=2; // consume %XX
      } else {
        ret += str[i]; 
      }
    }
	return ret;
  }

  std::string FormatPacketText(const std::string & str, const NRTOutput *nrt)
  {
    std::ostringstream out;
    const int len = str.length();
    for (int i = 0; i < len; ++i) {
      if (str[i] == '%') { // check for '%' format specifier
        if (i+1 < len 
            && (str[i+1] == '%' || str[i+1] == 'v' || str[i+1] == 't' ||
                str[i+1] == 'T' || str[i+1] == 's' || str[i+1] == 'c')) {    
          switch(str[i+1]) {          
          case '%':  out << '%';  break;
          case 'v':  out << nrt->trig; break;
          case 't':  out << double(nrt->ts_nanos)/1e9; break;
          case 'T':  out << nrt->ts_nanos; break;
          case 's':  out << nrt->state; break;
          case 'c':  out << unsigned(nrt->col); break;
          }
        } // else do nothing, has the effect of consuming %anychar
        ++i; // need to consume %anychar regardless
      } else { // not %, so just copy the data
        out << str[i];
      }
    }
    return out.str();
  }

};

std::vector<OutputSpec> ConnectionThread::parseOutputSpecStr(const std::string & str)
{
  // format of output string is \1TYPE_STR\2DATA_STR...

  std::string::size_type pos = 0, pos2 = 0;
  std::vector<OutputSpec> ret;
  while ( (pos = str.find('\1', pos)) != std::string::npos
          && (pos2 = str.find('\2', pos)) != std::string::npos
          && pos2 > pos ) {
    ++pos; // consume \1
    std::string type = str.substr(pos, pos2-pos);
    ++pos2; // consume \2
    std::string data = str.substr(pos2, str.find('\1', pos2)-pos2);
    std::string typeData = type + "::" + data;
    struct OutputSpec spec;
    if (type == "dout") {
      spec.type = OSPEC_DOUT;
    } else if (type == "trig") {
      spec.type = OSPEC_TRIG;
    } else if (type == "ext" || type == "sound") {
      spec.type = OSPEC_EXT;
    } else if (type == "sched_wave") {
      spec.type = OSPEC_SCHED_WAVE;
    } else if (type == "tcp") {
      spec.type = OSPEC_TCP;
    } else if (type == "udp") {
      spec.type = OSPEC_UDP;
    } else if (type == "noop") {
      spec.type = OSPEC_NOOP;
    } else {
      Log() << "Parse error for output spec \"" << typeData << "\" (ignoring matrix column! Argh!)\n"; 
      spec.type = OSPEC_NOOP;
    }
    // spec.data
    if (type == "dout" || type == "trig") {
      // parse data range
      if (sscanf(data.c_str(), "%u-%u", &spec.from, &spec.to) != 2) {
        Log() << "Could not parse channel range from output spec \"" << typeData << "\" assuming 0-1! Argh!\n"; 
        spec.from = 0;
        spec.to = 1;
      }
    } else if (type == "ext" || type == "sound") {
      // parse data range
      if (sscanf(data.c_str(), "%u", &spec.object_num) != 1) {
        Log() << "Could not parse object id from output spec \"" << typeData << "\" assuming same as fsm id " << fsm_id << "!\n"; 
        spec.object_num = fsm_id;
      }
    } else if (type == "udp" || type == "tcp") {
      // parse host:port:
      memset(spec.data, 0, sizeof(spec.data));
      if (sscanf(data.c_str(), "%79[0-9a-zA-Z.]:%hu:%942c", spec.host, &spec.port, spec.fmt_text) != 3) {
        Log() << "Could not parse host:port:packet from output spec \"" << typeData << "\"!  Suppressing column! Argh!\n"; 
        spec.type = OSPEC_NOOP;
      }
    } else {
      strncpy(spec.data, data.c_str(), OUTPUT_SPEC_DATA_SIZE);
      spec.data[OUTPUT_SPEC_DATA_SIZE-1] = 0;
    }
    ret.push_back(spec);
  }
  return ret;
}

template <class T> struct BackInsertFunctor
{
  BackInsertFunctor(T & t) : t(t) {}
  template <class I> void operator()(I & i) const { t.push_back(static_cast<typename T::value_type>(i)); }
  mutable T & t;
};

template <class T> BackInsertFunctor<T> BackInsert(T & t) { return BackInsertFunctor<T>(t); }

std::vector<int>  
ConnectionThread::parseInputSpecStr(const std::string & str)
{
  // format of input spec string is ID1,ID2,ID3...  so just split on commas
  std::vector<int> ret;
  std::vector<double> numbers = splitNumericString(str, ",");
  std::for_each(numbers.begin(), numbers.end(), BackInsert(ret));
  return ret;
}

std::vector<SchedWaveSpec> 
ConnectionThread::parseSchedWaveSpecStr(const std::string & str)
{

  // format of sched wave string is \1ID\2IN_EVT_COL\2OUT_EVT_COL\2DIO_LINE\2PREAMBLE_SECS\2SUSTAIN_SECS\2REFRACTION_SECS\2... so split on \1 and within that, on \2

  std::vector<SchedWaveSpec> ret;
  std::set<int> ids_seen;
  // first split on \1
  std::vector<std::string> waveSpecStrs = splitString(str, "\1"), flds;
  std::vector<std::string>::const_iterator it;
  for (it = waveSpecStrs.begin(); it != waveSpecStrs.end(); ++it) {
    flds = splitString(*it, "\2");
    if (flds.size() != 7) {
      Log() << "Parse error for sched waves spec \"" << *it << "\" (ignoring! Argh!)\n"; 
      continue;      
    }
    struct SchedWaveSpec spec;
    bool ok;
    int i = 0;
    spec.id = FromString<int>(flds[i++], &ok);
    if (ok) spec.in_evt_col = FromString<int>(flds[i++], &ok);
    if (ok) spec.out_evt_col = FromString<int>(flds[i++], &ok);
    if (ok) spec.dio_line = FromString<int>(flds[i++], &ok);
    if (ok) spec.preamble_us = static_cast<unsigned>(FromString<double>(flds[i++], &ok) * 1e6);
    if (ok) spec.sustain_us = static_cast<unsigned>(FromString<double>(flds[i++], &ok) * 1e6);
    if (ok) spec.refraction_us = static_cast<unsigned>(FromString<double>(flds[i++], &ok) * 1e6);
    if (!ok) {
      int num = i--;
      Log() << "Parse error for sched waves str: \"" << *it << "\" field #" << num << "\"" << flds[i] << "\" (ignoring! Argh!)\n"; 
      continue;
    }
    if (ids_seen.find(spec.id) != ids_seen.end()) {
      Log() << "Error for sched waves spec -- duplicate id " << spec.id << " encountered.  Ignoring dupe! Argh!\n"; 
      continue;
    }
    ids_seen.insert(spec.id);
    ret.push_back(spec);
  }
  return ret;
}

IntStringMap ConnectionThread::parseIntStringMapBlock(const std::string & strblk)
{
  // format is state_num -> sometext, one per line
  IntStringMap ret;
  // split lines on newline..
  std::vector<std::string> nv_pairs = splitString(strblk, "\n");
  int ct = 1;
  for(std::vector<std::string>::const_iterator it = nv_pairs.begin(); it != nv_pairs.end(); ++it, ++ct) {
    // split strings on '->'
    std::vector<std::string> nvp = splitString(*it, "->");
    bool ok;
    int state_num;
    if (nvp.size() != 2 || (state_num = FromString<int>(nvp[0], &ok)) < 0 || !ok) {
      Log() << "Error parsing state func map line number " << ct << ".  Line should be of the form: state_num -> some_text!\n"; 
      continue;
    }
    ret[state_num] = UrlDecode(nvp[1]);
  }
  return ret;
}

bool ConnectionThread::sendStringMatrix(const StringMatrix &m)
{
    std::ostringstream os;
    os << "URLENC STRINGTABLE " << m.rows() << " " << m.cols() << std::endl; 
    sockSend(os.str());
    
    std::string line = sockReceiveLine(); // wait for "READY" from client
    
    if (line.find("READY") != std::string::npos) {            
        std::string s(genStringTable(m));
        sockSend(s);
        return true;
    }
    return false;
}

static const char *ffwdSpaces(const char *p)
{
  while (*p && isspace(*p)) ++p;
  return p;
}

static const char *nextWhiteSpaceBlock(const char *p)
{
  // try and leave the current whitespace block, if we are in a 
  // whitespace block
  p = ffwdSpaces(p);
  // next, try and find the next whitespace block
  while (*p && *++p) 
    if (isspace(*p)) break;  
  return p;
}


// static
void ConnectionThread::parseStringTable(const char *stable, StringMatrix &m) 
{
  unsigned r,c;
  const char *pos = ffwdSpaces(stable), *npos = nextWhiteSpaceBlock(pos);
  for (r = 0; *pos && r < m.rows(); ++r) {
    for (c = 0; *pos && c < m.cols(); ++c) { 
      m[r][c] = UrlDecode(std::string(pos, npos-pos));
      pos = npos;
      npos = nextWhiteSpaceBlock(pos);
    }
  }
}

// static
std::string ConnectionThread::genStringTable(const StringMatrix &m) 
{
        std::ostringstream o;
        for (unsigned i = 0; i < m.rows(); ++i) {
            for (unsigned j = 0; j < m.cols(); ++j) {
                 if (j) o << " ";
                 o << UrlEncode(m.at(i,j));
            }
            o << "\n";
        }
        return o.str();
}

// static
std::string ConnectionThread::genStringTable(const Matrix &m) 
{
        std::ostringstream o;
        for (int i = 0; i < m.rows(); ++i) {
            for (int j = 0; j < m.cols(); ++j) {
                 if (j) o << " ";
                 o << m.at(i,j);
            }
            o << "\n";
        }
        return o.str();
}

static std::string trimWS(const std::string & s)
{
  std::string::const_iterator pos1, pos2;
  pos1 = std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun(::isspace)));
  pos2 = std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun(::isspace))).base();
  return std::string(pos1, pos2);
}

bool ConnectionThread::doSetStateProgram()
{
      /* FSM *program* upload 
         Format is:
         SET STATE PROGRAM
         BEGIN META
         GLOBALS
           URLEncoded Block of Globals Section code
         INITFUNC
           urlencoded name of init func
         CLEANUPFUNC
           urlencoded name of cleanup func
         TRANSITIONFUNC
           urlencoded name of transition func
         TICKFUNC
           urlencoded name of tick func
         ENTRYFUNCS
           state_number -> urlencoded func name
           etc ...
         EXITFUNCS
           state_number -> urlencoded func name
           etc ...
         ENTRYCODES
           state_number -> urlencoded code block
           etc ...
         EXITCODES
           state_number -> urlencoded code block
           etc ...
         IN CHAN TYPE 
           ai|dio
         INPUT SPEC STRING
           input spec string (id1, id2, id3, etc..)
         OUTPUT SPEC STRING
           URLEncoded output spec string
         SCHED WAVES
           URLEncoded sched wave spec
         READY FOR TRIAL JUMPSTATE
           number
//         SWAP FSM ON STATE 0 ONLY
//           boolean_number
         END META
         num_rows num_cols
           UrlEncoeded matrix cells, one per line
      */

  // NB, at this point "SET STATE PROGRAM" is already consumed.
  std::string line, section = "", block = "";
  std::string globals, initfunc, cleanupfunc, transitionfunc, tickfunc, inChanType;
  IntStringMap entryfuncs, exitfuncs, entrycodes, exitcodes;
  std::vector<int> inSpec;
  std::vector<OutputSpec> outSpec;
  std::vector<SchedWaveSpec> swSpec;
  unsigned readyForTrialJumpState = 0, state0FSMSwapFlg = 0;
  while ( section.find("END META") != 0 ) {
    line = sockReceiveLine();
    if (!line.length()) continue;
    if (::isspace(line[0])) {
      // in a section
      block += trimWS(line);
    } else {
      // start of a new section header, end of previous section
      
      // first, close out the previous section
      if (section == "BEGIN META" || section == "") {
        // nothing
      }
      else if (section == "GLOBALS") {
        globals = UrlDecode(block);
      }
      else if (section == "INITFUNC") {
        initfunc = UrlDecode(block);
      }
      else if (section == "CLEANUPFUNC") {
        cleanupfunc = UrlDecode(block);
      }
      else if (section == "TRANSITIONFUNC") {
        transitionfunc = UrlDecode(block);
      }
      else if (section == "TICKFUNC") {
        tickfunc = UrlDecode(block);
      }
      else if (section == "IN CHAN TYPE") {
        inChanType = block;
      }
      else if (section == "INPUT SPEC STRING") {
        inSpec = parseInputSpecStr(UrlDecode(block)); // for now, *IS* url encoded!
      }
      else if (section == "OUTPUT SPEC STRING") {
        outSpec = parseOutputSpecStr(UrlDecode(block));
      }
      else if (section == "SCHED WAVES") {
        swSpec = parseSchedWaveSpecStr(UrlDecode(block));
      }
      else if (section == "READY FOR TRIAL JUMPSTATE") {
        readyForTrialJumpState = FromString<unsigned>(block);
      } 
      else if (section == "SWAP FSM ON STATE 0 ONLY") {
        state0FSMSwapFlg = FromString<unsigned>(block);
      }
      else if (section == "ENTRYFUNCS") {
        entryfuncs = parseIntStringMapBlock(block);
      } 
      else if (section == "EXITFUNCS") {
        exitfuncs = parseIntStringMapBlock(block); 
      } 
      else if (section == "ENTRYCODES") {
        entrycodes = parseIntStringMapBlock(block); 
      } 
      else if (section == "EXITCODES") {
        exitcodes = parseIntStringMapBlock(block); 
      }
      else {
        Log() << "Unknown section `" << section << "' encountered, ignoring!" << std::endl; 
      }

      // next.. just indicate start of new section
      section = trimWS(line);
      std::transform(section.begin(), section.end(), section.begin(), toupper); // convert to ucase
      block = "";
    }
  }
  unsigned rows = 0, cols = 0;  
  // next, do the stringtable
  line = sockReceiveLine();
  std::istringstream ss(line);
  ss >> rows >> cols;
  unsigned ct;
  for (ct = 0, block = "";  ct < rows*cols; ++ct) block += sockReceiveLine();
  StringMatrix m(rows, cols);
  parseStringTable(block.c_str(), m);
  return matrixToRT(m, globals, initfunc, cleanupfunc, transitionfunc, tickfunc, entryfuncs, exitfuncs, entrycodes, exitcodes, inChanType, inSpec, outSpec, swSpec, readyForTrialJumpState, state0FSMSwapFlg);  
}

static std::vector<std::string> splitString(const std::string &str,
                                            const std::string &delim,
                                            bool trimws,
                                            bool skip_empties)
{
  std::vector<std::string> ret;
  if (!str.length() || !delim.length()) return ret;
  std::string::size_type pos, pos2;
  for ( pos = 0, pos2 = 0; pos < str.length() && pos != std::string::npos; pos = pos2 ) {
    if (pos) pos = pos + delim.length();
    if (pos > str.length()) break;
    pos2 = str.find(delim, pos);
    if (pos2 == std::string::npos) pos2 = str.length();
    std::string s = str.substr(pos, pos2-pos);
    if (trimws) s = trimWS(s);
    if (skip_empties && !s.length()) continue;
    ret.push_back(s);
  }
  return ret;  
}


std::string ConnectionThread::newShmName()
{ 
  std::ostringstream o;
  int fsmCtr;
  {
    MutexLocker ml(connectedThreadsLock);
    fsmCtr = ++shm->fsmCtr;
  }
  o << "fsm" << std::setw(3) << std::setfill('0') << fsmCtr << "f" << fsm_id << "t" << myid << "_c" << shm_num++; 
  return o.str();
}

namespace {
  bool FileExists(const std::string & f)
  {
    struct stat st;
    return ::stat(f.c_str(), &st) == 0;
  }
  const std::string & KernelVersion() 
  {    
    static std::string ret;
#ifndef EMULATOR    
    if (ret.empty()) {
      // kind of an inelegant HACK -- use but we are lazy
      static const int buflen = 64;
      char tmp_filename[buflen];
      std::strncpy(tmp_filename, (TmpPath() + "XXXXXX").c_str(), buflen);
      tmp_filename[buflen-1] = 0;
      int fd = ::mkstemp(tmp_filename);
      std::system((std::string("uname -r > ") + tmp_filename).c_str());
      std::ifstream inp;
      inp.open(tmp_filename);
      inp >> ret;
      inp.close();
      ::close(fd);
      ::unlink(tmp_filename); // we unlink after close due to wind0ze?
    }
#endif
    return ret;
  }
};

bool ConnectionThread::System(const std::string &cmd) const
{
  Log() << "Executing: \"" << cmd << "\"\n"; 
  return std::system(cmd.c_str()) == 0;
}

Matrix ConnectionThread::doGetTransitionsFromRT(int & first, int & last, int & state)
{
  static const Matrix empty (0, 5);

  // figure out the current state
  msg.id = GETCURRENTSTATE;
  sendToRT(msg);
  state = msg.u.current_state;

  // query the count first to check sanity
  msg.id = TRANSITIONCOUNT;
  sendToRT(msg);

  int n_trans = msg.u.transition_count;

  if (first < 0) first = 0;
  if (last < 0) last = n_trans-1;

  if (first > -1 && first <= last && last < n_trans) {
      unsigned num_input_events = getNumInputEventsFromRT();
      int desired = last-first+1, received = 0, ct = 0;
      Matrix mat(desired, 5);


      // keep 'downloading' the matrix from RT until we get all the transitions we require
      while (received < desired) {
            msg.id = TRANSITIONS;
            msg.u.transitions.num = desired - received;
            msg.u.transitions.from = first + received;
            sendToRT(msg);
            received += (int)msg.u.transitions.num;
            for (int i = 0; i < (int)msg.u.transitions.num; ++i, ++ct) {
              struct StateTransition & t = msg.u.transitions.transitions[i];
              mat.at(ct, 0) = t.previous_state;
              mat.at(ct, 1) = t.event_id > -1 ? 0x1 << t.event_id : 0x1<<num_input_events;
              mat.at(ct, 2) = static_cast<double>(t.ts/1000ULL) / 1000000.0; /* convert us to seconds */
              mat.at(ct, 3) = t.state;
              mat.at(ct, 4) = static_cast<double>(t.ext_ts/1000ULL) / 1000000.0;
            }
      }
      return mat;
  }
  return empty;
}

#if defined(OS_CYGWIN) || defined(OS_OSX)
namespace {
  /* Cygwin (and OSX?) lacks this function, so we will emulate it using 
     a static mutex and the non-reentrant gethostbyname */
  int gethostbyname2_r(const char *name, int af,
                       struct hostent *ret, char *buf, size_t buflen,
                       struct hostent **result, int *h_errnop)
  {
    static Mutex m;
    MutexLocker mlocker(m);
    struct hostent *he = gethostbyname(name);
    if (h_errnop) *h_errnop = h_errno;
    if (he && he->h_addrtype == af && ret && buf) {
      size_t bufused = 0, bufneeded;
      int n_ptrs = 0;
      while (he->h_addr_list[n_ptrs++]) // count number of pointers to addresses, INCLUDING the NULL pointer at the end..
        ;
      int namelen = strlen(he->h_name);
      bufneeded = namelen+1 + n_ptrs * sizeof(char *) + (n_ptrs-1)*he->h_length;
      if (buflen < bufneeded) {
        // not enough room in buffer for everything
        if (h_errnop) *h_errnop = ERANGE;
        return 1; 
      }
      strncpy(buf, he->h_name, namelen);
      buf[namelen] = 0;
      bufused += namelen+1;
      ret->h_name = buf+bufused;
      ret->h_aliases = NULL;
      ret->h_addrtype = he->h_addrtype;
      ret->h_length = he->h_length;
      ret->h_addr_list = reinterpret_cast<char **>(buf + bufused);
      char *buf_addr_area = reinterpret_cast<char *>(ret->h_addr_list) + n_ptrs * sizeof(char *);
      for (int i = 0; i < n_ptrs-1; ++i) {
        ret->h_addr_list[i] = buf_addr_area + i * ret->h_length;
        memcpy(ret->h_addr_list[i], he->h_addr_list[i], ret->h_length);
      }
      ret->h_addr_list[n_ptrs-1] = 0;
      if (result) *result = ret;
    } else {
      if (he && h_errnop) *h_errnop = EINVAL;
      return 1;
    }    
    return 0;
  }
}
#endif
