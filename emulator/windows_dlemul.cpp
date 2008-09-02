#include "windows_dlemul.h"
#include <windows.h>
extern "C" {

    static DWORD lasterror = 0;
    static char msgbuf[2048];

    void *dlopen(const char *filename, int flag)
    {
        (void)flag;
        void *ret = (void *)LoadLibraryA(filename);
        if (!ret) lasterror = GetLastError();
        return ret;
    }
    
    const char *dlerror(void)
    {
        DWORD ret = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM,
                                   0,
                                   lasterror,
                                   0,
                                   msgbuf,
                                   sizeof(msgbuf),
                                   0);
        if (ret) { msgbuf[ret] = 0; return msgbuf; }
        return "Unknown error.";
    }
    
    void *dlsym(void *handle, const char *symbol)
    {
        void * ret = (void *)GetProcAddress((HMODULE)handle, symbol);
        if (!ret) lasterror = GetLastError();
        return ret;
    }
    
    int dlclose(void *handle)
    {
        if (!FreeLibrary((HMODULE)handle)) {
            lasterror = GetLastError();
            return -1;
        }
        return 0;
    }

}
