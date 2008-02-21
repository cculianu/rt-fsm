#ifndef Mutex_H
#define Mutex_H

#include <pthread.h>

struct Mutex
{
  Mutex() { pthread_mutex_init(&mut, 0); }
  ~Mutex() { pthread_mutex_destroy(&mut); }
  void lock() { pthread_mutex_lock(&mut); }
  void unlock() { pthread_mutex_unlock(&mut); }
  bool tryLock() { return pthread_mutex_trylock(&mut) == 0; }
private:
  friend struct MutexLocker;
  pthread_mutex_t mut;
};

struct MutexLocker
{
  MutexLocker(pthread_mutex_t *m) : mut(*m) { pthread_mutex_lock(m); }
  MutexLocker(pthread_mutex_t  & m) : mut(m) { pthread_mutex_lock(&m); }
  MutexLocker(Mutex & m) : mut(m.mut) { pthread_mutex_lock(&mut); }
  ~MutexLocker() { pthread_mutex_unlock(&mut); }
  pthread_mutex_t & mut;
};

#endif
