#include "Util.h"
#include <time.h>
#include <algorithm>
#include <functional>
#include <iostream>
#include <ctype.h>
#include <sys/time.h> /* for struct timeval and gettimeofday -- for non-Unix we need to figure out a way to handle this */
#ifdef OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
    pos2 = str.find(delim, pos);
    if (pos2 == std::string::npos) pos2 = str.length();
    std::string s = str.substr(pos, pos2-pos);
    if (trimws) s = trimWS(s);
    if (!skip_empties || s.length())
        ret.push_back(s);
    pos2 += delim.length(); // move past delim. token
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
  if (pos1 > pos2) return ""; // completely empty string
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

const std::string & TmpPath()
{
    static std::string tmp = "";
    if (!tmp.length()) {
#if defined(OS_WINDOWS)
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

unsigned countBits(int32_t *dwords,
                   unsigned ndwords)
{
    unsigned ret = 0, v;
    for (unsigned i = 0; i < ndwords; ++i) {
        v = dwords[i];
        v = v - ((v >> 1) & 0x55555555);  // reuse input as temporary
        v = (v & 0x33333333) + ((v >> 2) & 0x33333333);     // temp
        ret += ((v + (v >> 4) & 0xF0F0F0F) * 0x1010101) >> 24; // count
    }
    return ret;
}

static inline int __ffs(int x) { return ffs(x)-1; }
#define BITS_PER_LONG (sizeof(long)*8)
#define BITOP_WORD(nr)          ((nr) / BITS_PER_LONG)
/**
 * findNextBit - find the next set bit in a memory region
 * @addr: The address to base the search on
 * @offset: The bitnumber to start searching at
 * @size: The maximum size to search, in number of bits
 */
unsigned long findNextBit(const unsigned long *addr, 
                          unsigned long size,
                          unsigned long offset)
{

        const unsigned long *p = addr + BITOP_WORD(offset);
        unsigned long result = offset & ~(BITS_PER_LONG-1);
        unsigned long tmp;

        if (offset >= size)
                return size;
        size -= result;
        offset %= BITS_PER_LONG;
        if (offset) {
                tmp = *(p++);
                tmp &= (~0UL << offset);
                if (size < BITS_PER_LONG)
                        goto found_first;
                if (tmp)
                        goto found_middle;
                size -= BITS_PER_LONG;
                result += BITS_PER_LONG;
        }
        while (size & ~(BITS_PER_LONG-1)) {
                if ((tmp = *(p++)))
                        goto found_middle;
                result += BITS_PER_LONG;
                size -= BITS_PER_LONG;
        }
        if (!size)
                return result;
        tmp = *p;

found_first:
        tmp &= (~0UL >> (BITS_PER_LONG - size));
        if (tmp == 0UL)         /* Are any bits set? */
                return result + size;   /* Nope. */
found_middle:
        return result + __ffs(tmp);
}
