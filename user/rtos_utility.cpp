#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(OS_WINDOWS) || defined(WIN32) || defined(WINDOWS)
#  undef OS_WINDOWS
#  undef WIN32
#  undef WINDOWS
#  define WIN32
#  define OS_WINDOWS
#  define WINDOWS
#endif
#include "rtos_utility.h"
#ifdef WIN32
#  define MAP_LOCKED 0
#  define ERRLOG "err.log"
//#  define PRTERR
#endif
#ifndef WIN32
#  include "rtos_shared_memory.h"
#  include <sys/types.h>
#  include <sys/ipc.h>
#  include <sys/types.h>
#  include <sys/shm.h>
#  include <sys/ioctl.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <sys/select.h>
#  include <pthread.h>
#  ifdef EMULATOR
#    define USE_UNIX_SOCKETS
#  endif
#endif
#include <fcntl.h> 
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <map>
#include <string>
#include <set>
#include <sstream>
#include <fstream>
#include <sys/stat.h>


#if defined(WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define mbuff_attach(n,s) (0)
#  define mbuff_detach(n,s) (0)
#  define rtai_shm_attach(n,s) (0)
#  define rtai_shm_detach(n,s) (0)
#  define RTAI_SHM_DEV ""
#  define MBUFF_DEV_NAME ""
#  define MBUFF_DEV_NAME2 ""
#elif defined(OS_LINUX)
#  include <time.h>
#endif

#ifndef MIN
#define MIN(a,b) ( (a) < (b) ? (a) : (b) )
#endif

namespace 
{
  long String2Long(const std::string & s);  
}

struct RTOS::Fifo
{
  long handle; /* EMULATOR:
                 Windows: the hHandle of the named pipe, 
                 Linux: sysv ipc msg id for a msg queue
                 Unix: the fd of the connected socket, or the listenfd if 
                       not connected yet
                 Non-EMULATOR:
                 the file descriptor of the /dev/rtfXX opened for reading or 
                 writing */
  int key;    /* the minor specified at open time */
  std::string name; /* On Windows the \\.\pipe\PIPENAME string, 
                       On UNIX: the path of the named pipe */
  unsigned size;
  volatile unsigned refct;
  volatile bool destroy, needcon;
  unsigned ctr;
#ifdef WIN32
  HANDLE thrd;
  static DWORD WINAPI winPipeListener(void *);
#endif
#ifdef USE_UNIX_SOCKETS
  pthread_t thrd;
  pthread_cond_t cond;
  pthread_mutex_t mut;
  static void *unixListenThr(void *);
  bool unixAccept();
  int unixChkCon(bool);
  void unixCreateListener();
#endif
  Fifo() : handle(-1), 
           key(-1), size(0), refct(0), destroy(false), needcon(false), ctr(0)
#ifdef WIN32
, thrd(0) 
#endif
{
#ifdef USE_UNIX_SOCKETS
    pthread_cond_init(&cond,0);
    pthread_mutex_init(&mut,0);
#endif
}
 ~Fifo() 
 {
#ifdef USE_UNIX_SOCKETS
    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&mut);
#endif
 }
};

RTOS::Fifo * const RTOS::INVALID_FIFO = 0;

namespace {
  std::set<RTOS::Fifo *> fifos;
  std::map<unsigned, RTOS::Fifo *> fifo_map; // map of key -> fifo instance
}


RTOS::RTOS RTOS::determine()
{
  RTOS rtosUsed = Unknown;
#ifndef EMULATOR
  struct stat statbuf;
  if (!stat("/proc/rtai", &statbuf)) rtosUsed = RTAI;
  else if(!stat("/proc/modules", &statbuf)) {
    std::ifstream proc_modules("/proc/modules");
    static const int BUFSZ = 256;
    char modname_buf[BUFSZ];
    char FMT[32];
    char line[BUFSZ];
    size_t lnsz = BUFSZ;

    snprintf(FMT, sizeof(FMT)-1, "%%%ds", BUFSZ);
    FMT[sizeof(FMT)-1] = 0;
    while(rtosUsed == Unknown 
          && proc_modules.getline(line, lnsz) > 0) {
      line[lnsz-1] = 0;
      // read in the modules list one at a time and see if 'rtl' is loaded
      if (sscanf(line, FMT, modname_buf)) {
        modname_buf[BUFSZ-1] = 0;
        if (!strcmp("rtl", modname_buf))
          rtosUsed = RTLinux; // bingo!
      }
    }
  }
#endif  
  return rtosUsed;
}

struct ShmInfo
{
  std::string name;
  size_t size;
  void *address;
#ifdef WIN32
  HANDLE hMapFile;
#endif
  RTOS::RTOS rtos;

  ShmInfo() : size(0), address(0), 
#ifdef WIN32
              hMapFile(0),
#endif
              rtos(RTOS::Unknown)
 {}
};

typedef std::map<unsigned long, ShmInfo> ShmMap;
static ShmMap shmMap;

void *RTOS::shmAttach(const char *SHM_NAME, size_t size, ShmStatus *s, bool create)
{
  void *ret = 0;
  RTOS rtos = determine();
  if (s) *s = Ok;
  if (rtos != Unknown) {
    ShmInfo shm_info;

    shm_info.size = size;
    shm_info.name = SHM_NAME;
    shm_info.rtos = rtos;

    switch(rtos) {
      case RTLinux:  ret = mbuff_attach(SHM_NAME, size);    break;
      case RTAI:  ret = rtai_shm_attach(SHM_NAME, size);    break;
      default:  break;
    }
    
    if (ret) {
      shm_info.address = ret;
      // save the shm's info for quick reference on detach..
      shmMap [reinterpret_cast<unsigned long>(ret)] = shm_info; 
      if (s) *s = Ok;
    } 
    
    else if (s && *s == Ok) { /* if no shm was available, 
                                 status is Ok, and  pointer is not NULL,
                                 update the ShmStatus intelligently      */
      
      if (determine() == RTLinux || determine() == RTAI) {
        if (!shmDevFileExists())
          *s = MissingDevFile;
        else if (!shmDevFileIsValid())
          *s = InvalidDevFile;
        else *s = NotFound;      
      }
    }
  } else { // no rtos, just use sysvipc, etc
    ret = 0;
#ifdef EMULATOR
    if (s) *s = NotFound; // in emulator mode the erros *shouldn't be* NoRTOS.
#else
    if (s) *s = NoRTOSFound;    
#endif
#if defined(WIN32)
  /* in cygwin we don't use sysv ipc *at all* because it requires 
     cygserver be running and it's annoying.  
     
     Instead, will go ahead and use the built-in windows 
     functions for shared memory.. */
    std::string shm_name = std::string("Global\\") + SHM_NAME;
    HANDLE hMapFile = 0;
    if (create) {
      hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE,   // use paging file
                                    NULL,       // default security attributes
                                    PAGE_READWRITE,  // read/write access
                                    0,           // size: high 32-bits
                                    size,       // size: low 32-bits
                                    shm_name.c_str()); // name of map object
      
    } else {
      hMapFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm_name.c_str());
    }
    if (hMapFile) {
      ret = (void *)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, size);
      //CloseHandle(hMapFile);
      
    }
#else
    // note we only attach to existing sysvipc shm
    // we create a new one if not exits and create flag specified
    int flags = 0777, id;
    if (create) {
      id = shmget(String2Long(SHM_NAME), 1, flags);
      if (id >= 0) {
        // delete the existing shm, if it exists
        struct shmid_ds ds; 
        shmctl(id, IPC_STAT, &ds);       
        shmctl(id, IPC_RMID, &ds);
      }
      flags |= IPC_CREAT;
    }
    id = shmget(String2Long(SHM_NAME), size, flags);    
    if (id >= 0) {
      ret = shmat(id, 0, 0);
      if (ret == (void *)-1) ret = 0;
    }
#endif
    if (ret) {
      ShmInfo & shm_info = shmMap [ reinterpret_cast<unsigned long>(ret) ];
      shm_info.size = size;
      shm_info.name = SHM_NAME;
      shm_info.rtos = rtos;
      shm_info.address = ret;
#ifdef WIN32
      shm_info.hMapFile = hMapFile;
#endif
      if (s) *s = Ok;
    }
  }
  return ret;
}

void RTOS::shmDetach(const void *SHM, ShmStatus *s, bool destroy)
{
  unsigned long shm_key = reinterpret_cast<unsigned long>(SHM);
  ShmMap::iterator it = shmMap.find(shm_key);

  if (s) *s = NotFound;

  if ( it != shmMap.end() ) {
      ShmInfo & inf = (*it).second;
      switch (inf.rtos) {
        case RTLinux:  mbuff_detach(inf.name.c_str(), inf.address); break;
        case RTAI:     rtai_shm_detach(inf.name.c_str(), inf.address); break;
        case Unknown:  
#if defined(WIN32)
          {
              //std::string shm_name = std::string("Global\\") + inf.name.c_str();

            //HANDLE hMapFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm_name.c_str());
            if (inf.hMapFile) {
              UnmapViewOfFile(inf.address);
              CloseHandle(inf.hMapFile);
            }
          }
#else
          // sysvipc case
          shmdt(inf.address); 
          if (destroy) {
            int id = shmget(String2Long(inf.name.c_str()), inf.size, 0777);
            // delete the existing shm
            struct shmid_ds ds; 
            shmctl(id, IPC_STAT, &ds);       
            shmctl(id, IPC_RMID, &ds);
          }
#endif
          break;
        default: break;
      }
      shmMap.erase(it);
      if (s) *s = Ok;
  }
}

/* true iff /dev/mbuff or /dev/rtai_shm at least exists */
bool RTOS::shmDevFileExists()
{
  struct stat statbuf;

  switch(determine()) {
    case RTLinux:
      if ( (stat (MBUFF_DEV_NAME, &statbuf) || !S_ISCHR(statbuf.st_mode))
           && (stat (MBUFF_DEV_NAME2, &statbuf) || !S_ISCHR(statbuf.st_mode)))
        return false;
      return true;
    break;
    case RTAI:
      if ( stat (RTAI_SHM_DEV, &statbuf) || !S_ISCHR(statbuf.st_mode) )
        return false;      
      return true;
    break;
    default: break;
  }
  return false;  
}

/* true iff /dev/mbuff and/or /dev/rtai_shm is valid and openable for RDWR */
bool RTOS::shmDevFileIsValid()
{
  int fd = -1;

  switch(determine()) {
    case RTLinux:
      /* since mbuff_attach isn't very good about giving us exactly what 
	 went wrong, we will attempt to figure out if it was a permissions
	 issue or some other error. */
      if ( (!shmDevFileExists()
            || (fd = open(MBUFF_DEV_NAME, O_RDWR)) < 0 || close(fd))
           && (!shmDevFileExists() 
               || (fd = open(MBUFF_DEV_NAME2, O_RDWR)) < 0 || close(fd))) 
        return false;
      return true;
    break;
    case RTAI:
      /* since rtai_shm_attach isn't very good about giving us exactly what 
	 went wrong, we will attempt to figure out if it was a permissions
	 issue or some other error. */
      if ( !shmDevFileExists()
            || (fd = open(RTAI_SHM_DEV, O_RDWR)) < 0 
            || close(fd) )
        return false;
      return true;
    break;
    default: break;
  }
  return false;
}


const char *RTOS::shmDevFile()
{
  switch(determine()) {
  case RTLinux:
    return MBUFF_DEV_NAME;
    break;
  case RTAI:
    return RTAI_SHM_DEV;
    break;
  default:
    break;
  }
  return "<no dev file>";  
}

const char *RTOS::shmDriverName()
{
  switch(determine()) {
  case RTLinux:
    return "mbuff.o";
    break;
  case RTAI:
    return "rtai_shm.o";
    break;
  default:
    break;
  }
  return "<none>";    
}

const char *RTOS::name()
{
  switch(determine()) {
  case RTLinux:
    return "RTLinux";
    break;
  case RTAI:
    return "RTAI";
    break;
  default:
    break;
  }
  return "<No RTOS>";      
}

  // /dev/rtf in RTOS mode, if emulator, \\.\pipe\fsm_pipe_no_ for win32
  // or /tmp/fsm_pipe_no_ otherwise
const char *RTOS::fifoFilePrefix(int rtos)
{
  if (rtos < 0) rtos = determine();
  if (rtos == None) {
#ifdef WIN32
  return "\\\\.\\pipe\\fsm_pipe_no_";
#else // !WIN32 -- Unix, etc
  return "/tmp/fsm_pipe_no_";
#endif
  }
  return "/dev/rtf";
}

// opens /dev/rtf[minor no] and returns its fd or 0 on error              
RTOS::FIFO RTOS::openFifo(unsigned key, ModeFlag mode, std::string *errmsg)
{
  FIFO f = 0;
#ifdef EMULATOR
  (void)mode; // avoid unused warning
  f = fifo_map[key]; 
  if (!f) {
    f = new Fifo;
    f->key = key;
  }
  f->refct++;

#  ifdef WIN32
  if (!f->name.length()) {
    std::ostringstream os;
    os << fifoFilePrefix() << key;
    f->name = os.str();
  }
  if (f->handle < 0) {
      f->handle = -1;
      f->destroy = false;
      DWORD m = 0;
      if (mode & Read) m |= GENERIC_READ;
      if (mode & Write) m |= GENERIC_WRITE;
      m = GENERIC_READ|GENERIC_WRITE;
      HANDLE h = CreateFileA( f->name.c_str(),   // pipe name 
                              m, // access mode. GENERIC_READ GENERIC_WRITE etc
                              0,              // no sharing 
                              NULL,           // default security attributes
                              OPEN_EXISTING,  // opens existing pipe 
                              0,              // default attributes 
                              NULL);          // no template file
      if (h != INVALID_HANDLE_VALUE) f->handle = (long)h;
      else {
          DWORD err = GetLastError();
#ifdef PRTERR
          FILE *fo = fopen(ERRLOG, "a");
          fprintf(fo, "(PID %d) open error on %s is %d\n", (int)GetCurrentProcessId(), f->name.c_str(), err);
          fclose(fo);
#endif
          if (errmsg) {
              std::ostringstream os;
              os << "(PID " << GetCurrentProcessId() << ") open error on " << f->name << " is " << err;
              *errmsg = os.str();
          }
      }
  }
#  elif defined(USE_UNIX_SOCKETS)
  if (!f->name.length()) {
      std::ostringstream os;
      os << fifoFilePrefix() << key;
      f->name = os.str();
  }
  if (f->handle < 0) {
      f->destroy = false;
      f->handle = socket(PF_UNIX, SOCK_STREAM, 0);
      if (f->handle < 0) {
          int err = errno;
          perror("socket");
          if (errmsg) *errmsg = strerror(err);          
      } else {
          struct sockaddr_un addr;
          addr.sun_family = AF_UNIX;
          snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", f->name.c_str());
          if (connect(f->handle, (struct sockaddr *)&addr, sizeof(addr))) {
              int err = errno;
              perror("connect");
              close(f->handle);
              f->handle = -1;
              if (errmsg) *errmsg = strerror(err);
          }
      }
  }
#  endif  

  if (f->handle == -1) {
    closeFifo(f);
    return INVALID_FIFO;
  }

#else /* !EMULATOR, regular Linux */
  char buf[64];

  snprintf(buf, 63, "%s%d", fifoFilePrefix(), key);
  buf[63] = 0;
  int m = mode == Read ? O_RDONLY : (mode == Write ? O_WRONLY : O_RDWR);
  int ret = ::open(buf, m);
  if (ret < 0) {
      if (errmsg) *errmsg = strerror(errno);
      return INVALID_FIFO;
  }
  f = new Fifo;
  f->handle = ret;
  f->key = key;
  f->size = 0;
  f->refct = 1;
  f->name = buf;  
#endif
  if (f) { fifos.insert(f); fifo_map[key] = f; }
  return f;
}

#ifdef WIN32
DWORD WINAPI RTOS::Fifo::winPipeListener(void *arg)
{
    RTOS::Fifo *f = reinterpret_cast<RTOS::Fifo *>(arg);
    HANDLE h = (HANDLE)f->handle;
    bool ok = true;
    DWORD err;
    BOOL res = ConnectNamedPipe(h, 0);
    err = GetLastError();
    ok = res || (!res && err == ERROR_PIPE_CONNECTED);
    /*        } while (ok);*/
    if (!ok) {
#ifdef PRTERR
        FILE *fo = fopen(ERRLOG, "a");
        fprintf(fo, "(PID %d) ConnectNamedPipe err: %d\n", (int)GetCurrentProcessId(), (int)err);
        fclose(fo);
#endif
    } else {
        f->needcon = false;
        f->destroy = true;
#ifdef PRTERR
        FILE *fo = fopen(ERRLOG, "a");
        fprintf(fo, "(PID %d) ConnectNamedPipe received connection on %s\n", (int)GetCurrentProcessId(), f->name.c_str());
        fclose(fo);
#endif
    }
    f->thrd = 0;
    return 0;
}
#endif

#ifdef USE_UNIX_SOCKETS

void RTOS::Fifo::unixCreateListener()
{
    destroy = true;
    needcon = true;
    int ret = pthread_create(&thrd, 0, unixListenThr, this);
    if ( ret ) fprintf(stderr, "pthread_create: %s\n", strerror(ret));    
}

void *RTOS::Fifo::unixListenThr(void *arg)
{
    RTOS::Fifo *f = reinterpret_cast<RTOS::Fifo *>(arg);
    int oldtype, oldstate, dummy;
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
    while (!f->unixAccept()) {
        pthread_testcancel();
    }
    pthread_detach(pthread_self()); // at this point we got a conn so detach to avoid needing to be reaped
    pthread_setcanceltype(oldtype, &dummy);
    pthread_setcancelstate(oldstate, &dummy);
    return 0;
}

int RTOS::Fifo::unixChkCon(bool block)
{
  if (needcon) {
      if (!block) return 0;
      pthread_mutex_lock(&mut);      
      while (needcon && refct) {
          struct timespec ts;
#ifdef OS_OSX
          struct timeval tv;
          struct timezone tz;
          gettimeofday(&tv, &tz);
          TIMEVAL_TO_TIMESPEC(&tv, &ts);
#else
          clock_gettime(CLOCK_REALTIME, &ts);
#endif
          ts.tv_nsec += 100000000; // 100 ms polltime
          if (ts.tv_nsec >= 1000000000) ts.tv_nsec -= 1000000000, ts.tv_sec++;
          int ret = pthread_cond_timedwait(&cond, &mut, &ts);
          if (ret && ret != ETIMEDOUT) {
              fprintf(stderr, "pthread_cond_timedwait: %s\n", strerror(ret));
              pthread_mutex_unlock(&mut);
              return -1;
          }
      }
      pthread_mutex_unlock(&mut);
      if (!refct) return -1;
  }
  return 1;
}

bool RTOS::Fifo::unixAccept()
{
    int listen_fd = handle;
    fd_set fds;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100msec
    FD_ZERO(&fds);
    FD_SET(listen_fd, &fds);        
    int ret = select(listen_fd+1, &fds, 0, 0, &tv);
    if (ret == 0) return false;
    else if (ret < 0) {
        if (!refct) pthread_cancel(pthread_self());
        else if (errno != EINTR)
            perror("select");
        return false;
    }
    // at this point, select told us we have a new conn
    struct sockaddr_un addr;
    socklen_t len = sizeof(addr);
    handle = accept(listen_fd, (struct sockaddr *)&addr, &len);
    if (handle > -1) {
        close(listen_fd);
        listen_fd = -1;
        pthread_mutex_lock(&mut);
        needcon = false;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mut);
        return true;
    } else {
        if (!refct || errno == EINVAL) pthread_cancel(pthread_self());
        else perror("accept");
        handle = listen_fd;        
    }
    return false;
}
#endif

#ifdef EMULATOR
RTOS::FIFO RTOS::createFifo(unsigned & key_out, unsigned size)
{
  FIFO f = new Fifo;
#ifdef WIN32
  HANDLE h;  
  key_out = 1;
  do {
    std::ostringstream os;
    os << fifoFilePrefix() << ++key_out;    
    f->name = os.str();
    // keep trying various key_out id's until one succeeds
    h = CreateNamedPipeA(f->name.c_str(), 
                         PIPE_ACCESS_DUPLEX|0x00080000/*|FILE_FLAG_FIRST_PIPE_INSTANCE*/,
                         PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT/*|PIPE_REJECT_REMOTE_CLIENTS*/,
                         PIPE_UNLIMITED_INSTANCES,
                         size, size, 0,                      
                         (LPSECURITY_ATTRIBUTES)NULL);
  } while (h == INVALID_HANDLE_VALUE);
  f->handle = (long)h;  
  f->needcon = true;
  f->thrd = CreateThread(0, 0, RTOS::Fifo::winPipeListener, (void *)f, 0, 0);
  if (f->thrd) {
      // noop
  } else {
      f->destroy = false;
      CloseHandle(h);
      f->handle = -1;
  }
      
  
#else  //!WIN32
#  ifdef USE_UNIX_SOCKETS
  f->handle = socket(PF_UNIX, SOCK_STREAM, 0);
  if (f->handle >= 0) {
      static unsigned fifo_ids = 11211; // zip code of Williamsburg, Brooklyn
      std::ostringstream os;
      key_out = fifo_ids++;
      os << fifoFilePrefix() << key_out;
      f->name = os.str();
      struct sockaddr_un addr;
      addr.sun_family = AF_UNIX;
      snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", f->name.c_str());
      remove(addr.sun_path);
      if ( bind(f->handle, (struct sockaddr *)&addr, sizeof(addr)) ) {
          perror("bind");
          close(f->handle);
          f->handle = -1;
      } else {
          if (listen(f->handle, 1)) {
              perror("listen");
              close(f->handle);
              f->handle = -1;
          } else {
              f->unixCreateListener();
          }
      }
  }
#  else
#    error Define one of OS_WINDOWS, OS_LINUX, or OS_OSX
#  endif // ifdef OSX
#endif // ifdef WIN32

  if (f->handle < 0) {
    closeFifo(f);
    return INVALID_FIFO;
  }

  f->key = key_out;
  f->size = size;
  f->refct = 1;  
  fifos.insert(f);
  fifo_map[f->key] = f;
  return f;
}
#endif // EMULATOR

void RTOS::closeFifo(FIFO f)
{
  --f->refct;
#ifdef EMULATOR
#  ifdef WIN32
  if (!f->refct && f->handle != (long)INVALID_HANDLE_VALUE) {
      if (f->destroy) DisconnectNamedPipe((HANDLE)f->handle);
      if (f->thrd) TerminateThread(f->thrd, 0), f->thrd = 0;
      CloseHandle((HANDLE)f->handle);
  }
#  elif defined(USE_UNIX_SOCKETS)
  if (!f->refct) { 
      if (f->needcon && f->destroy) {
          pthread_cancel(f->thrd);
          pthread_cond_broadcast(&f->cond);
          void *dummy = 0;
          pthread_join(f->thrd, &dummy);
      }
      close(f->handle);
      if (f->destroy) remove(f->name.c_str());
      f->handle = -1;
  }
#  endif
#else /* !EMULATOR */
  ::close(f->handle);
#endif
  if (!f->refct) {
    fifos.erase(f);
    fifo_map.erase(f->key);
    delete f;
  }
}

void RTOS::destroyAllFifos()
{
  std::set<Fifo *> fcpy = fifos;
  for (std::set<Fifo *>::iterator it = fcpy.begin(); it != fcpy.end(); ++it) {
    (*it)->refct = 1;
    closeFifo(*it);
  }
  fifos.clear();
  fifo_map.clear();
}

int RTOS::writeFifo(FIFO f, const void *buf, unsigned long bufsz, bool block)
{
  int ret = -1;
#ifndef EMULATOR
  (void)block;
  ret = ::write(f->handle, buf, bufsz);
#elif defined(WIN32)
  DWORD nwrit = 0, oldstate = 0, state = 0;

  if (f->needcon) {
#  ifdef PRTERR
          FILE *fo = fopen(ERRLOG, "a");
          fprintf(fo, "(PID %d) writeFifo pipe %s is not connected? \n", (int)GetCurrentProcessId(), f->name.c_str());
          fclose(fo);
#  endif
          if (!block) return 0;
          else WaitNamedPipeA(f->name.c_str(), NMPWAIT_WAIT_FOREVER);
  }
  
  if (!block) {      
      GetNamedPipeHandleState((HANDLE)f->handle,
                              &oldstate,
                              0, 0, 0, 0, 0);
      state = oldstate;
      state &= ~(PIPE_WAIT);
      state |= PIPE_NOWAIT;
      SetNamedPipeHandleState((HANDLE)f->handle, &state, 0, 0);
  }
  BOOL fSuc = WriteFile((HANDLE)f->handle, (void *)buf, bufsz, &nwrit, NULL);
  if (fSuc) ret = nwrit;  
  else {
#  ifdef PRTERR
      DWORD err = GetLastError();
      FILE *fo = fopen(ERRLOG, "a");
      fprintf(fo, "(PID %d) write error on %s is %d\n", (int)GetCurrentProcessId(), f->name.c_str(), err);
      fclose(fo);
#  endif
  }
  if (!block) {      
      SetNamedPipeHandleState((HANDLE)f->handle, &oldstate, 0, 0);
  }
#elif defined(USE_UNIX_SOCKETS)
  int tmp, fl = -1;
  long sz;
  bool tryagain, lostconn;
 try_again:
  tmp = f->unixChkCon(block);
  if (tmp < 1)  return tmp;
  if (!block) {
      if (fl < 0) fl = fcntl(f->handle, F_GETFL, 0);
      fcntl(f->handle, F_SETFL, fl|O_NONBLOCK);
  }
  do {
      lostconn = tryagain = false;
#ifdef OS_OSX
      sz = recv(f->handle, &tmp, 0, 0);
      if (sz < 0 && errno == ENOTSOCK) lostconn = true;
      else sz = send(f->handle, buf, bufsz, 0);
#endif
#ifdef OS_LINUX
      sz = send(f->handle, buf, bufsz, MSG_NOSIGNAL);
      if (sz < 0 && errno == EPIPE) lostconn = true;
#endif
      if (f->destroy && lostconn) {
          if (!f->needcon) f->unixCreateListener();
          goto try_again;
      }
      if (sz < 0) {
          int err = errno;
          perror("send");
          if (!block && (err == EAGAIN || err == ENOBUFS) ) ret = 0;
          else if (block && (err == ENOBUFS || err == EAGAIN)) {
              tryagain = true;
              fd_set fds;
              FD_ZERO(&fds);
              FD_SET(f->handle, &fds);
              if ( select(f->handle+1, 0, &fds, 0, 0) < 0 ) {
                  tryagain = false;              
                  perror("select");
              }
          }
      } else if (sz > -1) 
          ret = sz;
  } while (tryagain);
  if (!block) {
      fcntl(f->handle, F_SETFL, fl);
  }
#else
#  error Emulator mode: Need to define one of OS_WINDOWS, OS_LINUX, or OS_OSX
#endif  
  return ret;
}

int RTOS::readFifo(FIFO f, void *buf, unsigned long bufsz, bool block)
{
  int ret = -1;
#ifndef EMULATOR
  (void)block;
  ret = ::read(f->handle, buf, bufsz);
#elif defined(WIN32)
  DWORD state = 0, oldstate = 0, err;
  if (f->needcon) {
#  ifdef PRTERR
          FILE *fo = fopen(ERRLOG, "a");
          fprintf(fo, "(PID %d) readFifo pipe %s is not connected? \n", (int)GetCurrentProcessId(), f->name.c_str());
          fclose(fo);
#  endif
          if (!block) return 0;
          else WaitNamedPipeA(f->name.c_str(), NMPWAIT_WAIT_FOREVER);
  }
  if (!block) {
      if (!GetNamedPipeHandleState((HANDLE)f->handle,
                                   &oldstate,
                                   0, 0, 0, 0, 0)) {
#  ifdef PRTERR
          FILE *fo = fopen(ERRLOG, "a");
          err = GetLastError();
          fprintf(fo, "(PID %d) GetNamedPipeHandleState error on %s is %d (block=%d)\n", (int)GetCurrentProcessId(), f->name.c_str(), err, block?1:0);
          fclose(fo);
#  endif
      }
      state = oldstate;
      state &= ~(PIPE_WAIT);
      state |= PIPE_NOWAIT;
      if (!SetNamedPipeHandleState((HANDLE)f->handle, &state, 0, 0)) {
#  ifdef PRTERR
          FILE *fo = fopen(ERRLOG, "a");
          err = GetLastError();
          fprintf(fo, "(PID %d) SetNamedPipeHandleState error on %s is %d (block=%d)\n", (int)GetCurrentProcessId(), f->name.c_str(), err, block?1:0);
          fclose(fo);
#  endif
      }
  }
  DWORD nread = 0;
  BOOL fSuc = ReadFile((HANDLE)f->handle, (void *)buf, bufsz, &nread, NULL);
  err = GetLastError();
  if (fSuc) ret = nread;
  else if (!err 
           || (err == ERROR_PIPE_NOT_CONNECTED && !block)
           || (err == ERROR_NO_DATA && !block)) ret = 0;
  else {
#  ifdef PRTERR
      FILE *fo = fopen(ERRLOG, "a");
      fprintf(fo, "(PID %d) read error on %s is %d (block=%d)\n", (int)GetCurrentProcessId(), f->name.c_str(), err, block?1:0);
      fclose(fo);
#  endif
  }
  if (!block) {
      SetNamedPipeHandleState((HANDLE)f->handle, &oldstate, 0, 0);
  }
#elif defined(USE_UNIX_SOCKETS)
  int tmp, fl = -1, sz;
 try_again:
  tmp = f->unixChkCon(block);
  if (tmp < 1)  return tmp;
  if (!block) {
      if (fl < 0) fl = fcntl(f->handle, F_GETFL, 0);
      fcntl(f->handle, F_SETFL, fl|O_NONBLOCK);
  }
  sz = recv(f->handle, buf, bufsz, 0);
  if (!sz && bufsz && f->destroy) { // connection lost
      if (!f->needcon) f->unixCreateListener();
      goto try_again;
  }
  if (sz < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !block) ret = 0;
  else if (sz > -1) ret = sz;
  else {
      perror("recv");      
  }
  if (!block) {
      fcntl(f->handle, F_SETFL, fl);
  }
#else 
#  error Emulator mode: Need to define one of OS_WINDOWS, OS_LINUX, or OS_OSX 
#endif
  return ret;
}

int RTOS::readFifo(unsigned key, void *buf, unsigned long bufsz, bool block)
{
  std::map<unsigned, Fifo *>::iterator it = fifo_map.find(key);
  if (it != fifo_map.end()) 
    return readFifo(it->second, buf, bufsz, block);
  return -1;
}

int RTOS::writeFifo(unsigned key, const void *buf, unsigned long bufsz, bool block)
{
  std::map<unsigned, Fifo *>::iterator it = fifo_map.find(key);
  if (it != fifo_map.end()) 
    return writeFifo(it->second, buf, bufsz, block);
  return -1;
}

void RTOS::closeFifo(unsigned key)
{
  std::map<unsigned, Fifo *>::iterator it = fifo_map.find(key);
  if (it != fifo_map.end()) 
    closeFifo(it->second);  
}

int RTOS::fifoNReadyForReading(FIFO f)
{
  int num = -1;
#if !defined(EMULATOR) || defined(USE_UNIX_SOCKETS)
  if ( ioctl(f->handle, FIONREAD, &num) )  // how much data is there?
      perror("ioctl");
#elif defined(WIN32)
  DWORD avail;
  if (f->needcon) {
#  ifdef PRTERR
          FILE *fo = fopen(ERRLOG, "a");
          fprintf(fo, "(PID %d) fifoNReadyForReading pipe %s is not connected? \n", (int)GetCurrentProcessId(), f->name.c_str());
          fclose(fo);
#  endif
          return 0;
  }

  if (PeekNamedPipe((HANDLE)f->handle, 0, 0, 0, &avail, 0)) {
      num = avail;
  } else {
#  ifdef PRTERR
      DWORD err = GetLastError();
      FILE *fo = fopen(ERRLOG, "a");
      fprintf(fo, "(PID %d) PeekNamedPipe error on %s is %d\n", (int)GetCurrentProcessId(), f->name.c_str(), err);
      fclose(fo);      
#  endif
  }
#else
#  error Emulator mode: Need to define one of OS_WINDOWS, OS_LINUX, or OS_OSX 
#endif /* def WIN32 */
  return num;
}

int RTOS::fifoNReadyForReading(unsigned key)
{
  std::map<unsigned, Fifo *>::iterator it = fifo_map.find(key);
  if (it != fifo_map.end()) 
    return fifoNReadyForReading(it->second);
  return -1;
}

const char *RTOS::statusString(ShmStatus s)
{
  switch(s) {
  case WrongSize:
    return  "The shared memory buffer was created with a different size than that specified in the attach call.";
  case NotFound:
    return "The shared memory buffer could not be found for the specified name.";
  case InvalidDevFile:
    {
      static char msgBuffer[256];
      ::snprintf(msgBuffer, sizeof(msgBuffer), "The shared memory buffer's device file, %s, does not appear to have the correct permissions.", shmDevFile());
      msgBuffer[sizeof(msgBuffer)-1] = 0;
      return  msgBuffer;
    }
  case MissingDevFile:
    {
      static char msgBuffer[256];
      ::snprintf(msgBuffer, sizeof(msgBuffer), "The shared memory buffer's device file, %s, is missing or otherwise inaccessible.", shmDevFile());
      msgBuffer[sizeof(msgBuffer)-1] = 0;
      return  msgBuffer;
    }
  case NoRTOSFound:
    return "The system does not appear to be running a known RTOS.";
  default:
    break;
  }
  return "Success.";
}


  /// wait on a fifo using select() - return true iff read possible, false if timed out or error
bool RTOS::waitReadFifo(unsigned key, unsigned millisecs)
{
    std::map<unsigned, Fifo *>::iterator it = fifo_map.find(key);
    if (it != fifo_map.end()) 
        return waitReadFifo(it->second, millisecs);
    return false;
}

bool RTOS::waitReadFifo(FIFO f, unsigned millisecs)
{
#ifdef WIN32    
    int ret = fifoNReadyForReading(f);
    if (ret == 0) {
        Sleep(millisecs);
        ret = fifoNReadyForReading(f);
    }
    return ret > 0;
#else
    if (f->handle < 0) return false;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(f->handle, &fds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = millisecs*1000;
    while (tv.tv_usec >= 1000000) ++tv.tv_sec, tv.tv_usec -= 1000000;
    int ret = select(f->handle+1, &fds, 0, 0, &tv);
    if (ret < 0) {
        perror("select");
    }
    return ret > 0;
#endif
}


namespace 
{
  long String2Long(const std::string & s)
  {
    long sum = 0;
    for (unsigned i = 0; i < s.length(); ++i) {
      unsigned c = s[i];
      c <<= i%(sizeof(long)) * 8;
      sum += c;
    }
    return sum;
  }
}
