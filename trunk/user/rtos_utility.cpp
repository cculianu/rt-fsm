#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rtos_utility.h"
#include "rtos_shared_memory.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
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

#if defined(WINDOWS) || defined(WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/ipc.h>
#  include <sys/shm.h>
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#endif

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX 108
#endif

namespace 
{
  long String2Long(const std::string & s);  
}

struct RTOS::Fifo
{
  long handle; /* EMULATOR:
                 in Windows the hHandle of the named pipe, 
                 otherwise the a unix socket dgram socket fd!
                 Non-EMULATOR:
                 the file descriptor of the /dev/rtfXX opened for reading or 
                 writing */
  int key;    /* the minor specified at open time */
  std::string name; /* in Windows the \\.\pipe\PIPENAME string, otherwise
                       nothing */
  unsigned size, refct;
  bool unlink;
  Fifo() : handle(-1), key(-1), size(0), refct(0), unlink(false) {}  
};

RTOS::Fifo * const RTOS::INVALID_FIFO = 0;

namespace {
  std::set<RTOS::Fifo *> fifos;
  std::map<unsigned, RTOS::Fifo *> fifo_map; // map of key -> fifo instance
}


RTOS::RTOS RTOS::determine()
{
  RTOS rtosUsed = Unknown;
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
  
  return rtosUsed;
}

struct ShmInfo
{
  std::string name;
  size_t size;
  void *address;
  RTOS::RTOS rtos;
  ShmInfo() : size(0), address(0), rtos(RTOS::Unknown) {}
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
#if defined(WINDOWS) || defined(WIN32)
  /* in cygwin we don't use sysv ipc *at all* because it requires 
     cygserver be running and it's annoying.  
     
     Instead, will go ahead and use the built-in windows 
     functions for shared memory.. */
    HANDLE hMapFile;
    if (create) {
      hMapFile = CreateFileMapping(INVALID_HANDLE_VALUE,   // use paging file
                                   NULL,       // default security attributes
                                   PAGE_READWRITE,  // read/write access
                                   0,           // size: high 32-bits
                                   size,       // size: low 32-bits
                                   SHM_NAME); // name of map object
      
    } else {
      hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);
    }
    if (hMapFile) {
      ret = (void *)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, size);
      CloseHandle(hMapFile);
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
#if defined(WINDOWS) || defined(WIN32)
          {
            HANDLE hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, inf.name.c_str());
            if (hMapFile) {
              UnmapViewOfFile(inf.address);
              CloseHandle(hMapFile);
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
RTOS::FIFO RTOS::openFifo(unsigned key, ModeFlag mode)
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

#  if defined(WIN32)
  if (!f->name.length()) {
    std::ostringstream os;
    os << fifoFilePrefix() << key;
    f->name = os.str();
  }
  if (f->handle < 0) {
    int m = GENERIC_READ | GENERIC_WRITE; // always open read/write, no matter what they asked for
    f->handle = (long)CreateFile(f->name.c_str(), m, 0, NULL, OPEN_EXISTING, 0, NULL); 
  }
#  else // unix socket
  if (!f->name.length()) {
    std::ostringstream os;
    os << fifoFilePrefix() << key;
    f->name = os.str();
    f->handle = socket(PF_UNIX, SOCK_DGRAM, 0);
    if (f->handle > -1) {
      struct sockaddr_un addr;
      addr.sun_family = AF_UNIX;
      strncpy(addr.sun_path, f->name.c_str(), UNIX_PATH_MAX);
      addr.sun_path[UNIX_PATH_MAX-1] = 0;
      connect(f->handle, (struct sockaddr *)&addr, sizeof(addr));
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
  if (ret < 0) return INVALID_FIFO;
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
    h = CreateNamedPipe(f->name.c_str(), 
                        PIPE_ACCESS_DUPLEX,
                        PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT,
                        PIPE_UNLIMITED_INSTANCES,
                        size, size, 0, NULL);
  } while (h == INVALID_HANDLE_VALUE);
  f->handle = (long)h;
#else  //!WIN32
  static int fifo_ids = getpid()*1000;
  key_out = ++fifo_ids;
  std::ostringstream os;
  os << fifoFilePrefix() << key_out;
  f->name = os.str();
  f->handle = socket(PF_UNIX, SOCK_DGRAM, 0);
  struct sockaddr_un addr;
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, f->name.c_str(), UNIX_PATH_MAX);
  addr.sun_path[UNIX_PATH_MAX-1] = 0;
  int err = bind(f->handle, (struct sockaddr *)&addr, sizeof(addr)); // bind socket to /tmp/file..  
  if (err) {    
    closeFifo(f); 
    return INVALID_FIFO;
  }
  f->unlink = true;
#endif

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
#endif

void RTOS::closeFifo(FIFO f)
{
  --f->refct;
#ifdef EMULATOR
#  ifdef WIN32
  if (!f->refct && f->handle != (long)INVALID_HANDLE_VALUE) {
      CloseHandle((HANDLE)f->handle);
  }
#  else
  if (!f->refct) { 
    ::shutdown(f->handle, SHUT_RDWR);
    ::close(f->handle);
    f->handle = -1;
    if (f->unlink)
      ::unlink(f->name.c_str());
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

int RTOS::writeFifo(FIFO f, const void *buf, unsigned long bufsz)
{
  int ret = -1;
#ifdef WIN32  
  DWORD nwrit = 0;
  BOOL fSuc = WriteFile((HANDLE)f->handle, (void *)buf, bufsz, &nwrit, NULL);
  if (fSuc) ret = nwrit;  
#else
#  ifdef EMULATOR
  ret = ::send(f->handle, buf, bufsz, 0);
#  else
  ret = ::write(f->handle, buf, bufsz);
#  endif
#endif
  return ret;
}

int RTOS::readFifo(FIFO f, void *buf, unsigned long bufsz)
{
  int ret = -1;
#ifdef WIN32  
  DWORD nread = 0;
  BOOL fSuc = ReadFile((HANDLE)f->handle, (void *)buf, bufsz, &nread, NULL);
  if (fSuc) ret = nread;
#else
#  ifdef EMULATOR
  ret = ::recv(f->handle, buf, bufsz, 0);
#  else
  ret = ::read(f->handle, buf, bufsz);
#  endif
#endif
  return ret;
}

int RTOS::readFifo(unsigned key, void *buf, unsigned long bufsz)
{
  std::map<unsigned, Fifo *>::iterator it = fifo_map.find(key);
  if (it != fifo_map.end()) 
    return readFifo(it->second, buf, bufsz);  
  return -1;
}

int RTOS::writeFifo(unsigned key, const void *buf, unsigned long bufsz)
{
  std::map<unsigned, Fifo *>::iterator it = fifo_map.find(key);
  if (it != fifo_map.end()) 
    return writeFifo(it->second, buf, bufsz);  
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
#ifdef WIN32
  DWORD avail;
  if (PeekNamedPipe((HANDLE)f->handle, 0, 0, 0, &avail, 0)) num = avail;  
#else
  ::ioctl(f->handle, FIONREAD, &num); // how much data is there?
#endif
  return num;
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
