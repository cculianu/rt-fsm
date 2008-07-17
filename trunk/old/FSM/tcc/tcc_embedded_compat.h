/** @file tcc_embedded_compat.h - adaptor/code that 
    makes tcc work in an embedded environment by implementing libc
    functions ourselves, etc.  Portions of this code stolen from uclibc */
#ifndef TCC_EMBEDDED_COMPAT_H
#define TCC_EMBEDDED_COMPAT_H

#include "stdarg.h"

#ifdef __cplusplus
extern "C" {
#endif

  /** jmp_buf used for setjmp/longjmp */
  typedef int __jmp_buf[6];

  /* Calling environment, plus possibly a saved signal mask.  */
  typedef struct __jmp_buf_tag	/* C++ doesn't like tagless structs.  */
  {
    /* NOTE: The machine-dependent definitions of `__sigsetjmp'
       assume that a `jmp_buf' begins with a `__jmp_buf' and that
       `__mask_was_saved' follows it.  Do not move these members
       or add others before it.  */
    __jmp_buf __jmpbuf;		/* Calling environment.  */
  } jmp_buf[1];
#ifndef NULL
#define NULL 0
#endif
  extern int setjmp (jmp_buf __env);
  extern void longjmp (jmp_buf __env, int __val) __attribute__((__noreturn__));
  extern long long getclock_us(void);
  extern void *Memset(void *s, int c, unsigned long n);
  extern void *Memcpy(void *dest, const void *src, unsigned long n);
  extern void *Memmove(void *dest, const void *src, unsigned long n);
  extern void *Malloc(unsigned long size);
  extern void *Realloc(void *ptr, unsigned long size);
  extern void Free(void *ptr);
  extern unsigned long Strlen(const char *s);
  extern char *Strchr(const char *s, int c);
  extern char *Strrchr(const char *s, int c);
  extern char *Strcpy(char *d, const char *s);
  extern char *Strncpy(char *d, const char *s, unsigned long);
  extern char *Strcat(char *d, const char *s);
  extern char *Strncat(char *d, const char *s, unsigned long n);
  extern int (*Printf)(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
  extern int Sprintf(char *, const char *, ...);
  extern int Snprintf(char *, unsigned long, const char *, ...);
  extern int Vsnprintf(char *buf, unsigned long size, const char *fmt, va_list args);
  extern double strtod(const char *, char **endptr);
  extern unsigned long strtoul(const char *, char **, unsigned int);
  extern long strtol(const char *, char **, unsigned int);
  extern long long strtoll(const char *s, char **e, unsigned int b);
  extern double Ldexp(double x, int exp);
  extern int Strcmp(const char *s1, const char *s2);
  extern int Memcmp(const void *s1, const void *s2, unsigned long n);
#ifdef __cplusplus
}
#endif

#endif
