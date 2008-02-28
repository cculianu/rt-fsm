#include "Util.h"
#include <time.h>
#include <algorithm>
#include <functional>
#include <iostream>
#include <ctype.h>
#include <sys/time.h> /* for struct timeval and gettimeofday -- for non-Unix we need to figure out a way to handle this */
pthread_mutex_t Log::mut = PTHREAD_MUTEX_INITIALIZER;
std::ostream * volatile Log::logstream = 0; // in case we want to log other than std::cerr..

Log::Log() : suppress(false)
{ 
  MutexLocker m(mut);
  if (!logstream) logstream = &std::cerr; 
}

Log::~Log() { if (nrefs() == 1) { MutexLocker m(mut); (*logstream) << get(); get() = ""; } }
  
void Log::setLogStream(std::ostream &os) 
{
  MutexLocker ml(mut);
  logstream  = &os;
}

Log & Log::operator << (std::ostream & (*pf)(std::ostream &)) {
    if (suppress) return *this;
    MutexLocker m(mut);
    std::ostringstream os;
    os << get() << pf;  
    get() = os.str();
    return *this;
}

std::vector<double> splitNumericString(const std::string &str,
                                       const std::string &delims,
                                       bool allowEmpties)
{
  std::vector<double> ret;
  if (!str.length() || !delims.length()) return ret;
  std::string::size_type pos;
  for ( pos = 0; pos < str.length() && pos != std::string::npos; pos = str.find_first_of(delims, pos) ) {
      if (pos) ++pos;
      bool ok;
      double d = FromString<double>(str.substr(pos), &ok);
      if (!ok)
          // test for zero-length token
          /* break if parse error -- but conditionally allow empty tokens.. */
          if (allowEmpties && ( delims.find(str[pos]) != std::string::npos || pos >= str.length() ) ) {
              if (!pos) ++pos;
          }      
          else
              break; 
      else
          ret.push_back(d);
  }
  return ret;
}

std::vector<std::string> splitString(const std::string &str,
                                     const std::string &delim,
                                     bool trimws,
                                     bool skip_empties)
{
  std::vector<std::string> ret;
  if (!str.length() || !delim.length()) return ret;
  std::string::size_type pos, pos2;
  for ( pos = 0, pos2 = 0; pos < str.length() && pos != std::string::npos; pos = pos2 ) {
    if (pos) pos = pos + delim.length();
    if (pos > str.length()) break;
    pos2 = str.find(delim, pos);
    if (pos2 == std::string::npos) pos2 = str.length();
    std::string s = str.substr(pos, pos2-pos);
    if (trimws) s = trimWS(s);
    if (skip_empties && !s.length()) continue;
    ret.push_back(s);
  }
  return ret;  
}


std::string TimeText ()
{
  char buf[1024];
  time_t t = time(0);
  struct tm tms;
  localtime_r(&t, &tms);
  

  size_t sz = strftime(buf, sizeof(buf)-1, "%b %d %H:%M:%S", &tms);
  buf[sz] = 0;
  return buf;
}

std::string trimWS(const std::string & s)
{
  std::string::const_iterator pos1, pos2;
  pos1 = std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun(::isspace)));
  pos2 = std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun(::isspace))).base();
  return std::string(pos1, pos2);
}

void Timer::reset()
{
  ::gettimeofday(&ts, NULL);
}

double Timer::elapsed() const
{
  if (ts.tv_sec == 0) return 0.0;
  struct timeval tv;
  ::gettimeofday(&tv, NULL);
  return tv.tv_sec - ts.tv_sec + (tv.tv_usec - ts.tv_usec)/1000000.0;
}

// static
double Timer::now() { struct timeval ts; ::gettimeofday(&ts, 0); return double(ts.tv_sec) + ts.tv_usec/1e6; }
