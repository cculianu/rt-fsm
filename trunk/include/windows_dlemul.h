#ifndef windows_dlemul_H
#define windows_dlemul_H

#if defined(OS_WINDOWS) || defined(WIN32)
#ifdef __cplusplus
extern "C" {
#endif

    extern void *dlopen(const char *filename, int flag);    
    extern const char *dlerror(void);
    extern void *dlsym(void *handle, const char *symbol);
    extern int dlclose(void *handle);
#   define RTLD_NOW 0

#ifdef __cplusplus
}
#endif

#else

#  warning windows_dlemul.h included but not using Windows platform!  Trying to #include <dlfcn.h> ...
#  include <dlfcn.h>

#endif

#endif
