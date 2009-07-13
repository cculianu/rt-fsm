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
#  define closesocket(x) close(x)
#  define GetLastNetErrStr() (strerror(errno))
#endif
#include <unistd.h>

#include <regex.h>

#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
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

#include "FSM.h"
#include "rtos_utility.h"
#include "deflate_helper.h"
#if defined(OS_LINUX) || defined(OS_OSX)
#include "scanproc.h"
#endif
#include "Version.h"
#include "Mutex.h"
#include "Util.h"


#if defined(OS_CYGWIN) || defined(OS_OSX) || defined(OS_WINDOWS)
namespace {
  /* Cygwin (and OSX?) lacks this function, so we will emulate it using 
     a static mutex */
  int gethostbyname2_r(const char *name, int af,
                       struct hostent *ret, char *buf, size_t buflen,
                       struct hostent **result, int *h_errnop);
}
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

struct Matrix
{
public:
  Matrix(const Matrix & mat) : d(0) { *this = mat; }
  Matrix(int m, int n) : m(m), n(n) { d = new double[m*n ? m*n : 1]; }
  Matrix() : m(0), n(0) { d = new double[1]; }
  ~Matrix() { delete [] d; d = 0; }
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
  d = new double[m*n ? m*n : 0];
  if (rhs.d && m*n) std::memcpy(d, rhs.d, m*n*sizeof(double));
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
  const unsigned MinimumClientVersion = 220080319;
  typedef std::map<ConnectionThread *, ConnectionThread *> ConnectedThreadsList;

  bool debug = false; // set with -d command-line switch to enable debug mode.
  std::string compiler = ""; // set with -c command-line switch to specify an alternate compiler
  bool fast_embc_builds = true; // set by default, unset with the -e command-line switch which makes it more compatible

  unsigned n_threads;
  ConnectedThreadsList connectedThreads;
  pthread_mutex_t connectedThreadsLock = PTHREAD_MUTEX_INITIALIZER;

  std::string UrlEncode(const std::string &);
  std::string UrlDecode(const std::string &);
  std::string FormatPacketText(const std::string &, const NRTOutput *);


  const std::string & KernelVersion();
  bool IsKernel24() { return KernelVersion().substr(0, 4) == "2.4."; }
  bool IsKernel26() { return KernelVersion().substr(0, 4) == "2.6."; }
  bool IsFastEmbCBuilds() { return fast_embc_builds; }

  std::string CompilerPath() { return compiler.length() ? compiler : (IsKernel24() ? "tcc/tcc" : ""); }
  std::string IncludePath() { char buf[128]; return std::string(getcwd(buf, 128)) + "/include"; }
  std::string EmbCHPath() { return IncludePath() + "/EmbC.h"; }
  std::string EmbCBuildPath() { return (IsKernel26() && IsFastEmbCBuilds()) ? "runtime/EmbC.build" : ""; }
  std::string ModWrapperPath() { return IsKernel24() ? "./embc_mod_wrapper.o" : "runtime/embc_mod_wrapper.c"; }
  std::string MakefileTemplatePath() { return IsKernel26() ? (IsFastEmbCBuilds() ? "runtime/EmbC.build/Makefile_for_Kbuild.EmbC" : "runtime/Kernel2.6MakefileTemplate") : ""; }
  std::string Makefile() { return (IsKernel26() && IsFastEmbCBuilds()) ? "Makefile_for_Kbuild.EmbC" : ""; }
  std::string LdPath() { return "ld"; }
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
#ifndef OS_WINDOWS
      transNotifyThread(0), daqReadThread(0),
#endif
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

  unsigned int dio_scheduled_wave_num_columns;  /* Default is for DIO scheduled waves not to have the 'loop' column;
						   If the 'loop' column is to be used, then it is the 9th column, 
						   and this variable should be set to 9. The need for this variable
						   exists only here in FSMServer.cpp. By the time SchedWave specs
						   are sent to fsm.c, we've gotten rid of the silly matrix 
						   message-passing, and we pass proper sensible C structs. 
						   Current legal values are 8, 9, 10, or 11: the last 3 are loop, 
						   wave_ids to trigger, and wave_ids to untrigger
						*/
  bool use_happenings;                          /* Default is false. When this is true, we expect a happenings
						   spec and a happenings list to be sent with the state matrix */

  void *transNotifyThrFun();
  void *daqThrFun();
  void *nrtThrFun();
  void doNRT_IP(const NRTOutput *, bool isUDP, bool isBinData) const;
};

static void *transNotifyThrWrapper(void *);
static void *daqThrWrapper(void *);
static void *nrtThrWrapper(void *);
  
static FSMSpecific fsms[NUM_STATE_MACHINES];

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
  unsigned shm_num, client_ver;
  std::string remoteHost;
  volatile bool thread_running, thread_ran;
    
  static const unsigned MAX_LINE = 16384;
  char sockbuf[MAX_LINE];
  unsigned sockbuf_len;
  static pthread_mutex_t compileLock;
  
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
  std::string sockReceiveLine(bool suppressLineLog = false);
  bool matrixToRT(const Matrix & m, unsigned numEvents, unsigned numSchedWaves, const std::string & inChanType, unsigned readyForTrialState,  const std::string & outputSpecStr, bool state0_fsm_swap_flg);
  bool matrixToRT(const StringMatrix & m,
                  const std::string & globals,
                  const std::string & initfunc,
                  const std::string & cleanupfunc,
                  const std::string & transitionfunc,
                  const std::string & tickfunc,
                  const std::string & threshfunc,
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

  Matrix doGetTransitionsFromRT(int & first, int & last, int & state, bool old_format = false);
  Matrix doGetTransitionsFromRT(int & first, int & last, bool old_format = false) { int dummy; return doGetTransitionsFromRT(first, last, dummy, old_format); }
  Matrix doGetTransitionsFromRT(int & first, bool old_format = false) { int dummy = -1; return doGetTransitionsFromRT(first, dummy, old_format); }
  Matrix doGetSimInpEvtsFromRT();
  Matrix doGetStimuliFromRT(int & first, int & last);

  IntStringMap parseIntStringMapBlock(const std::string &); ///< not static because it needs object for logging errors
  static void parseStringTable(const char *stable, StringMatrix &m);
  static std::string genStringTable(const StringMatrix &m);
  static std::string genStringTable(const Matrix &m);

  std::ostringstream error_string;  /* used for error messages to be sent back to Client if error during reading */

  int  mapHappName2ID(std::string myHappSpecName);
  bool sockReadHappeningsSpecs();
  bool sockReadHappeningsList();
  /* Check whether happening specs user asked for are legal, and map detectorFunctionNames to detectorFunctionNumbers: */
  bool happeningSpecsAreLegal(int num_specs, happeningUserSpec *happeningSpecs);



  // the extern "C" wrapper func passed to pthread_create
  friend void *threadfunc_wrapper(void *);
  void *threadFunc();
};


/* some statics for class ConnectionThread */
int ConnectionThread::id = 0;
pthread_mutex_t ConnectionThread::compileLock = PTHREAD_MUTEX_INITIALIZER;

ConnectionThread::ConnectionThread() : sock(-1), thread_running(false), thread_ran(false), sockbuf_len(0) { myid = id++; fsm_id = 0; shm_num = 0; client_ver = 0; sockbuf[0] = 0; }
ConnectionThread::~ConnectionThread()
{ 
  if (sock > -1)  shutdown(sock, SHUT_RDWR), closesocket(sock), sock = -1;
  if (isRunning())  pthread_join(handle, NULL), thread_running = false;
  Log() <<  "deleted." << std::endl;
}

extern "C" 
{
  static void * threadfunc_wrapper(void *arg)   
  { 
    pthread_detach(pthread_self());
    ConnectionThread *me = static_cast<ConnectionThread *>(arg);
    void * ret = 0;
    try {
        ret = me->threadFunc();  
    } catch (const FatalException & e) {
        Log() << "Caught FATAL exception: " << e.why() << " --  Exiting program...\n";      
        std::abort();
    } catch (const Exception & e) {
        Log() << "Caught exception: " << e.why() << " -- Exiting thread...\n";
    }
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

/* NOTE that lots of functions in this program have the potential to throw
 * Exception on failure (which is why they seem not to haev error status return values) */

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
    std::string errmsg;
    fsm.fifo_in = RTOS::openFifo(shm->fifo_out[f], RTOS::Read, &errmsg);
    if (fsm.fifo_in == RTOS::INVALID_FIFO) throw Exception (std::string("Could not open RTF fifo_in for reading: ") + errmsg);
    // Clear any pending/stale data in case we crashed last time and
    // couldn't read all data off reply fifo
    fifoReadAllAvail(fsm.fifo_in);
    fsm.fifo_out = RTOS::openFifo(shm->fifo_in[f], RTOS::Write, &errmsg);
    if (fsm.fifo_out == RTOS::INVALID_FIFO) throw Exception (std::string("Could not open RTF fifo_out for writing: ") + errmsg);
    fsm.fifo_trans = RTOS::openFifo(shm->fifo_trans[f], RTOS::Read, &errmsg);
    if (fsm.fifo_trans == RTOS::INVALID_FIFO) throw Exception (std::string("Could not open RTF fifo_trans for reading: ") + errmsg);
    fsm.fifo_daq = RTOS::openFifo(shm->fifo_daq[f], RTOS::Read, &errmsg);
    if (fsm.fifo_daq == RTOS::INVALID_FIFO) throw Exception (std::string("Could not open RTF fifo_daq for reading: ") + errmsg);
    fsm.fifo_nrt_output = RTOS::openFifo(shm->fifo_nrt_output[f], RTOS::Read, &errmsg);
    if (fsm.fifo_nrt_output == RTOS::INVALID_FIFO) throw Exception (std::string("Could not open RTF fifo_nrt_output for reading: ") + errmsg);
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

#ifdef OS_WINDOWS
static void doWsaStartup()
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
  std::cerr << "Deleting old/crufty FSM files from TEMP dir..\n";
#ifdef OS_WINDOWS
  std::system((std::string("del /F /Q \"") + TmpPath() + "\"\\fsm*_*.c").c_str());
  std::system((std::string("del /F /Q \"") + TmpPath() + "\"\\fsm*_*.def").c_str());
  std::system((std::string("del /F /Q \"") + TmpPath() + "\"\\fsm*_*.dll").c_str());
#else
  std::system((std::string("rm -fr '") + TmpPath() + "'/fsm*_*.c").c_str());
  std::system((std::string("rm -fr '") + TmpPath() + "'/fsm*_*.so").c_str());
#endif
}

static bool isSingleInstance()
{
#if defined(OS_LINUX) || defined(OS_OSX)
    return ::num_procs_of_my_exe_no_children() <= 1;
#elif defined(OS_OSX)
    return true;
#elif defined(OS_WINDOWS)
    HANDLE mut = CreateMutexA(NULL, FALSE, "Global\\FSMServer.exe.Mutex");
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
#if !defined(EMULATOR)  
  if (::geteuid() != 0)
    throw Exception("Need to be root or setuid-root to run this program as it requires the ability to load kernel modules!");
  std::string missing;
  if (IsKernel24()) {
    if ( !FileExists(missing = EmbCHPath()) || !FileExists(missing = CompilerPath()) || !FileExists(missing = ModWrapperPath()) ) 
      throw Exception(std::string("Required file or program '") + missing + "' is not found -- make sure you are in the rtfsm build dir and that you fully built the rtfsm tree!");
  } else if (IsKernel26()) {
    if ( !FileExists(missing = EmbCHPath()) || !FileExists(missing = MakefileTemplatePath()) || !FileExists(missing = ModWrapperPath()) || (IsFastEmbCBuilds() && ( !FileExists(missing = EmbCBuildPath()) || !FileExists(missing = (EmbCBuildPath()+"/.mod.ko.cmd")) ) ) )
      throw Exception(std::string("Required file or program '") + missing + "' is not found -- make sure you are in the rtfsm build dir and that you fully built the rtfsm tree!");
    if (CompilerPath().length() && IsFastEmbCBuilds()) {
        throw Exception(std::string("Cannot use -c switch *and* use the fast embedded C build scheme because the compiler was already decided by the rtfsm build system.  Please specify -e switch to turn off fast embedded C builds (or omit the -c switch)."));
    }
  } else 
      throw Exception("Could not determine Linux kernel version!  Is this Linux??");
#endif

  for (int f = 0; f < NUM_STATE_MACHINES; ++f) {
    fsms[f].dio_scheduled_wave_num_columns = 8;        /* Initialize default of 8 items for sched wave spec */
    fsms[f].use_happenings                  = false;
  }
  attachShm();
  openFifos();
  createTransNotifyThreads();
  createDAQReadThreads();
  createNRTReadThreads();
}
  
static void cleanup(void)
{
  if (listen_fd >= 0) { shutdown(listen_fd, SHUT_RDWR), closesocket(listen_fd);  listen_fd = -1; }
  closeFifos();
  if (shm) { RTOS::shmDetach((void *)shm); shm = 0; }
#ifdef OS_WINDOWS
  WSACleanup();
#endif
}


static void handleArgs(int argc, char *argv[])
{
  int opt;
  while ( ( opt = getopt(argc, argv, "edl:c:")) != -1 ) {
    switch(opt) {
    case 'e': 
        Log() << "-e switch, using slower more compatible EmbC build mechanism\n";
        fast_embc_builds = false; 
        break;
    case 'd': 
        Log() << "-d switch, debug printing turned on\n";        
        debug = true; 
        break;
    case 'l': 
      listenPort = atoi(optarg);
      Log() << "-l switch, set listenport to `" << optarg << "'\n";
      if (! listenPort) throw Exception ("Could not parse listen port.");
      break;
    case 'c':
      compiler = optarg;
      Log() << "-c switch, set compiler to `" << optarg << "'\n";
      break;
    default:
      throw Exception(std::string("Unknown command line parameters.\n\nUsage: ")
                      + argv[0] + " [-l LISTEN_PORT] [-c COMPILER_OVERRIDE] [-e] [-d]");
      break;
    }
  }
}

static void sighandler(int sig)
{
#ifndef OS_WINDOWS    
  if (sig == SIGPIPE) return; // ignore SIGPIPE..
#endif

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
  int ext_trig;
};

static void doServer(void)
{
  //do the server listen, etc..
    
  listen_fd = socket(PF_INET, SOCK_STREAM, 0);

  if (listen_fd < 0) 
    throw Exception(std::string("socket: ") + GetLastNetErrStr());

  struct sockaddr_in inaddr;
  socklen_t addr_sz = sizeof(inaddr);
  inaddr.sin_family = AF_INET;
  inaddr.sin_port = htons(listenPort);
  inet_aton("0.0.0.0", &inaddr.sin_addr);

  Log() << "FSM Server version " << VersionSTR << std::endl;
  if (compiler.length()) 
    Log() << "EmbC compiler override: " << CompilerPath() << std::endl;
  Log() << "Listening for connections on port: " << listenPort << std::endl; 

  
  const int parm_int = 1, parmsz = sizeof(parm_int);
  const char *parm = (const char *)&parm_int;
  if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, parm, parmsz) )
    Log() << "Error: setsockopt returned " << GetLastNetErrStr() << std::endl; 
  
  if ( bind(listen_fd, (struct sockaddr *)&inaddr, addr_sz) != 0 ) 
    throw Exception(std::string("bind: ") + GetLastNetErrStr());
  
  if ( listen(listen_fd, 1) != 0 ) 
    throw Exception(std::string("listen: ") + GetLastNetErrStr());

  while (1) {
    int sock;
    if ( (sock = accept(listen_fd, (struct sockaddr *)&inaddr, &addr_sz)) < 0 ) 
      throw Exception(std::string("accept: ") + GetLastNetErrStr());

    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, parm, parmsz) )
      Log() << "Error: setsockopt returned " << GetLastNetErrStr() << std::endl; 
    
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
  signal(SIGTERM, sighandler);
#ifndef OS_WINDOWS
  signal(SIGPIPE, sighandler);
  signal(SIGQUIT, sighandler);
  signal(SIGHUP, sighandler);
#endif

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





bool ConnectionThread::happeningSpecsAreLegal(int num_specs, happeningUserSpec *happeningSpecs) 
/*
  Checks that all happeningUserSpecs have unique happIds, and that they all use existing detectorFunctionNames; 
  in this process, also fills in the detectorFunctionNumber for each of the user specs. Returns true if all ok.
 */
{
  int i,j; bool foundspec;
  for( i=0; i<num_specs; i++ ) {
    for( j=0; j<i; j++ ) {  /* This j loop checks for duplicate happIds */
      if ( happeningSpecs[j].happId == happeningSpecs[i].happId ) { error_string.str(""); 
	error_string << "ERROR! I'm being sent duplicate happening ID ";
	error_string << happeningSpecs[i].happId << " in the happening specs!!" << std::endl;
	return false;
      }
    } /* end for j */

    std::string str1, str2;
    foundspec = false;
    for( j=0; j<NUM_HAPPENING_DETECTOR_FUNCTIONS; j++ ) { /* This j loop checks that all requested 
							     detectorsFunctioNames exist internally */
      str1 = happeningDetectorsList[j].detectorFunctionName;
      str2 = happeningSpecs[i].detectorFunctionName;
      if (str1 == str2) {
	foundspec = true;
	happeningSpecs[i].detectorFunctionNumber = j;
      }
    }
    if (!foundspec) { error_string.str("");
      error_string << "ERROR! All detectorFunctionNames must be names known internally-- " 
		   << " I don't know \"" << happeningSpecs[i].detectorFunctionName 
		   << "\", call GET HAPPENING SPECS for a list of known functions " << std::endl;
      return false;
    }
  }

  return true;
}


bool ConnectionThread::sockReadHappeningsSpecs()
/* return TRUE if read was fine*/
{
  // Log() << "Carlos: entered sockReadHappeningSpecs()" << std::endl;
  int i, num_specs = 0; std::string line;
  if ((line = sockReceiveLine()).length() == 0) { error_string.str("");
    error_string << "ERROR: expecting a line with the number of happening specs and not finding it!" << std::endl;
    return false;
  }
   
  std::string::size_type pos = line.find_first_of("0123456789");
  if (pos == std::string::npos) { error_string.str(""); 
    error_string << "ERROR:expected a number (total number of happenings), but didn't find it." << std::endl;
    return false;
  }

  std::stringstream s(line.substr(pos));
  s >> num_specs;
  // Log() << "Carlos: I'm told to expect " << num_specs << " happening specs " << std::endl;
  if (num_specs > MAX_HAPPENING_SPECS) { error_string.str(""); 
    error_string << "ERROR: max number of happening specs is fixed at " << MAX_HAPPENING_SPECS << 
      " but you're telling me you want to send me " << num_specs << std::endl;
    return false;
  }

  msg.u.fsm.plain.numHappeningSpecs = num_specs;
  static struct happeningUserSpec *myspec = NULL;
  for ( i=0; i<num_specs; i++ ) { /* iterate over happening specs */
    if ((line = sockReceiveLine()).length() == 0) {  /* it's an error if we couldn't read the line */
      error_string.str(""); 
      error_string << "ERROR: expected to read a line for happening spec " << i << " but didn't get it " << std::endl;
      return false; 
    }

    /* FINISH CHECKING BOTH OSCKREADHAPP FNS AND MAKE SURE THEIR BOOL RETURNS AND ERROR STRINGS ARE HANDLED OK */
    /* 2nD SOCKREADFN NEEDS CHECKING FOR EWRROR STRUNG */

    std::stringstream s(line);
    myspec = &(msg.u.fsm.plain.happeningSpecs[i]);
    // Log() << "Carlos: got line \"" << line << "\" now in stringstream \"" << s << "\"." << std::endl;
    s >> myspec->name >> myspec->detectorFunctionName >>
      myspec->inputNumber >> myspec->happId;
    // Log() << "Carlos: Got happening space with name \"" << myspec->name << "\", detectorFunctionName \"" << myspec->detectorFunctionName << "\", inputNumber " << myspec->inputNumber << ", happId " << myspec->happId << std::endl;
  } /* end for ( i=0 ... iteration over happening specs */

  /* Have received all happening specs, now go through them to check they are legal, and map detectorFunctioNames
     to detectorFunctionNumbers: */
  if (!happeningSpecsAreLegal(num_specs, msg.u.fsm.plain.happeningSpecs)) return false;

  return true;
}

int ConnectionThread::mapHappName2ID(std::string myHappSpecName)
/* Assumes that the happening specs in msg are legal. Runs through them to find which one
   has a name matching myHappSecName; if found, returns its position in the list. If not
   found, returns -1. */
{
  int k; 
  for( k=0; k<(int)msg.u.fsm.plain.numHappeningSpecs; k++ )
    if (myHappSpecName == msg.u.fsm.plain.happeningSpecs[k].name) return k;
  return -1;
}

bool ConnectionThread::sockReadHappeningsList()
/* returns TRUE if read was successful */
{
  Log() << "Carlos: Going into 'SET HAPPENINGS LIST'" << std::endl;

  int i, num_happenings = 0;  std::string line;
  if ((line = sockReceiveLine()).length() == 0) { error_string.str("");
    error_string << "ERROR: expecting a line with the total number of happenings specs and not finding it!" << std::endl;
    return false;
  }
  std::string::size_type pos = line.find_first_of("0123456789");
  if (pos == std::string::npos) { error_string.str("");
    error_string << "ERROR:expected a number (total number of happenings), but didn't find it." << std::endl;
    return false;
  }

  std::stringstream s(line.substr(pos));
  s >> num_happenings;
  Log() << "Will read " << num_happenings << " total happenings " << std::endl;
  if (num_happenings > MAX_HAPPENING_LIST_LENGTH) { error_string.str("");
    error_string << "ERROR: max happening list length is fixed at " << MAX_HAPPENING_LIST_LENGTH 
       << " but you're telling me you want to send me " << num_happenings << std::endl;
    return false;
  } 
    
  int totalReadHapps = 0;
  for ( i=0; i<(int)msg.u.fsm.plain.numStates; i++ ) {  /* iterate over total number of states */
    int my_num_happs, j;
    if ((line = sockReceiveLine()).length() == 0) {  /* it's an error if we couldn't read the line */
      error_string.str("");
      error_string << "ERROR: expected to read a line for the number of happenings of state " << i 
		   << " but didn't get it " << std::endl;
      return false; 
    }
    pos = line.find_first_of("0123456789");
    if ( pos==std::string::npos ) { error_string.str(""); /* error if we don't find a number */
      error_string << "ERROR: expected the number of happenings for state " << i << " but didn't find it " << std::endl;
      return false; 
    }
    std::stringstream s(line.substr(pos));
    s >> my_num_happs; /* number of happenings in this state */
    if ( my_num_happs > num_happenings || my_num_happs < 0 ) { /* error if one state has more than the 
								  total num of happenings! */
      error_string.str("");
      error_string << "ERROR: I was told that the total sum of happenings across states was " << num_happenings 
		    << " but now you tell me state " << i << " has " << my_num_happs << " happenings??? " << std::endl;
      return false;
    } else if ( totalReadHapps + my_num_happs > num_happenings ) { error_string.str("");
      error_string << "ERROR: I was told that the total sum of happenings across states was " << num_happenings 
		   << " but adding in the ones for state " << i 
		   << " would take me to " << totalReadHapps + my_num_happs << std::endl;
      return false;
    }
    /* ok, number of happenings we're expecting to read for this state looks ok */
    msg.u.fsm.plain.numHappenings[i] = my_num_happs;  /* number of happenings of this state */
    msg.u.fsm.plain.myHapps[i]       = totalReadHapps; /* where this state's happs start in the total happs list */
    for( j=0; j<my_num_happs; j++, totalReadHapps++ ) { /* iterate over happenings for this state */
      std::string myHappSpecName;
      int myHappSpecNum, myJumpState;

      if ((line = sockReceiveLine()).length() == 0) {  /* it's an error if we couldn't read the line */
	error_string.str("");
	error_string << "ERROR: expected to read a line for happening " << j+1 << " of state " << i 
		     << " but didn't get it " << std::endl;
	return false; 
      }
      std::stringstream s(line);
      s >> myHappSpecName;
      s >> myJumpState;
      if ( (myHappSpecNum = mapHappName2ID(myHappSpecName)) < 0 ) { error_string.str("");
	error_string << "ERROR: I couldn't find the happening spec name \"" << myHappSpecName << "\" in the list "
		     << "of happening specs that I was sent." << std::endl;
	return false;
      }      
      if ( myJumpState < 0 || myJumpState >= (int)msg.u.fsm.plain.numStates ) { error_string.str("");
	error_string << "ERROR: I have " << msg.u.fsm.plain.numStates << " states in my spec, but state " << i 
		     << " wants a happening that jumps to state  " << myHappSpecNum << "????" << std::endl;
	return false;
      }
      if ( myHappSpecNum < 0 || myHappSpecNum >= (int)msg.u.fsm.plain.numHappeningSpecs ) { error_string.str("");
	error_string << "ERROR: was previously sent " << msg.u.fsm.plain.numHappeningSpecs << " total happening specs " 
		     << "but now state " << i << " is asking for spec number " << myHappSpecNum << "????" << std::endl;
	return false;
      }
      /* ok, this happening entry looks ok */
      msg.u.fsm.plain.happsList[totalReadHapps].happeningSpecNumber = (unsigned)myHappSpecNum;
      msg.u.fsm.plain.happsList[totalReadHapps].stateToJumpTo       = (unsigned)myJumpState;
    } /* end iterate over happenings for one state */
    // Log() << "Read " << my_num_happs << " happenings for state " << i << std::endl;
  } /* end iterate over states */	

  if ( totalReadHapps != num_happenings ) { error_string.str("");
    error_string << "ERROR: I was told that the sum of happenings across states was " << num_happenings 
		 << " but after reading all states, I only got " << totalReadHapps << std::endl;
    return false;
  }
  
  return true;
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
    if (line.find("VERSION") == 0) {
        std::ostringstream os;
        os << VersionNUM << " - [" << VersionSTR << "]\n";
        sockSend(os.str());
        cmd_error = false;
    } else if (line.find("CLIENTVERSION") == 0) {
      std::string::size_type pos = line.find_first_of("0123456789");
      if ( pos != std::string::npos ) {
          unsigned ver = 0;
          std::istringstream is(line.substr(pos));
          is >> ver;
          if (ver >= MinimumClientVersion) {
              cmd_error = false;
              client_ver = ver;
          } else {
              Log() << "Client version " << ver << " does not meet minimum requirement of " << MinimumClientVersion << "\n";
              sockSend("ERROR client version is incompatible\n");
              break;
          }
      }
    } else if (line.find("NOOP") == 0) {
      // noop is just used to test the connection, keep it alive, etc
      // it doesn't touch the shm...
      cmd_error = false;        
    } else if ( line.find("EXIT") == 0 || line.find("BYE") == 0 || line.find("QUIT") == 0) {
      Log() << "Graceful exit requested." << std::endl;
      break;
    } else if (client_ver) { // the rest of these commands require that the client has told us its version!
        
        if (line.find("SET STATE PROGRAM") == 0) {
            cmd_error = !doSetStateProgram();
        } else if (line.find("SET STATE MATRIX") == 0) {
            /* FSM Upload.. */
        
      // determine M and N
            std::string::size_type pos = line.find_first_of("0123456789");

            if (pos != std::string::npos) {
	      unsigned m = 0, n = 0, num_Events = 0, num_SchedWaves = 0, readyForTrialState = 0, num_ContChans = 0, num_TrigChans = 0, num_Vtrigs = 0, state0_fsm_swap_flg = 0;
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
			  if (!matrixToRT(mat, num_Events, num_SchedWaves, inChanType, 
					  readyForTrialState, outputSpecStr, state0_fsm_swap_flg)) break;
			  
                          // Log() << "Carlos: Just received the matrix" << std::endl;

			  if ( fsms[fsm_id].use_happenings ) {
			    std::string line;   
			    if ((line = sockReceiveLine()).length() == 0) { 
			      error_string.str("");
			      error_string << "ERROR: expecting a line with SET HAPPENINGS SPEC " 
					   << "and not finding it!" << std::endl;
			      break;
			    }
			    if (!sockReadHappeningsSpecs()) {
			      Log() << error_string.str(); sockSend(error_string.str()); break;			    
			    } else 
			      sockSend("OK\n");

			    if ((line = sockReceiveLine()).length() == 0) { 
			      error_string.str("");
			      error_string << "ERROR: expecting a line with SET HAPPENINGS LIST " 
					   << "and not finding it!" << std::endl;
			      break;
			    }
			    if (!sockReadHappeningsList()) {
			      Log() << error_string.str(); sockSend(error_string.str()); break;			    
			    } else
			      sockSend("OK\n");
			  } /* end if use_happenings */

 			  sendToRT(msg); 
			  cmd_error = false;
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
            switch (msg.u.fsm.type) {
            case FSM_TYPE_EMBC: {
                std::auto_ptr<char> program(new char[msg.u.fsm.program.program_len+1]);
                memcpy(program.get(), msg.u.fsm.program.program_z, msg.u.fsm.program.program_z_len);
                inflateInplace(program.get(), msg.u.fsm.program.program_z_len, msg.u.fsm.program.program_len);
                program.get()[msg.u.fsm.program.program_len] = 0;
                std::vector<std::string> lines = splitString(program.get(), "\n", false, false);
                std::stringstream s;
                s << "LINES " << lines.size() << "\n" << program.get();
                std::string str = s.str();
                if (str[str.length()-1] != '\n') str = str + "\n";
                sockSend(str);
                cmd_error = false;
            }
                break;
            case FSM_TYPE_PLAIN: {
                sockSend("LINES 0\n");
                cmd_error = false;
            }
                break;
            default:
                Log() << "INVALID MATRIX TYPE! Expected one of 'FSM_TYPE_PLAIN' or 'FSM_TYPE_EMBC'\n";
                cmd_error = true;
                break;
            }
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
        } else if (line.find("SET DIO SCHED WAVE NUM COLUMNS") == 0 ) { 
            unsigned int num_columns = 8;
            std::string::size_type pos = line.find_first_of("0123456789");
            if (pos != std::string::npos) {
                std::stringstream s(line.substr(pos));
                s >> num_columns;
                if ( MIN_DIO_SCHED_WAVE_NUM_COLUMNS <= num_columns & num_columns <= MAX_DIO_SCHED_WAVE_NUM_COLUMNS) { // ensure legal # of columns
		  fsms[fsm_id].dio_scheduled_wave_num_columns = num_columns;
		  cmd_error = false;
                } else {
		  std::ostringstream os;
		  os << "SET DIO SCHED WAVE NUM COLUMNS expects an integer between " << MIN_DIO_SCHED_WAVE_NUM_COLUMNS;
		  os << " and " << MAX_DIO_SCHED_WAVE_NUM_COLUMNS << " but you gave me " << num_columns << " \n";
		  sockSend(os.str());
		  cmd_error = true;
		}
            }
        } else if (line.find("GET DIO SCHED WAVE NUM COLUMNS") == 0 ) { 
	  std::ostringstream os;
	  os << fsms[fsm_id].dio_scheduled_wave_num_columns << std::endl;
	  sockSend(os.str());
	  cmd_error = false;
        } else if (line.find("USE HAPPENINGS") == 0 ) { 
	  fsms[fsm_id].use_happenings = true;
	  cmd_error = false;
        } else if (line.find("DO NOT USE HAPPENINGS") == 0 ) { 
	  fsms[fsm_id].use_happenings = false;
	  cmd_error = false;
        } else if (line.find("GET USE HAPPENINGS FLAG") == 0 ) { 
	  std::ostringstream os;
	  os << (int)fsms[fsm_id].use_happenings << std::endl;
	  sockSend(os.str());
	  cmd_error = false;
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
        } else if (line.find("GET INPUT CHANNEL STATES") == 0) {
            msg.id = GETINPUTCHANNELSTATES;
            sendToRT(msg);
	    /* Read the vector of states out from the returned message */
	    Matrix mat(1, msg.u.input_channels.num);
	    for(int i=0; i<(int)msg.u.input_channels.num; ++i) 
	      mat.at(0,i) = msg.u.input_channels.state[i];
	    /* Tell the client that a vector is coming */
            std::ostringstream os;
            os << "MATRIX " << mat.rows() << " " << mat.cols() << std::endl;
            sockSend(os.str());
	    /* Wait for the client's READY signal */
	    line = sockReceiveLine(); // wait for "READY" from client
	    if (line.find("READY") != std::string::npos) {
	      /* send the result */
	      sockSend(mat.buf(), mat.bufSize(), true);
	      cmd_error = false;
	    }
        } else if (line.find("GET EVENTS_II") == 0) {
            std::string::size_type pos = line.find_first_of("0123456789");
            if (pos != std::string::npos) {
                std::stringstream s(line.substr(pos));
                int first = -1, last = -1;
                s >> first >> last;
                Matrix mat = doGetTransitionsFromRT(first, last, false);

                std::ostringstream os;
                os << "MATRIX " << mat.rows() << " " << mat.cols() << std::endl;
                sockSend(os.str());

                line = sockReceiveLine(); // wait for "READY" from client

                if (line.find("READY") != std::string::npos) {
                    sockSend(mat.buf(), mat.bufSize(), true);
                    cmd_error = false;
                }
            }
        } else if (line.find("GET STIMULI COUNTER") == 0) {
            msg.id = STIMULICOUNT;
            sendToRT(msg);
            std::stringstream s;
            s << msg.u.fsm_events_count << std::endl;
            sockSend(s.str());
            cmd_error = false;
        } else if (line.find("GET STIMULI") == 0) {
            std::string::size_type pos = line.find_first_of("-0123456789");
            if (pos != std::string::npos) {
                std::istringstream is(line.substr(pos));
                int first = 0, last = 0;
                is >> first >> last;
                Matrix mat  = doGetStimuliFromRT(first, last);
                cmd_error = false;
                std::ostringstream os;
                os << "MATRIX " << mat.rows() << " " << mat.cols() << std::endl;
                sockSend(os.str());

                line = sockReceiveLine(); // wait for "READY" from client
                
                if (line.find("READY") != std::string::npos) {
                    sockSend(mat.buf(), mat.bufSize(), true);
                } else
                    cmd_error = true;
            }
        } else if (line.find("GET EVENTS") == 0) {
            std::string::size_type pos = line.find_first_of("0123456789");
            if (pos != std::string::npos) {
                std::stringstream s(line.substr(pos));
                int first = -1, last = -1;
                s >> first >> last;
                Matrix mat = doGetTransitionsFromRT(first, last, true);

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
        } else if (line.find("NOTIFY EVENTS") == 0) {
            std::string::size_type pos = line.find("VERBOSE");
            bool verbose = pos != std::string::npos;
            sockSend("OK\n"); // tell them we accept the command..
            doNotifyEvents(verbose); // this doesn't return for a LONG time normally..
            break; // if we return from the above, means there is a socket error
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
                unsigned m = 0, n = 0, id = 0, aoline = 0, loop = 0, pend_flg = 0;
                std::stringstream s(line.substr(pos));
                s >> m >> n >> id >> aoline >> loop >> pend_flg;
                if (m && n) {
        // guard against memory hogging DoS
                    if (m*n*sizeof(double) > FSM_MEMORY_BYTES) {
                        Log() << "Error, incoming matrix would exceed cell limit of " << FSM_MEMORY_BYTES/sizeof(double) << std::endl; 
                        break;
                    }
                    msg.id = GETVALID;
                    sendToRT(msg);
                    if (!msg.u.is_valid) {
                  // can't upload AOWaves to an invalid fsm!
                        cmd_error = true;
                    } else { 

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
                            msg.u.aowave.pend_flg = pend_flg;
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
                        cmd_error = false;        
                    }
                } else { 
              // m or n were 0.. indicates we are clearing an existing wave
                    msg.id = AOWAVE;
                    msg.u.aowave.id = id;
                    msg.u.aowave.nsamples = 0;
                    sendToRT(msg);
                }
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
        } else if (line.find("GET AIMODE") == 0) { // GET AIMODE
            msg.id = GETAIMODE;
            sendToRT(msg);
            if (msg.u.ai_mode_is_asynch)
                sockSend("asynch\n");
            else
                sockSend("synch\n");
            cmd_error = false;
        } else if (line.find("SET AIMODE") == 0) { // SET AIMODE
            std::istringstream s(line.substr(10));
            std::string mode;
            s >> mode;
            std::transform(mode.begin(), mode.end(), mode.begin(), tolower);
            msg.id = SETAIMODE;
            if (mode == "asynch")
                msg.u.ai_mode_is_asynch = 1, cmd_error = false;
            else if (mode == "synch")
                msg.u.ai_mode_is_asynch = 0, cmd_error = false;
            if (!cmd_error)
                sendToRT(msg);
        } else if (line.find("GET SIMULATED INPUT EVENT QUEUE") == 0) {
            Matrix mat = doGetSimInpEvtsFromRT();            
            std::ostringstream os;
            os << "MATRIX " << mat.rows() << " " << mat.cols() << std::endl;
            sockSend(os.str());

            line = sockReceiveLine(); // wait for "READY" from client

            if (line.find("READY") != std::string::npos) {
                sockSend(mat.buf(), mat.bufSize(), true);
                cmd_error = false;
            }
        } else if (line.find("ENQUEUE SIMULATED INPUT EVENTS") == 0) {
            std::string::size_type pos = line.find_first_of("-0123456789");
            if (pos != std::string::npos) {
                std::istringstream is(line.substr(pos));
                double ts = 0.;
                unsigned m = 0, n = 0;
                int ret;
                is >> ts >> m >> n;
                if (m < MAX_SIM_INP_EVTS && (!m || n == 2)) {
                    if ( (ret = sockSend("READY\n")) <= 0 ) {
                        Log() << "Send error..." << std::endl; 
                        break;
                    }
                    Log() << "Getting ready to receive simulated input events matrix sized " << m << "x" << n << std::endl; 
                
                    Matrix mat (m, n);
                    ret = sockReceiveData(mat.buf(), mat.bufSize());
                    if (ret == (int)mat.bufSize()) {
                        if (debug) { 
                            Log() << "Matrix is:" << std::endl; 
                            for (int i = 0; i < (int)mat.rows(); ++i) {
                                TLog log = Log();
                                for (int j = 0; j < (int)mat.cols(); ++j)
                                    log << mat.at(i,j) << " " ;
                                log << "\n";       
                            }
                        }
                        msg.id = ENQSIMINPEVTS;
                        msg.u.sim_inp.num = m;
#ifdef EMULATOR
                        msg.u.sim_inp.ts_for_clock_latch = static_cast<long long>(ts*1e9);
#endif
                        for (unsigned i = 0; i < m; ++i) {
                            msg.u.sim_inp.evts[i].event_id = static_cast<int>(mat.at(i, 0));
                            msg.u.sim_inp.evts[i].ts = static_cast<long long>(mat.at(i, 1) * 1e9);
                        }
                        sendToRT(msg); 
                        cmd_error = false;
                    } else if (ret <= 0) {
                        break;
                    }
                }
            }
        } else if (line.find("CLEAR SIMULATED INPUT EVENTS") == 0) {
            msg.id = CLEARSIMINPEVTS;
            sendToRT(msg);
            cmd_error = false;
        } else if (line.find("SET NUM STATES") == 0 ) { /* Here only for debug purposes, user shouldn't use
							   this cmd-- sending num states is 
							   part of SET STATE MATRIX */
	  std::string::size_type pos = line.find_first_of("0123456789");
	  if (pos == std::string::npos) { error_string.str("");
	    error_string << "ERROR:expected a number(num states), but didn't find it." << std::endl;
	    sockSend(error_string.str()); Log() << error_string.str();
	  }
	  std::stringstream s(line.substr(pos));
	  s >> msg.u.fsm.plain.numStates;
	  cmd_error = false;
        } else if (line.find("GET NUM STATES") == 0 ) {
	  std::stringstream s;
	  s << msg.u.fsm.plain.numStates << std::endl;
	  sockSend(s.str());
	  cmd_error = false;
        } else if (line.find("SET HAPPENINGS SPEC") == 0 ) { /* Here for debug purposes, user shouldn't use
								this cmd-- sending happenings spec is 
								part of SET STATE MATRIX */
	  if (sockReadHappeningsSpecs()) cmd_error = false;
	  else                         { Log() << error_string.str(); sockSend(error_string.str()); }
        } else if (line.find("SET HAPPENINGS LIST") == 0 ) { /* Here for debug purposes, user shouldn't use
								this cmd-- sending happenings list is
								part of SET STATE MATRIX */
	  if (sockReadHappeningsList())  cmd_error = false;
	  else                         { Log() << error_string.str(); sockSend(error_string.str()); }
        } else if (line.find("GET HAPPENINGS SPEC") == 0) {
	  std::ostringstream os; int i;
	  os << "LINES " << msg.u.fsm.plain.numHappeningSpecs << std::endl;
	  for(i=0; i<(int)msg.u.fsm.plain.numHappeningSpecs; i++ ) {
	    happeningUserSpec thisSpec = msg.u.fsm.plain.happeningSpecs[i];
	    os << "Name=\"" << thisSpec.name << "\" detectorFunctionName=\"" << thisSpec.detectorFunctionName 
	       << "\"" << " inputNumber=" << thisSpec.inputNumber << " happId=" << thisSpec.happId
	       << " detectorFunctionNumber=" << thisSpec.detectorFunctionNumber << std::endl;
	  }
	  sockSend(os.str());
	  cmd_error = false;
        } else if (line.find("GET HAPPENINGS LIST") == 0) {
	  std::ostringstream os; int i, j, statenum, tothapps=0, actualhapps=0;
	  for( tothapps = 0, i=0; i<(int)msg.u.fsm.plain.numStates; i++ ) tothapps += msg.u.fsm.plain.numHappenings[i];
	  os << "LINES " << tothapps+2 << std::endl;
	  for( statenum=0; statenum<(int)msg.u.fsm.plain.numStates; statenum++ ) {
	    int myhapps = msg.u.fsm.plain.numHappenings[statenum];
	    for ( j=0; j<myhapps; j++, actualhapps++ ) {
	      os << "Happ " << j << " in state " << statenum << ": happeningSpecNumber=" 
		 << msg.u.fsm.plain.happsList[actualhapps].happeningSpecNumber
		 << " jumpTo=" << msg.u.fsm.plain.happsList[actualhapps].stateToJumpTo 
		 << " by pointer, specNum=" 
		 << msg.u.fsm.plain.happsList[msg.u.fsm.plain.myHapps[statenum]+j].happeningSpecNumber
		 << " and jumpTo=" << msg.u.fsm.plain.happsList[msg.u.fsm.plain.myHapps[statenum]+j].stateToJumpTo
		 << std::endl;
	    }
	  }
	  os << actualhapps << " total happs " << std::endl;

	  /* for(j=0; j<(int)msg.u.fsm.plain.fsm.num
	    happeningUserSpec thisSpec = msg.u.fsm.plain.happeningSpecs[i];
	    os << "Name=\"" << thisSpec.name << "\" detectorFunctionName=\"" << thisSpec.detectorFunctionName 
	       << "\"" << " inputNumber=" << thisSpec.inputNumber << " happId=" << thisSpec.happId
	       << " detectorFunctionNumber=" << thisSpec.detectorFunctionNumber << std::endl;
	       } */
	  sockSend(os.str());
	  cmd_error = false;
        } else if (line.find("GET HAPPENING DETECTOR FUNCTIONS") == 0) {
	    std::ostringstream os; int i;
	    os << "LINES " << NUM_HAPPENING_DETECTOR_FUNCTIONS+2 << std::endl;
	    os << "These are the functions available as happening detector functions:" << std::endl << std::endl;
	    for(i=0; i<NUM_HAPPENING_DETECTOR_FUNCTIONS; i++) {
	      os << "Function name: \"" << happeningDetectorsList[i].detectorFunctionName << "\". Type=" << 
		(happeningDetectorsList[i].happType==HAPPENING_CONDITION ? "Condition. " : "Event. ") <<
		"Description: \"" << happeningDetectorsList[i].description << "\"" << std::endl;
	    }
	    sockSend(os.str());
	    cmd_error = false;
#ifdef EMULATOR
        } else if (line.find("GET CLOCK LATCH MS") == 0) { // GETCLOCKLATCHMS
            msg.id = GETCLOCKLATCHMS;
            sendToRT(msg);
            std::ostringstream os;
            os << msg.u.latch_time_ms << "\n";
            sockSend(os.str());
            cmd_error = false;
        } else if (line.find("SET CLOCK LATCH MS") == 0) { // SETCLOCKLATCHMS
            std::string::size_type pos = line.find_first_of("0123456789");
            if (pos != std::string::npos) {
                std::istringstream s(line.substr(pos));
                double ms = -1;
                s >> ms;
                msg.id = SETCLOCKLATCHMS;
                if (ms >= 0. && ms < 2000.)
                    msg.u.latch_time_ms = unsigned(ms), cmd_error = false;
                if (!cmd_error)
                    sendToRT(msg);
            }
        } else if (line.find("GET LATCH TIME T0 SECS") == 0) { // CLOCKLATCHQUERY
            msg.id = CLOCKLATCHQUERY;
            sendToRT(msg);
            std::ostringstream os;
            os << (double(msg.u.ts_for_clock_latch/1000LL)/1e6) << "\n";
            sockSend(os.str());
            cmd_error = false;
        } else if (line.find("CLOCK LATCH PING") == 0) { // CLOCKLATCHPING
            msg.id = GETRUNTIME;
            sendToRT(msg);
            long long ts = msg.u.runtime_us*1000LL;
            msg.u.ts_for_clock_latch = ts;
            msg.id = CLOCKLATCHPING;
            sendToRT(msg);
            cmd_error = false;
        } else if (line.find("IS CLOCK LATCHED") == 0) { //CLOCKISLATCHED
            msg.id = CLOCKISLATCHED;
            sendToRT(msg);
            std::ostringstream os;
            os << (msg.u.latch_is_on ? 1 : 0) << "\n";
            sockSend(os.str());
            cmd_error = false;
        } else if (line.find("IS FAST CLOCK") == 0) { //ISFASTCLOCK
            msg.id = ISFASTCLOCK;
            sendToRT(msg);
            std::ostringstream os;
            os << (msg.u.fast_clock_flg ? 1 : 0) << "\n";
            sockSend(os.str());
            cmd_error = false;
        } else if (line.find("SET FAST CLOCK") == 0) { // FASTCLOCK
            std::string::size_type pos = line.find_first_of("0123456789");
            if (pos != std::string::npos) {
                std::istringstream s(line.substr(pos));
                int flg = 0;
                s >> flg;
                msg.id = FASTCLOCK;
                msg.u.fast_clock_flg = flg;
                sendToRT(msg);
                cmd_error = false;
            }
#endif
        } // end if to compare line to cmds
    } // end if client_ver
    if (cmd_error) {
      if (client_ver)
        sockSend("ERROR\n"); 
      else
        sockSend("ERROR - please send CLIENTVERSION!\n");
    } else {
      sockSend("OK\n"); 
    }

  } // end while
    
  Log() << "Connection to host " << remoteHost << " ended after " << connectionTimer.elapsed() << " seconds." << std::endl; 
    

  shutdown(sock, SHUT_RDWR);
  closesocket(sock);
  sock = -1;
  thread_running = false;
  Log() << " thread exit." << std::endl; 
  return 0;
}

void ConnectionThread::sendToRT(ShmMsg & msg) // note param name masks class member
{
  MutexLocker locker(fsms[fsm_id].msgFifoLock);
  
  if (shm->magic != FSM_SHM_MAGIC)
    throw FatalException("ARGH! The rt-shm was cleared from underneath us!  Did we lose the kernel module?");

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
  const char * const charbuf = static_cast<const char *>(buf);
  if (!is_binary) {
    std::stringstream ss;
    ss << "Sending: " << charbuf; 
    if (charbuf[len-1] != '\n') ss << std::endl;
    Log() << ss.str() << std::flush; 
  } else {
    Log() << "Sending binary data of length " << len << std::endl;   
  }
  int ret = send(sock, charbuf, len, flags);
  if (ret < 0) {
    Log() << "ERROR returned from send: " << GetLastNetErrStr() << std::endl; 
  } else if (ret != (int)len) {
    Log() << "::send() returned the wrong size; expected " << len << " got " << ret << std::endl; 
  }
  return ret;
}

static const char *strFind(const char *buf, long len, char c)
{
    for (long pos = 0; pos < len; ++pos) 
        if (buf[pos] == c) return buf+pos;
    return 0;
}

// Note: trims trailing whitespace!  If string is empty, connection error!
std::string ConnectionThread::sockReceiveLine(bool suppressLineLog)
{
  int ret;
  std::string rets = "";
  const char *nlpos = 0;
  // keep looping until we fill the buffer, or we get a \n
  // eg: slen < MAXLINE and (if nread then buf[slen-1] must not equal \n)
  while ( !(nlpos = strFind(sockbuf, sockbuf_len, '\n')) && sockbuf_len < (MAX_LINE-1) ) {
    ret = recv(sock, sockbuf+sockbuf_len, (MAX_LINE-1)-sockbuf_len, 0);
    if (ret <= 0) break;
    sockbuf_len += ret;
    sockbuf[sockbuf_len] = 0; // add NUL, nust to be paranoid
  }
  if (!nlpos) {
      Log() << "WARNING! RAN OUT OF BUFFER SPACE! READING PARTIAL LINE!\n";
      nlpos = sockbuf + sockbuf_len - 1;
  }
  long num = nlpos-sockbuf+1;
  rets.assign(sockbuf, num); // assign to string
  if (num < (long)sockbuf_len)  // now deal with cruft left over in the sockbuf.. move everything over
    memmove(sockbuf, nlpos+1, sockbuf_len-num);
  sockbuf_len -= num;
  // now, trim trailing spaces
  while(rets.length() && isspace(*rets.rbegin())) 
      rets.erase(rets.length()-1, 1); 
  
  if (!suppressLineLog)
    Log() << "Got: " << rets << std::endl; 
  return rets;
}


int ConnectionThread::sockReceiveData(void *buf, int size, bool is_binary)
{
  int nread = 0, n2cpy = MIN(size, ((int)sockbuf_len));
  
  // first flush out our line-read buffer since it may have data we were interested in
  if (n2cpy) {
    memcpy(buf, sockbuf, n2cpy);
    memmove(sockbuf, sockbuf+n2cpy, sockbuf_len-n2cpy);
    sockbuf_len -= n2cpy;
    nread += n2cpy;
  }
  
  while (nread < size) {
    int ret = recv(sock, (char *)(buf) + nread, size - nread, 0);
    
    if (ret < 0) {
      Log() << "ERROR returned from recv: " << GetLastNetErrStr() << std::endl; 
      return ret;
    } else if (ret == 0) {
      Log() << "ERROR in recv, connection probably closed." << std::endl; 
      return ret;
    } 
    nread += ret;
    //if (!is_binary) break;
  }

  if (!is_binary) {
    char *charbuf = static_cast<char *>(buf);
    charbuf[size-1] = 0;
    Log() << "Got: " << charbuf << std::endl; 
  } else {
    Log() << "Got: " << nread << " bytes." << std::endl; 
    if (nread != size) {
      Log() << "INFO recv() returned the wrong size; expected " << size << " got " << nread << std::endl; 
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
                                  const std::string & threshfunc,
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
  memset(&msg.u.fsm, 0, sizeof(msg.u.fsm)); // zero memory since some code assumes unset values are zero
  // setup matrix here..
  msg.u.fsm.n_rows = m.rows(); // note this will get set to inpRow later in this function via the alias nRows...
  msg.u.fsm.n_cols = m.cols();

  // tell it this is an embedded C fsm
  msg.u.fsm.type = FSM_TYPE_EMBC;
  
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
    msg.u.fsm.routing.sched_wave_dout[w.id] = w.dio_line;
    msg.u.fsm.routing.sched_wave_extout[w.id] = w.ext_trig;
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
      regex_t isNumericRE;
      
      if ( regcomp(&isNumericRE, "^([-+]?[[:digit:]]+)|([-+]?[0-9]*\\.?[0-9]+([eE][-+]?[0-9]+)?)$", REG_EXTENDED) ) {
          throw FatalException("INTERNAL ERROR: regcomp() returned failure status!  Argh!");
      }
      std::ostringstream numArrayStrm;
      // as an optimization to keep the switch/case function below smaller, first build a table of the *NUMERIC* state matrix for cells that are numeric (should be the common case)
      numArrayStrm << "unsigned long __embc_state_matrix[" << m.rows() << "][" << m.cols() << "] = {\n";
      std::list<std::pair<unsigned, unsigned> > nonNumerics;
      unsigned numericsCt = 0;
      for (unsigned r = 0; r < m.rows(); ++r) {
          numArrayStrm << "{ ";
          for (unsigned c = 0; c < m.cols(); ++c) {
              std::string thisCell = trimWS(m.at(r,c));
              if (!thisCell.length()) thisCell = "0";
              if ( 0 == regexec(&isNumericRE, thisCell.c_str(), 0, 0, 0) ) {
                  if (c == TIMEOUT_TIME_COL(&msg.u.fsm)) {
                      // timeout times are in microseconds, 
                      // so transform number to micros
                      // note we must do this here because there is a 
                      // bug in tcc on windows!
                      std::istringstream is(thisCell);
                      double d = 0.;
                      is >> d;
                      d *= 1e6;
                      std::ostringstream os;
                      os << static_cast<unsigned long>(d);
                      thisCell = os.str();
                  }
                  numArrayStrm << thisCell << ", ";
                  ++numericsCt;
              } else {
                  numArrayStrm << "0/*C Expr Omitted*/, ";
                  nonNumerics.push_back(std::pair<unsigned, unsigned>(r, c));
              }
          }
          numArrayStrm << "},\n";
      }
      numArrayStrm << "};\n\n";
      if (numericsCt) {
          prog << numArrayStrm.str();
      }
      // next, for non-numeric cells (ones containing bona-fide C-expressions) build a function with a big switch/case on the row/cols containing the c-expressions for each case..
      prog << 
        "unsigned long __embc_fsm_get_at(ushort __r, ushort __c)\n" 
        "{\n" 
        "  if (__r >= " << m.rows() << " || __c >= " << m.cols() << ") {\n"
        "    printf(\"FSM Runtime Error: no such state,column found (%hu, %hu) at time %d.%d!\\n\", __r, __c, (int)time(), (int)((time()-(double)((int)time())) * 10000));\n"
        "    return 0;\n"
        "  }\n" // end if
        "  switch(__r) {\n";
      unsigned last_r = 0xffffffff;
      for (std::list<std::pair<unsigned, unsigned> >::const_iterator it = nonNumerics.begin(); it != nonNumerics.end(); ) {
          const unsigned r = (*it).first, c = (*it).second;
          if (last_r != r) {
              prog <<
                  "  case " << r << ":\n"
                  "    switch(__c) {\n";
          }
          const std::string & thisCell = m.at(r,c);
          prog << 
              "    case " << c << ":\n"
              "      return ({ " << thisCell << "; })";
              // make sure the timeout column is timeout_us and not timeout_s!
          if (c == TIMEOUT_TIME_COL(&msg.u.fsm)) prog << " * 1e6 /* timeout secs to usecs */";
          prog << ";\n";
          ++it;
          if (it == nonNumerics.end() || (*it).first != r) { // test if the state (row) ended, if so, close the switch(__c)...
              prog << 
                  "    }\n" // end switch(__c)
                  "    break;\n"; // this is needed to prevent fall-thru to next state and instead do retun 0; below..              
          }
          last_r = r;
      }
      prog <<
          "  } // end switch(__r)\n"; // end switch(__r);
      if (numericsCt) {
          prog <<  
              "  return __embc_state_matrix[__r][__c]; /* reached for states that have numeric values rather than C expressions (should be the common case actually) */\n";
      } else {
          prog << "  return 0; /* uncommon.. all states are non-numeric so this should never be reached! */\n";
      }
      prog <<  "}\n\n"; // end function
      regfree(&isNumericRE);
    }
  
    // build the __fsm_do_state_entry and __finChanTypesm_do_state_exit functions.. 
    IntStringMap::const_iterator it_f, it_c;
    // __fsm_do_state_entry
    prog << 
      "void __embc_fsm_do_state_entry(ushort __s)\n" 
      "{\n"
      "  if ( __s >= " << m.rows() << " ) {\n";
    prog << 
      "    printf(\"FSM Runtime Error: no such state found (%hu) at time %d.%d!\\n\", __s, (int)time(), (int)((time()-(double)((int)time())) * 10000));\n"
      "  }\n\n"
            
      "  switch(__s) {\n";
    
    for (unsigned s = 0; s < m.rows(); ++s) {
      it_f = entryfmap.find(s);
      it_c = entrycmap.find(s);
      if (it_f == entryfmap.end() && it_c == entrycmap.end()) 
          continue; // optimize for common case to reduce code size -- no entry function or code..
      prog << "  case " << s << ":\n";
      if ( it_f != entryfmap.end() ) 
        prog << "  " << it_f->second << "();\n"; // call the entry function that was specified    
      if ( it_c != entrycmap.end() ) 
        prog << "  {\n" << it_c->second << "\n}\n"; // embed the entry code that was specified
      prog << "  break;\n";    
    }
    prog << "  } // end switch\n";
    prog << "} // end function\n\n";
    // __fsm_do_state_exit
    prog << 
      "void __embc_fsm_do_state_exit(ushort __s)\n" 
      "{\n"
      "  if ( __s >= " << m.rows() << " ) {\n";
    prog << 
      "    printf(\"FSM Runtime Error: no such state found (%hu) at time %d.%d!\\n\", __s, (int)time(), (int)((time()-(double)((int)time())) * 10000));\n"
      "  }\n\n"
            
      "  switch(__s) {\n";
    
    for (unsigned s = 0; s < m.rows(); ++s) {
      it_f = exitfmap.find(s);
      it_c = exitcmap.find(s);
      if (it_f == exitfmap.end() && it_c == exitcmap.end()) 
          continue; // optimize for common case to reduce code size -- no exit function or code..
      prog << "  case " << s << ":\n";
      if ( it_f != exitfmap.end() ) 
        prog << "  " << it_f->second << "();\n"; // call the exit function that was specified    
      if ( it_c != exitcmap.end() ) 
        prog << "  {\n" << it_c->second << "\n}\n"; // embed the exit code that was specified
      prog << "  break;\n";    
    }
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
    prog << "TRISTATE (*__embc_threshold_detect)(int,double) = ";
    if (threshfunc.length())  prog << threshfunc << ";\n";
    else prog << "0;\n";

    fsm_prog = prog.str();
  }
  
  //std::cerr << "---\n" << fsm_prog << "---\n"; sleep(1);// DEBUG

  // compress the fsm program text, put it in msg.u.fsm.program.program_z
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
  memcpy(msg.u.fsm.program.program_z, defBuf.get(), defSz);
  msg.u.fsm.program.program_len = sz;
  msg.u.fsm.program.program_z_len = defSz;
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
  memcpy(msg.u.fsm.program.matrix_z, defBuf.get(), defSz);
  freeDHBuf(defBuf.release());
  msg.u.fsm.program.matrix_len = sz;
  msg.u.fsm.program.matrix_z_len = defSz;
  strncpy(msg.u.fsm.name, fsm_shm_name.c_str(), sizeof(msg.u.fsm.name));
  msg.u.fsm.name[sizeof(msg.u.fsm.name)-1] = 0;
  msg.u.fsm.wait_for_jump_to_state_0_to_swap_fsm = state0_fsm_swap_flg;
  if (!doCompileLoadProgram(fsm_shm_name, fsm_prog)) return false;
  sendToRT(msg);
  return true;
}

bool ConnectionThread::doCompileLoadProgram(const std::string & prog_name, const std::string & prog_text) const
{
  bool ret = true;
  MutexLocker ml(compileLock); // make sure only 1 thread at a time compiles because sometimes we use the *same* tmp dir for all compiles..

  if (!compileProgram(prog_name, prog_text)) {
    Log() << "Error compiling the program.\n"; 
    ret = false;
  } else if (!loadModule(prog_name)) {
    Log() << "Error loading the module.\n"; 
    ret = false;
  }
  if (debug)
#ifndef EMULATOR
    Log() << "DEBUG MODE, build dir '" << TmpPath() << prog_name << ".build' not unlinked.\n";
#else
    Log() << "DEBUG MODE, build dir '" << TmpPath() << prog_name << ".c' not unlinked.\n";
#endif
  else
    unlinkModule(prog_name);  
  return ret;
}


#ifdef EMULATOR
#if defined(OS_LINUX) || defined(OS_OSX) || defined(OS_WINDOWS)
bool ConnectionThread::compileProgram(const std::string & prog_name, const std::string & prog_text) const
{
    std::string fname = TmpPath() + prog_name + ".c", 
#ifdef OS_WINDOWS
        soname = TmpPath() + prog_name + ".dll", 
#else
        soname = TmpPath() + prog_name + ".so", 
#endif
        wname = "embc_so_wrapper.c";
    std::ofstream of(fname.c_str(), std::ios::out|std::ios::trunc);
    
    of.write(prog_text.c_str(), prog_text.length());
    of.close();
#if defined(OS_WINDOWS)
    struct stat st;
    if (!::stat((std::string("../")+wname).c_str(), &st))
        wname = std::string("../")+wname;
    return System(std::string("tcc\\tcc -DOS_WINDOWS -DNEED_INT64 -DUNDEF_INT64 -W -O2 -shared -rdynamic -I. -I ../../include -o \"") + soname + "\" \"" + fname + "\" \"" + wname + "\"");
#elif defined(OS_LINUX) 
    return System(std::string("gcc -DOS_LINUX -W -O2 -shared -fPIC -export-dynamic -I../include  -o '") + soname + "' '" + fname + "' '" + wname + "'");
#elif defined(OS_OSX)
    std::string additional_includes = "";
    if (FileExists(std::string("FSMEmulator.app/Contents/Resources/") + wname)) {
        wname = std::string("FSMEmulator.app/Contents/Resources/") + wname;
        additional_includes = "-I FSMEmulator.app/Contents/Resources";
    }
                            
    return System(std::string("gcc -DOS_OSX -W -dynamiclib -fPIC -I../include -I. -I../../include " + additional_includes + " -o '") + soname + "' '" + fname + "' '" + wname + "'");
#endif
}

bool ConnectionThread::unlinkModule(const std::string & program_name) const
{
    std::string fname = TmpPath() + program_name + ".c";
    return System("rm -fr '" + fname + "'");
}

bool ConnectionThread::loadModule(const std::string & program_name) const
{
    (void)program_name;
    /*std::string modname = TmpPath() + program_name + ".so";*/
    /* NOOP for emulator */
  return true;
}
#elif defined(OS_WINDOWS)
#error doCompileLoadProgram needs to be implemented for Windows!
#endif
#else               
bool ConnectionThread::compileProgram(const std::string & fsm_name, const std::string & program_text) const
{
  bool ret = false;
  double t0 = Timer::now();
  if ( IsKernel24() ) { // Kernel 2.4 mechanism 
    std::string fname = TmpPath() + fsm_name + ".c", objname = TmpPath() + fsm_name + ".o", modname = TmpPath() + fsm_name;
    std::ofstream outfile(fname.c_str());
    outfile << program_text;
    outfile.close();
    if ( System(CompilerPath() + " -I'" + IncludePath() + "' -c -o '" + objname + "' '" + fname + "'") ) 
      ret = System(LdPath() + " -r -o '" + modname + "' '" + objname + "' '" + ModWrapperPath() + "'");
  } else if ( IsKernel26() ) { // Kernel 2.6 mechanism
      if (!IsFastEmbCBuilds()) { // traditional EmbC build scheme

          std::string 
              buildDir = TmpPath() + fsm_name + ".build", 
              fname = buildDir + "/" + fsm_name + "_generated.c";
          if (System(std::string("mkdir -p '") + buildDir + "'")) {
              std::ofstream outfile(fname.c_str());
              outfile << program_text;
              outfile.close();
              ret = System(std::string("cp -f '") + ModWrapperPath() + "' '" + buildDir + "' && cp -f '" + MakefileTemplatePath() + "' '" + buildDir + "/Makefile' && make -C '" + buildDir + "' TARGET='" + fsm_name + "' EXTRA_INCL='" + IncludePath() + "'" + ( CompilerPath().length() ? (std::string(" COMPILER_OVERRIDE='") + CompilerPath() + "'") : "" ) );
          }

      } else { // faster embc build scheme that avoids the kernel makefiles
               // by precaching partially built kernel objects..

          std::string 
              buildDir = EmbCBuildPath(), 
              fname = buildDir + "/mod_impl.c", 
              cpSwitches = debug ? "fpvra" : "fpra";
          std::ofstream outfile(fname.c_str(), std::ios::out|std::ios::trunc);
          outfile << program_text;
          outfile.close();
          // copy makefile to build dir, use sed to replace instances of KBUILD_MODNAME=KBUILD_STR(mod) with KBUILD_MODNAME=KBUILD_STR($fsm_name) in the .cmd files, run make in the build dir
          ret = System(std::string("for f in '") + buildDir + "'/.*.cmd; do cat \"$f\" | sed '{s/KBUILD_MODNAME=KBUILD_STR(.*)/KBUILD_MODNAME=KBUILD_STR(" + fsm_name + ")/g}' > \"$f\".tmp && mv -f \"$f\".tmp \"$f\"; done && rm -f '" + buildDir + "'/mod_impl.o && make -C '" + buildDir + "' -f '" + Makefile() + "' mod.ko");

      }
  } else {
    Log() << "ERROR: Unsupported Kernel version in ConnectionThread::compileProgram()!";
  }
  if (ret) 
      Log() << "Compile time: " << (Timer::now()-t0) << "s\n";
  
  return ret;
}

bool ConnectionThread::loadModule(const std::string & program_name) const
{
  std::string modname;
  if ( IsKernel24() ) 
    modname = TmpPath() + program_name;
  else if ( IsKernel26() ) {
      if (IsFastEmbCBuilds()) 
          modname = EmbCBuildPath() + "/mod.ko";
      else
          modname = TmpPath() + program_name + ".build/" + program_name + ".ko";
  } else {
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
    ret |= remove(modname.c_str());
    ret |= remove(cfile.c_str());
    ret |= remove(objfile.c_str());
    return ret == 0;
  } else if ( IsKernel26() ) {
      if (IsFastEmbCBuilds()) {
          System("rm -f '" + EmbCBuildPath() + "'/mod.ko '" + EmbCBuildPath() + "'/mod_impl.o");
      } else {
          std::string buildDir = TmpPath() + program_name + ".build"; 
          System("rm -fr '" + buildDir + "'");
      }
  } else
    Log() << "ERROR: Unsupported Kernel version in ConnectionThread::unlinkModule()!";
  return false;
}
#endif

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
  memset(&msg.u.fsm, 0, sizeof(msg.u.fsm)); // zero memory since some code assumes unset values are zero
  
  // setup matrix here..
  msg.u.fsm.n_rows = m.rows(); // note this will get set to inpRow later in this function via the alias nRows...
  msg.u.fsm.n_cols = m.cols();
  msg.u.fsm.type = FSM_TYPE_PLAIN;
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
  int swCells = numSchedWaves * fsms[fsm_id].dio_scheduled_wave_num_columns; // each sched wave uses dio_scheduled_wave_num_columns cells.
  int swRows  = (swCells / m.cols()) + (swCells % m.cols() ? 1 : 0);  /* total number of rows used in sched wave def */
  int swFirstRow  = (int)nRows - swRows;                    /* matrix nRows is:
							       states; one row of input spec ; swRows */
  int inpRow = swFirstRow - 1;                                        /* the row with the input spec */
  nRows = inpRow;                                                     /* nRows is now number of states */
  Log() << "I believe this state matrix has " << nRows << " rows corresponding to states and " << swRows << " corresponding to scheduled waves " << std::endl;
  if ( nRows > MAX_STATES ) {
    Log() << "Matrix has " << nRows << " rows but I can only take a max of " << MAX_STATES << std::endl;
    return false;
  }
  msg.u.fsm.plain.numStates = nRows;
  if (inpRow < 0 || swFirstRow < 0 || inpRow < 0) {
    Log() << "Matrix specification has invalid number of rows! Error!" << std::endl; 
    return false;            
  }

  // ----- Read the input routing row of the matrix ----------
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
  
  // ----- Read the sched waves spec ----------
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
    msg.u.fsm.routing.sched_wave_dout[id] = dio_line;
    NEXT_COL();
    int ext_out = (int)m.at(row, col);
    if (ext_out < 0) ext_out = 0;
    msg.u.fsm.routing.sched_wave_extout[id] = ext_out;
    NEXT_COL();
    w.preamble_us = static_cast<unsigned>(m.at(row,col)*1e6);
    NEXT_COL();
    w.sustain_us = static_cast<unsigned>(m.at(row,col)*1e6);
    NEXT_COL();
    w.refraction_us = static_cast<unsigned>(m.at(row,col)*1e6);
    if (fsms[fsm_id].dio_scheduled_wave_num_columns > 8) {
      /* If there is a ninth column, it is always the loop parameter */
      NEXT_COL();
      w.loop = (int)m.at(row, col);
    } else {
      w.loop = 0;
    }
    if (fsms[fsm_id].dio_scheduled_wave_num_columns > 9) {
      /* If there are a 10th and an 11th column, 
	 they are always the waves to trigger on up and
	 the waves to UNtrigger on down. */
      NEXT_COL();
      w.sw_trigger_on_up     = (unsigned int)m.at(row, col);
    } else {
      w.sw_trigger_on_up = 0; 
    } if (fsms[fsm_id].dio_scheduled_wave_num_columns > 10) {
      /* Now the 11th column if it is there */
      NEXT_COL();
      w.sw_untrigger_on_down = (unsigned int)m.at(row, col);
    } else {
      w.sw_untrigger_on_down = 0;
    }
    w.enabled = true;
    if (debug) 
      Log() << "Sched wave " << id << " says preamble=" << w.preamble_us << " sustain=" << w.sustain_us << " refraction=" << w.refraction_us << " loop=" << w.loop << "\n";
  }
#undef NEXT_COL


  // make sure it fits
  {
      unsigned long inBytes = nRows * m.cols() * sizeof(msg.u.fsm.plain.matrix[0]),
                    maxBytes = sizeof(msg.u.fsm.plain.matrix);
    if ( inBytes > maxBytes ) {
        Log() << "The specified matrix is too big!  It takes up " << inBytes << " bytes when the max capacity for a plain matrix is " << maxBytes << " bytes!  Try either an FSM program, or reduce the size of the matrix!\n"; 
        return false;
    }
  }
  // next, just assign it into msg.u.fsm.plain.matrix
  for (i = 0; i < (int)nRows; ++i) {
    for (j = 0; j < (int)m.cols(); ++j) {
      double val = m.at(i, j);
      if (j == (int)TIMEOUT_TIME_COL(&msg.u.fsm)) val *= 1e6;
      // NB: this cast to int then to unsigned is required because on OSX
      // for some reason casting from a negative double to unsigned always
      // returns 0, whereas castin to int then unsigned does not
      FSM_MATRIX_AT(&msg.u.fsm, i, j) = static_cast<unsigned>(static_cast<int>(val));
    }
  }

  msg.u.fsm.wait_for_jump_to_state_0_to_swap_fsm = state0_fsm_swap_flg;

  std::string fsm_shm_name = newShmName(); // the name is not really for a shm, just descriptive/unique
  strncpy(msg.u.fsm.name, fsm_shm_name.c_str(), sizeof(msg.u.fsm.name));
  msg.u.fsm.name[sizeof(msg.u.fsm.name)-1] = 0;

  return true;
}

StringMatrix ConnectionThread::matrixFromRT()
{

  msg.id = GETVALID;

  sendToRT(msg);

  if (!msg.u.is_valid/* NB: the check of this flag is a hack!  this works becuse the msg.u.is_valid flag takes up the same memory as the msg.u.fsm.n_rows and msg.u.fsm.n_cols fields due to the union!*/) {
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
  
  switch (msg.u.fsm.type) {
  case FSM_TYPE_EMBC: {  
        std::auto_ptr<char> strBuf(inflateCpy(msg.u.fsm.program.matrix_z, msg.u.fsm.program.matrix_z_len, msg.u.fsm.program.matrix_len, 0));
  
        parseStringTable(strBuf.get(), m);
        freeDHBuf(strBuf.release());
    } 
    break;
  case FSM_TYPE_PLAIN: {
        for (unsigned r = 0; r < m.rows(); ++r) 
        for (unsigned c = 0; c < m.cols(); ++c) {
            std::ostringstream os;
            unsigned val = FSM_MATRIX_AT(&msg.u.fsm, r, c);
            if (c == TIMEOUT_TIME_COL(&msg.u.fsm)) os << (double(val) / 1e6);
            else os << val;
            m.at(r,c) = os.str(); 
        }
    }
    break;
  default: {
        const std::string zero = "0";
        Log() << "INVALID MATRIX TYPE! Expected one of 'FSM_TYPE_PLAIN' or 'FSM_TYPE_EMBC' -- sending empty matrix\n";
        for (unsigned r = 0; r < m.rows(); ++r) 
        for (unsigned c = 0; c < m.cols(); ++c) 
            m.at(r,c) = zero;
        return m;
    }
    break;
  } // end switch
  
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
#ifdef OS_WINDOWS
    unsigned long nready;
    ioctlsocket(sock, FIONREAD, &nready);
#else
    int nready;
    ioctl(sock, FIONREAD, &nready);
#endif
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
      gettimeofday(&now, NULL);
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
      unsigned char type = nrt->type&(~NRT_BINDATA), isbin = nrt->type&(NRT_BINDATA);
      switch (type) {
      case NRT_TCP: 
        doNRT_IP(nrt.get(), false, isbin);
        break;
      case NRT_UDP: 
        doNRT_IP(nrt.get(), true, isbin);
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

void FSMSpecific::doNRT_IP(const NRTOutput *nrt, bool isUDP, bool isBin) const
{  
        struct hostent he, *he_result;
        int h_err;
        char hostEntAux[32768];
        unsigned datalen = 0;
        const void *data = 0;
        const char *datachar = 0;
        std::string packetText;
        
        if (!isBin) {
            packetText = FormatPacketText(nrt->ip_packet_fmt, nrt);
            data = datachar = packetText.c_str();
            datalen = packetText.length();
#ifdef EMULATOR
            Log() << "Sending to " << nrt->ip_host << ":" << nrt->ip_port << " data: '" << packetText << "'\n";
#endif
            
        } else {
            data = datachar = (const char *)nrt->data;
            datalen = nrt->datalen;
#ifdef EMULATOR
            Log() << "Sending to " << nrt->ip_host << ":" << nrt->ip_port << " binary data of length " << datalen << "\n";
#endif
        }
        int ret = gethostbyname2_r(nrt->ip_host, AF_INET, &he, hostEntAux, sizeof(hostEntAux),&he_result, &h_err);

        if (ret) {
          Log() << "ERROR In doNRT_IP() got error (ret=" << ret << ") in hostname lookup for " << nrt->ip_host << ": h_errno=" << h_err << "\n"; 
          return;
        }
        int theSock;
        if (isUDP) theSock = socket(PF_INET, SOCK_DGRAM, 0);
        else theSock = socket(PF_INET, SOCK_STREAM, 0);
        if (theSock < 0) {
          Log() << "ERROR In doNRT_IP() got error (errno=" << GetLastNetErrStr() << ") in socket() call\n"; 
          return; 
        }
        if (!isUDP) {
          long flag = 1;
          const char * const flagptr = (const char *)&flag;
          setsockopt(theSock, SOL_SOCKET, SO_REUSEADDR, flagptr, sizeof(flag));
          flag = 1;
          // turn off nagle for less latency
          setsockopt(theSock, IPPROTO_TCP, TCP_NODELAY, flagptr, sizeof(flag));
        }
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(nrt->ip_port);
        memcpy(&addr.sin_addr.s_addr, he.h_addr, sizeof(addr.sin_addr.s_addr));
        ret = connect(theSock, (struct sockaddr *)&addr, sizeof(addr)); // this call is ok for UDP as well
        if (ret) {
          Log() << "ERROR In doNRT_IP() could not connect to " << nrt->ip_host << ":" << nrt->ip_port << " got error (errno=" << GetLastNetErrStr() << ") in connect() call\n"; 
          closesocket(theSock);
          return;
        }
        if (isUDP)
          // UDP
          ret = sendto(theSock, datachar, datalen, 0, (const struct sockaddr *)&addr, sizeof(addr));
        else
          // TCP
          ret = send(theSock, datachar, datalen, MSG_NOSIGNAL);
        if (ret != (int)datalen) {
          Log() << "ERROR In doNRT_IP() sending " << datalen << " bytes to " << nrt->ip_host << ":" << nrt->ip_port << " got error (errno=" << GetLastNetErrStr() << ") in send() call\n"; 
        }
        closesocket(theSock);
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

  // format of sched wave string is \1ID\2IN_EVT_COL\2OUT_EVT_COL\2DIO_LINE\2SOUND_TRIG\2PREAMBLE_SECS\2SUSTAIN_SECS\2REFRACTION_SECS\2LOOP\2... so split on \1 and within that, on \2

  std::vector<SchedWaveSpec> ret;
  std::set<int> ids_seen;
  // first split on \1
  std::vector<std::string> waveSpecStrs = splitString(str, "\1"), flds;
  std::vector<std::string>::const_iterator it;
  for (it = waveSpecStrs.begin(); it != waveSpecStrs.end(); ++it) {
    flds = splitString(*it, "\2");
    if (flds.size() != fsms[fsm_id].dio_scheduled_wave_num_columns) {
      Log() << "Parse error for sched waves spec \"" << *it << "\" (ignoring! Argh!)\n"; 
      Log() << "Didn't find " << fsms[fsm_id].dio_scheduled_wave_num_columns << " fields for my sched wave!! Spec \"" << *it << "\" Argh!\n";
      continue;      
    }
    struct SchedWaveSpec spec;
    bool ok;
    int i = 0;
    spec.id = FromString<int>(flds[i++], &ok);
    if (ok) spec.in_evt_col = FromString<int>(flds[i++], &ok);
    if (ok) spec.out_evt_col = FromString<int>(flds[i++], &ok);
    if (ok) spec.dio_line = FromString<int>(flds[i++], &ok);
    if (ok) spec.ext_trig = FromString<int>(flds[i++], &ok);
    if (ok) spec.preamble_us = static_cast<unsigned>(FromString<double>(flds[i++], &ok) * 1e6);
    if (ok) spec.sustain_us = static_cast<unsigned>(FromString<double>(flds[i++], &ok) * 1e6);
    if (ok) spec.refraction_us = static_cast<unsigned>(FromString<double>(flds[i++], &ok) * 1e6);
    if (fsms[fsm_id].dio_scheduled_wave_num_columns>8) {
      if (ok) spec.loop = FromString<int>(flds[i++], &ok);
    } else {
      spec.loop = 0;
    }
    if (!ok) {
      Log() << "parsing sched wave " << spec.id << "broke\n";
    } else {
      Log() << "FSMServer: parsing got wave " << spec.id << " : " << spec.in_evt_col << " : " << spec.out_evt_col << " : " << spec.ext_trig;
      Log() << " : " << spec.preamble_us <<  " : " << spec.sustain_us <<  " : " << spec.refraction_us <<  " : " << spec.loop <<  "\n";
    }     
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
  std::vector<std::string> nv_pairs = splitString(strblk, ",");
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
  std::string globals, initfunc, cleanupfunc, transitionfunc, tickfunc, inChanType, threshfunc;
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
      else if (section == "THRESHFUNC") {
        threshfunc = UrlDecode(block);
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
  {
    std::istringstream ss(line);
    ss >> rows >> cols;
  }
  unsigned ct;
  if (!debug) 
      Log() << "(Receiving state program, one cell per line, output suppressed to prevent screen flood)\n";
  {
    std::ostringstream ss;
    for (ct = 0;  ct < rows*cols; ++ct) 
      ss << sockReceiveLine(!debug);
    block = ss.str();
  }
  StringMatrix m(rows, cols);
  parseStringTable(block.c_str(), m);
  return matrixToRT(m, globals, initfunc, cleanupfunc, transitionfunc, tickfunc, threshfunc, entryfuncs, exitfuncs, entrycodes, exitcodes, inChanType, inSpec, outSpec, swSpec, readyForTrialJumpState, state0FSMSwapFlg);  
}

std::string ConnectionThread::newShmName()
{ 
  std::ostringstream o;
  int fsmCtr;
  {
    MutexLocker ml(connectedThreadsLock);
    fsmCtr = ++shm->fsmCtr;
  }
  o << "fsm" << std::setw(3) << std::setfill('0') << (fsmCtr%1000) << "f" << fsm_id << "t" << myid << "_c" << shm_num++; 
  return o.str();
}

namespace {
  bool FileExists(const std::string & f)
  {
    struct stat st;
    return stat(f.c_str(), &st) == 0;
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
      int fd = mkstemp(tmp_filename);
      std::system((std::string("uname -r > ") + tmp_filename).c_str());
      std::ifstream inp;
      inp.open(tmp_filename);
      inp >> ret;
      inp.close();
      closesocket(fd);
      remove(tmp_filename); // we unlink after close due to wind0ze?
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

Matrix ConnectionThread::doGetTransitionsFromRT(int & first, int & last, int & state, bool old_format)
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
              if (old_format)
                mat.at(ct, 1) = t.event_id > -1 ? 0x1 << t.event_id : 0x1<<num_input_events;
              else
                mat.at(ct, 1) = t.event_id;
              mat.at(ct, 2) = static_cast<double>(t.ts/1000ULL) / 1000000.0; /* convert us to seconds */
              mat.at(ct, 3) = t.state;
              mat.at(ct, 4) = static_cast<double>(t.ext_ts/1000ULL) / 1000000.0;
            }
      }
      return mat;
  }
  return empty;
}


Matrix ConnectionThread::doGetSimInpEvtsFromRT()
{
    // query the count first to check sanity
    msg.id = GETSIMINPEVTS;
    sendToRT(msg);
    Matrix mat(msg.u.sim_inp.num, 2);
    for (unsigned i = 0; i < msg.u.sim_inp.num; ++i) {
        mat.at(i, 0) = msg.u.sim_inp.evts[i].event_id;
        mat.at(i, 1) = static_cast<double>(msg.u.sim_inp.evts[i].ts) / 1e9;
    }
    return mat;
}

Matrix ConnectionThread::doGetStimuliFromRT(int & first, int & last)
{
  static const Matrix empty(0, 4);

  // query the count first to check sanity
  msg.id = STIMULICOUNT;
  sendToRT(msg);

  int n_events = msg.u.fsm_events_count;

  if (first < 0) first = 0;
  if (last < 0) last = n_events-1;

  if (first > -1 && first <= last && last < n_events) {
      int desired = last-first+1, received = 0, ct = 0;
      Matrix mat(desired, 4);


      // keep 'downloading' the matrix from RT until we get all the transitions we require
      while (received < desired) {
            msg.id = GETSTIMULI;
            msg.u.fsm_events.num = desired - received;
            msg.u.fsm_events.from = first + received;
            sendToRT(msg);
            received += (int)msg.u.fsm_events.num;
            for (int i = 0; i < (int)msg.u.fsm_events.num; ++i, ++ct) {
              struct FSMEvent & e = msg.u.fsm_events.e[i];
              mat.at(ct, 0) = double(e.type);
              mat.at(ct, 1) = e.id;
              mat.at(ct, 2) = e.val;
              mat.at(ct, 3) = double(e.ts) / 1e9;
            }
      }
      return mat;
  }
  return empty;
}

#if defined(OS_CYGWIN) || defined(OS_OSX) || defined(OS_WINDOWS)
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
