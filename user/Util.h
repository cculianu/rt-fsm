/***************************************************************************
 *   Copyright (C) 2008 by Calin A. Culianu   *
 *   cculianu@yahoo.com   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifndef FSM_USER_UTIL_H
#define FSM_USER_UTIL_H
#include <vector>
#include <pthread.h>
#include <string>
#include <string.h>
#include <sstream>
#include "Mutex.h"
#include <sys/time.h> /* for struct timeval and gettimeofday -- for non-Unix we need to figure out a way to handle this */
#include "IntTypes.h"

#ifndef ABS
#define ABS(a) ( (a) < 0 ? -(a) : (a) )
#endif
#ifndef MIN
#  define MIN(a,b) ( (a) < (b) ? (a) : (b) )
#endif
#ifndef MAX
#  define MAX(a,b) ( (a) > (b) ? (a) : (b) )
#endif

template <class T> std::string ToString(const T & t)
{
  std::ostringstream o;
  o << t;
  return o.str();
}

/// returns the platform-specific temp path
const std::string & TmpPath();

template <class T> T FromString(const std::string & s, bool *ok = 0)
{
  std::istringstream is(s);
  T t;
  is >> t;
  if (ok) *ok = !is.fail();
  return t;
}

std::string trimWS(const std::string & s);

std::string TimeText ();

std::vector<double> splitNumericString(const std::string & str,
                                       const std::string &delims = ",",
                                       bool allowEmpties = true);
std::vector<std::string> splitString(const std::string &str,
                                     const std::string &delim = ",",
                                     bool trim_whitespace = true,
                                     bool skip_empties = true);

template <typename T> class shallow_copy
{
  struct Impl
  {
    T *t;
    volatile int nrefs;
    Impl() : t(0), nrefs(0) {}
    ~Impl() {}
  };
  mutable Impl *p;

  void ref_incr() const { ++p->nrefs; }
  void ref_decr() const { 
    if (!--p->nrefs) { 
      delete p->t; p->t = 0; delete p; 
    } 
  }

public:

  shallow_copy(T * t = new T) {  p = new Impl; p->t = t; ref_incr(); }
  ~shallow_copy() { ref_decr(); p = 0; }
  shallow_copy(const shallow_copy &r) { p = r.p; ref_incr();  }
  shallow_copy & operator=(const shallow_copy & r) { ref_decr(); p = r.p; ref_incr();  }
  // makes a private copy
  void duplicate() { Impl * i = new Impl(*p);  i->nrefs = 1; i->t = new T(*p->t); ref_decr(); p = i; }

  T & get() { return *p->t; }
  const T & get() const { return *p->t; }

  unsigned nrefs() const { return p->nrefs; }
};

/// A thread-safe logger class.  On destruction
/// the singleton logstream is written-to, in a thread-safe manner.
/// Note it uses shallow copy of data so it's possible to pass this by
/// value in and out of functions.  
class Log : protected shallow_copy<std::string>
{
  static pthread_mutex_t mut; // this is what makes it thread safe
  static std::ostream * volatile logstream;
protected:
  bool suppress;
public:
  static void setLogStream(std::ostream &);

  Log();
  virtual ~Log();
  
  template <typename T> Log & operator<<(const T & t) 
  {
    if (suppress) return *this;
    MutexLocker m(mut);
    std::ostringstream os;
    os << get() << t;  
    get() = os.str();
    return *this;
  }

  Log & operator << (std::ostream & (*pf)(std::ostream &));
}; 

class Timer
{
public:
  Timer() { reset(); }
  void reset();
  double elapsed() const; // returns number of seconds since ctor or reset() was called 
  static double now();
private:
  struct timeval ts;
};

class Exception
{
public:
  Exception(const std::string & reason = "") : reason(reason) {}
  virtual ~Exception() {}

  const std::string & why() const { return reason; }

private:
  std::string reason;
};

class FatalException : public Exception
{
public:
    FatalException(const std::string & s) : Exception(s) {}
};

/// returns a count of the number of bits set in the bitarray dwords
extern unsigned countBits(int32_t *dwords, unsigned num_dwords_in_array);


/**
 * findNextBit - find the next set bit in a memory region
 * @addr: The address to base the search on
 * @offset: The bitnumber to start searching at
 * @size: The maximum size to search, in number of bits
 */
extern unsigned long findNextBit(const unsigned long *addr, 
                                 unsigned long size,
                                 unsigned long offset);

#endif
