#include "deflate_helper.h"

#ifdef __KERNEL__
#define USERSPACE 0
#define KERNELSPACE 1
#else
#define USERSPACE 1
#define KERNELSPACE 0
#endif


#if USERSPACE
#  include <zlib.h>
#  include <string.h>
#  include <stdlib.h>
#  define DH_MALLOC malloc
#  define DH_FREE free
#elif KERNELSPACE
#  include <linux/zlib.h>
#  include <linux/string.h>
#  include <linux/kernel.h>
#  include <linux/slab.h>
#  include <linux/vmalloc.h>
#  define DH_MALLOC vmalloc
#  define DH_FREE vfree
#endif

#if USERSPACE 
  static void *dh_helper_alloc(void *opaque, uInt items, uInt size)
  {
    (void)opaque;
    return DH_MALLOC(items*size);
  }
  static void dh_helper_free(void *opaque, void *address)
  {
    (void)opaque;
    DH_FREE(address);
  }
#endif

#ifndef NO_DEFLATE
int deflateInplace(char *buf, unsigned buflen, unsigned buftotal)
{
  unsigned cmp_sz = 0;
  char *altbuf = deflateCpy(buf, buflen, &cmp_sz);
  if (!altbuf || cmp_sz > buftotal) {
    freeDHBuf(altbuf);
    return -1;
  }
  memcpy(buf, altbuf, cmp_sz);
  freeDHBuf(altbuf);
  return cmp_sz;
}
#endif // NO_DEFLATE

int inflateInplace(char *buf, unsigned buflen_comp, unsigned bufsize)
{
  unsigned uncmp_sz = 0;
  char *altbuf = inflateCpy(buf, buflen_comp, bufsize, &uncmp_sz);
  if (!altbuf || uncmp_sz > bufsize) {
    freeDHBuf(altbuf);
    return -1;
  }
  memcpy(buf, altbuf, uncmp_sz);
  freeDHBuf(altbuf);
  return uncmp_sz;  
}

void freeDHBuf(void *buf) { DH_FREE(buf); }
#ifndef NO_DEFLATE
char *deflateCpy(const char *buf, unsigned buflen, unsigned *cmplen_out)
{
  z_stream z;
  char *altbuf, *retval = 0;
  int tmp = ~Z_OK;
  unsigned buftotal = buflen+buflen/1000+12;
  
  memset(&z, 0, sizeof(z));
  altbuf = (char *)DH_MALLOC(buftotal);
  if (!altbuf) return 0;
  memset(altbuf, 0, buftotal);
#if USERSPACE
  z.zalloc = &dh_helper_alloc;
  z.zfree = &dh_helper_free;
#endif
  z.data_type = Z_ASCII;
  z.next_in = (Byte *)buf;
  z.avail_in = buflen;
  z.next_out = (Byte *)altbuf;
  z.avail_out = buftotal;

#if KERNELSPACE
    z.workspace = DH_MALLOC(zlib_deflate_workspacesize());
    tmp = zlib_deflateInit(&z, Z_BEST_COMPRESSION);
#elif USERSPACE
    tmp = deflateInit(&z, Z_BEST_COMPRESSION);
#endif

  if (tmp != Z_OK) {
#if KERNELSPACE
    DH_FREE(z.workspace);
#endif
    DH_FREE(altbuf);    
    retval = 0;
    goto out;
  }
  
#if KERNELSPACE
  tmp = zlib_deflate(&z, Z_FINISH);
#elif USERSPACE
  tmp = deflate(&z, Z_FINISH);
#endif

  if (tmp != Z_STREAM_END || z.total_out > buftotal) {
    DH_FREE(altbuf);
    retval = 0;
    goto out2;
  }

  retval = altbuf;
  if (cmplen_out) *cmplen_out = z.total_out;
  
 out2:
#if KERNELSPACE
  zlib_deflateEnd(&z);
  DH_FREE(z.workspace);
#elif USERSPACE
  deflateEnd(&z);
#endif
 out:
  return retval;
}
#endif // NO_DEFLATE
char *inflateCpy(const char *buf, unsigned buflen, unsigned bufmax, unsigned * uncomp_size_out)
{
  z_stream z;
  char *altbuf, *retval = 0;
  int tmp = ~Z_OK;

  memset(&z, 0, sizeof(z));
  altbuf = (char *)DH_MALLOC(bufmax);
#if KERNELSPACE
  // debug
  if (!altbuf) printk("inflateCpy: error allocating memory\n");
#endif
  if (!altbuf) return 0;

  memset(altbuf, 0, bufmax);
#if USERSPACE
  z.zalloc = &dh_helper_alloc;
  z.zfree = &dh_helper_free;
#endif
  z.data_type = Z_ASCII;
  z.next_in = (Byte *)buf;
  z.avail_in = buflen;
  z.next_out = (Byte *)altbuf;
  z.avail_out = bufmax;

#if USERSPACE  
  tmp = inflateInit(&z);
#elif KERNELSPACE
  z.workspace = (void *)DH_MALLOC(zlib_inflate_workspacesize());
  tmp = zlib_inflateInit(&z);
#endif

  if (tmp != Z_OK) {
#if KERNELSPACE
  // debug
    printk("inflateCpy: error from inflateInit: %d\n", tmp);
    DH_FREE(z.workspace);
#endif
    DH_FREE(altbuf);
    goto out;
  }

#if USERSPACE
  tmp = inflate(&z, Z_FINISH);
#elif KERNELSPACE
  tmp = zlib_inflate(&z, Z_FINISH);
#endif
  if (tmp != Z_STREAM_END || z.total_out > bufmax) {
    DH_FREE(altbuf);
    goto out2;
  }
  
  if (uncomp_size_out) *uncomp_size_out = z.total_out;
  retval = altbuf;
  
 out2:
#if USERSPACE  
  inflateEnd(&z);
#elif KERNELSPACE
  zlib_inflateEnd(&z);
  DH_FREE(z.workspace);
#endif
 out:
  return retval;
}


#ifdef TEST_DEFLATE_HELPER

#  if !USERSPACE
#    error Can only test the deflate helper in userspace
#  endif

#include <stdio.h>
#include <time.h>
#include <limits.h>

int main(void)
{
  char buf[63926], buf2[63926], *buf3, *buf4;
  unsigned sz_cmp = 63926, sz_uncmp = 63900;
  int i, ret;
  unsigned tmp;
  srand(time(0));

  for (i = 0; i < (int)sz_uncmp; ++i) buf[i] = rand() % (CHAR_MAX-CHAR_MIN) - CHAR_MIN;
  memcpy(buf2, buf, sz_uncmp);

  ret = deflateInplace(buf, sz_uncmp, sz_cmp);
  if (ret < 0) {
    printf("deflateInplace returned %d\n", i);
    return -1;
  }
  
  ret = inflateInplace(buf, ret, sz_cmp);
  if (ret < 0) {
    printf("inflateInplace returned %d\n", ret);
    return -1;    
  }

  if (memcmp(buf, buf2, ret)) {
    printf("buffers differ\n"); 
    return -2;
  }
  printf("buffers identical after in-place inflate/deflate\n");   
  buf3 = deflateCpy(buf, ret, &tmp);
  printf("tmp = %u\n", tmp);      
  if (!buf3) {
    printf("deflateCpy returned NULL\n");       
    return -1;
  }
  buf4 = inflateCpy(buf3, tmp, sz_uncmp, &tmp);
  printf("tmp = %u\n", tmp);      

  if (memcmp(buf2, buf4, tmp)) {
    printf("buffers differ after copy inflate/deflate\n"); 
    return -1;
  }
  printf("buffers identical after copy inflate/deflate\n");   
  freeDHBuf(buf3);
  freeDHBuf(buf4);
  return 0;
}
#endif

