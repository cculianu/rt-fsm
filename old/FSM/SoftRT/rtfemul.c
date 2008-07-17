#include "rtfemul.h"
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>

static const char pathprefix[] = "rtfemul";
static int minor_to_fd_map[RTF_NO] = { 0 };

int rtf_create(unsigned minor, unsigned size)
{
  char pathbuf[] = "rtfemulXXXX";
  int ret;
  mode_t oldmask;
  (void) size;
  if (minor >= RTF_NO) return -EINVAL;
  if (minor_to_fd_map[minor]) return -EBUSY;  
  snprintf(pathbuf, strlen(pathbuf), "%s%u", pathprefix, minor);
  unlink(pathbuf);
  oldmask = umask(0);
  ret = mknod(pathbuf, 0666|S_IFIFO, 0);  
  umask(oldmask);
  if (ret) {
    perror("mknod");
    return -errno;
  }
  ret = rtf_open(minor);
  if (ret < 0) return -errno;
  minor_to_fd_map[minor] = ret+1;
  return 0;
}

int rtf_destroy(unsigned minor)
{
  if (minor_to_fd_map[minor]) {
    char pathbuf[] = "rtfemulXXXX";
    snprintf(pathbuf, strlen(pathbuf), "%s%u", pathprefix, minor);
    unlink(pathbuf);
    close(minor_to_fd_map[minor]-1);
  }
  minor_to_fd_map[minor] = 0;
  return 0;
}

int rtf_get(unsigned minor, void *buf, unsigned bytes)
{
  int fd = minor_to_fd_map[minor] - 1;
  if (fd < 0) return -EINVAL;
  return read(fd, buf, bytes);
}

int rtf_put(unsigned minor, const void *buf, unsigned bytes)
{
  int fd = minor_to_fd_map[minor] - 1;
  if (fd < 0) return -EINVAL;
  return write(fd, buf, bytes);
}

int rtf_num_avail(unsigned minor)
{
  int fd = minor_to_fd_map[minor] - 1, num;
  if (fd < 0) return -EINVAL;
  if ( ioctl(fd, FIONREAD, &num) < 0 ) 
    return -errno;
  return num;
}

int rtf_open(unsigned minor)
{
  if (minor >= RTF_NO) return -EINVAL;
  char pathbuf[] = "rtfemulXXXX";
  sprintf(pathbuf, "%s%u", pathprefix, minor);
  return open(pathbuf, O_RDWR);
}
