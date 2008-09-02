#ifndef Common_H
#define Common_H
#include <QApplication>
#include <QString>
#include <exception>
#include "EmulApp.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

static inline EmulApp *emulApp() { return EmulApp::instance(); }

class Exception : public std::exception
{
public:
  Exception(const QString & reason = "") : reason(reason.toUtf8().data()) {}
  Exception(const std::string & reason = "") : reason(reason) {}
  virtual ~Exception() throw() {}

  const char * what() const throw() { return reason.c_str(); }

private:
  std::string reason;
};

class FatalException : public Exception
{
public:
    FatalException(const std::string & s) : Exception(s) {}
    FatalException(const QString & s) : Exception(s) {}
};

static inline QString TmpPath()
{
    static QString tmp = "";
    if (!tmp.length()) {
#if defined(Q_OS_WIN)
        char buf[256];    
        GetTempPathA(sizeof(buf), buf);
        buf[sizeof(buf)-1] = 0;
        tmp = buf;
#else
        tmp = "/tmp/";
#endif
    }
    return tmp;    
}

#endif
