#include "mbuffemul.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <map>
#include <stdio.h>
#include <errno.h>
#include <string>

namespace {
  typedef std::map<std::string, std::pair<void *, unsigned> > MbufMap;
  MbufMap mbufMap;
  const std::string myext = ".MbufEmul";
};

#if defined(MBUF_EMUL_USE_NAMESPACE)
namespace MbufEmul {
#endif

void *mbuff_alloc(const char *n, unsigned size)
{
  std::string name(n);
  MbufMap::iterator it;
  if ((it = mbufMap.find(name)) != mbufMap.end()) 
    mbuff_free(n, it->second.first);
  
  mode_t oldMask = umask(0);
  std::string fname = name + myext;
  int fd = open(fname.c_str(), O_CREAT|O_TRUNC|O_RDWR, 00666);
  umask(oldMask);
  if (fd < 0) {
    int saved_errno = errno;
    perror("open");
    errno = saved_errno;
    return 0;
  }
  if (!size) return 0;
  lseek(fd, size-1, SEEK_SET);  
  char dummy = 0;
  write(fd, &dummy, 1); // write 1 byte to 'grow' the file
  void *ret = mmap(0, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  int saved_errno = errno;
  close(fd);
  if (!ret || ret == MAP_FAILED) {
    errno = saved_errno;
    perror("mmap");
    errno = saved_errno;
    return 0;
  }
  mbufMap[name] = std::pair<void *, unsigned>(ret, size);
  return ret;
}

void mbuff_free(const char *n, void *mbuf)
{
  std::string name(n);
  std::string fname(name + myext);
  MbufMap::iterator it;
  if ((it = mbufMap.find(name)) != mbufMap.end()) {
    munmap(it->second.first, it->second.second);
    if (mbuf != it->second.first) {
      fprintf(stderr, "Warning: mbuf_free() passed in mbuff pointer (%p) differs from the pointer used at allocation-time (%p)!\n", mbuf, it->second.first);
    }
    mbufMap.erase(it);
    unlink(fname.c_str());
  }
}

void *mbuff_attach(const char *n, unsigned size)
{
  std::string name(n);

  if (mbufMap.find(name) != mbufMap.end()) 
    return mbufMap[name].first;
  
  std::string fname(name + myext);  
  int fd = open(fname.c_str(), O_RDWR);
  if (fd < 0) {
    int saved_errno = errno;
    perror("open");
    errno = saved_errno;
    return 0;
  }
  void *ret = mmap(0, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  int saved_errno = errno;
  close(fd);
  if (!ret || ret == MAP_FAILED) {
    errno = saved_errno;
    perror("mmap");
    errno = saved_errno;
    return 0;
  }
  mbufMap[name] = std::pair<void *, unsigned>(ret, size);
  return ret;
}

void mbuff_detach(const char *n, void *mbuf)
{
  std::string name(n);
  MbufMap::iterator it;
  if ( (it=mbufMap.find(name)) != mbufMap.end()) {
    munmap(it->second.first, it->second.second);
    if (mbuf != it->second.first) {
      fprintf(stderr, "Warning: mbuf_free() passed in mbuff pointer (%p) differs from the pointer used at allocation-time (%p)!\n", mbuf, it->second.second);
    }
    mbufMap.erase(it);
  }
}

#if defined(MBUF_EMUL_USE_NAMESPACE)
};
#endif
