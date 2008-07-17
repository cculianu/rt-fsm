
#include "tcc_embedded_compat.h"
#include <rtl_time.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h>


long long getclock_us(void)
{
  return gethrtime();
}

void *Memset(void *s, int c, unsigned long n) { return memset(s, c, n); }
void *Memcpy(void *d, const void *s, unsigned long n) { return memcpy(d, s, n); }
void *Memmove(void *dest, const void *src, unsigned long n) { return memmove(dest, src, n); }


#define MIN_SIZE_USE_VMALLOC (32*1024)

void *Malloc(unsigned long size) 
{   
  unsigned long *ret =  0;
  if (size >= MIN_SIZE_USE_VMALLOC) 
    ret = (unsigned long *)vmalloc(size + sizeof(unsigned long));
  else 
    ret = (unsigned long *)kmalloc(size + sizeof(unsigned long), GFP_KERNEL);  
  if (ret) *ret++ = size; /* save the requested size before the
                             region, then increment ptr to the region */
  return ret;
}

#ifndef MIN
#define MIN(a,b) ( (a) > (b) ? (b) : (a) )
#endif

void *Realloc(void *ptr, unsigned long size)
{
  void *newptr = Malloc(size);
  if (newptr && ptr) {
    unsigned long *pul = (unsigned long *)ptr;
    --pul;
    Memcpy(newptr, ptr, MIN(*pul, size));
    Free(ptr);
  }
  return newptr;
}

void Free(void *ptr) 
{  
  unsigned long *pul = (unsigned long *)ptr;
  if (!ptr) return; /* Free(NULL) is valid */
  if (*--pul >= MIN_SIZE_USE_VMALLOC) 
    vfree(pul);
  else 
    kfree(pul);
}

unsigned long Strlen(const char *s) { return strlen(s); }
char *Strchr(const char *s, int c) { return strchr(s,c); }
char *Strrchr(const char *s, int c) { return strrchr(s,c); }

char *Strcpy(char *d, const char *s) { return strcpy(d, s); }
char *Strncpy(char *d, const char *s, unsigned long sz) { return strncpy(d, s, sz); }

int (*Printf)(const char *fmt, ...) __attribute__ ((format (printf, 1, 2))) = &printk;

int Sprintf(char *s, const char *fmt, ...)
{
  int ret;
  va_list ap;
  va_start(ap, fmt);
  ret = vsprintf(s, fmt, ap);
  va_end(ap);
  return ret;
}

int Snprintf(char *s, unsigned long n, const char *fmt, ...)
{
  int ret;
  va_list ap;
  va_start(ap, fmt);
  ret = vsnprintf(s, n, fmt, ap);
  va_end(ap);
  return ret;  
}

int Vsnprintf(char *buf, unsigned long size, const char *fmt, va_list args)
{ return vsnprintf(buf, size, fmt, args); }

double strtod(const char *s, char **endptr)
{
  char *end = 0;
  double ret = (double)strtol(s, &end, 10);
  if (*end == '.') {
    double tmp = 0.;
    char *pos = end+1;
    tmp = (double)strtoul(pos, &end, 10);
    while (end > pos++) tmp /= 10.0;
    ret += tmp;
  }
  *endptr = end;
  return ret;
}

unsigned long strtoul(const char *s, char **e, unsigned int b)
{
  return simple_strtoul(s, e, b);
}

long strtol(const char *s, char **e, unsigned int b) 
{ 
  return simple_strtol(s, e, b);
}

static inline int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

long long strtoll(const char *s, char **e, unsigned int b) 
{ 
  long long ret = 1;

  while (*s && is_space(*s)) ++s;
  if (*s == '-') ret = -1, ++s;
  ret *= (long long)(simple_strtoull(s, e, b));
  return ret;
}

double Ldexp(double x, int exp)
{
  double e = 1.0;
  while (exp--) {
    e *= 2.0;
  }
  return x*e;
}

int Strcmp(const char *s1, const char *s2) { return strcmp(s1,s2); }

int Memcmp(const void *v1, const void *v2, unsigned long n) { return memcmp(v1, v2, n); }

char *Strcat(char *d, const char *s) { return strcat(d, s); }
char *Strncat(char *d, const char *s, unsigned long n) { return strncat(d, s, n); }
