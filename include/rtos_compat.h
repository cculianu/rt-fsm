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
/** @file rtos_compat.h
 *  Kernel-side definitions to make RTAI calls look lik RTLinux calls on the
 *  kernel side.  While we prefer RTAI, we originally wrote this code
 *  for RTLinux.  Hence, this spaghetti of making RTAI calls look like RTLinux 
 *  calls!  The benefit is the kernel-side code just calls one set of functions
 *  (the RTLINUX ones) even if it's in RTAI.  Making the code easier to read and
 *  maintain, if a bit confusing.  :)
 */
#ifndef RTOS_COMPAT_H
#define RTOS_COMPAT_H

#ifndef __KERNEL__
#  error This header is meant to be used in the kernel only!
#endif

#include <linux/spinlock.h>

#if defined(RTLINUX) && !defined(RTAI)

#  include <rtl.h>
#  include <rtl_time.h>
#  include <rtl_fifo.h>
#  include <rtl_sched.h>
#  include <rtl_mutex.h>
#  include <rtl_sync.h>
#  include <mbuff.h>
#  define rt_printk rtl_printf
#  define rtf_get_if rtf_get
#  define rt_critical(flags) rtl_critical(flags)
#  define rt_end_critical(flags) rtl_end_critical(flags)
#  define CURRENT_IS_RT() (pthread_self() != pthread_linux())
#  define CURRENT_IS_LINUX() (pthread_self() == pthread_linux())
#  define rt_spinlock_t pthread_spinlock_t
#  define RT_SPINLOCK_INITIALIZER PTHREAD_SPINLOCK_INITIALIZER
#  define rt_spin_lock_irqsave(x) pthread_spin_lock(x) /* returns flags */
#  define rt_spin_unlock_irqrestore(flags, x) do { (void)flags; pthread_spin_unlock(x); }
#  define rt_spin_lock_init(lock) pthread_spin_init(lock, 0 )
#  define rt_spin_lock_destroy(lock) pthread_spin_destroy(lock)
#elif defined(RTAI) && !defined(RTLINUX) /* RTAI */

#  include <rtai.h>
#  include <rtai_sched.h>
#  include <rtai_fifos.h>
#  include <rtai_posix.h>
#  include <rtai_shm.h>
#  include <rtai_sem.h>
#  define rt_spinlock_t spinlock_t
#  define RTF_NO MAX_FIFOS
#  define mbuff_alloc(name, size) rt_shm_alloc(nam2num(name), size, USE_VMALLOC)
#  define mbuff_free(name, ptr) rt_shm_free(nam2num(name))
#  define mbuff_attach(name, size) rt_shm_alloc(nam2num(name), size, USE_VMALLOC)
#  define mbuff_detach(name, ptr) rt_shm_free(nam2num(name))
   typedef RTIME hrtime_t;
   extern spinlock_t FSM_GLOBAL_SPINLOCK;
#  define rt_critical(flags) do { flags = rt_spin_lock_irqsave(&FSM_GLOBAL_SPINLOCK); } while (0)
#  define rt_end_critical(flags) do { rt_spin_unlock_irqrestore(flags, &FSM_GLOBAL_SPINLOCK); } while (0)
#  define RTF_FREE(f) (-1) /* unsupported in RTAI */
   static inline hrtime_t gethrtime(void) { return count2nano(rt_get_time()); }
#  define sched_get_priority_max(x) MAX_PRIO
#  define sched_get_priority_min(x) MIN_PRIO
#  define CURRENT_IS_RT() (rt_get_prio(rt_whoami()) >= 0)
#  define CURRENT_IS_LINUX() (rt_get_prio(rt_whoami()) < 0)
#  define rt_spinlock_t spinlock_t
#  define RT_SPINLOCK_INITIALIZER SPIN_LOCK_UNLOCKED
#  define rt_spin_lock_init(lock) spin_lock_init(lock)
#  define rt_spin_lock_destroy(lock) ((void)lock)
#else
#  error Need to define exactly one of RTLINUX or RTAI to use this header!
#endif

/** find_free_rtf - Tries to find the first free Realtime-Fifo.
 * @param minor - an out parameter -- the unsigned int to save the found fifo minor to.  On error, value is undefined.
 * @param size - the desired size of the fifo
 * @return 0 on succes, or a -errno value if there is a problem allocating or finding the fifo.
 */
#ifdef RTLINUX
static int rtf_find_free(unsigned *minor, unsigned size)
{
  unsigned i;
  for (i = 0; i < RTF_NO; ++i) {
    int ret = rtf_create(i, size);
    if ( ret  == 0 ) {
      *minor = i;
      return 0;
    } else if ( ret != -EBUSY ) 
      /* Uh oh.. some deeper error occurred rather than just the fifo was
	 already allocated.. */
      return ret;
  }
  return -EBUSY;
  rtf_find_free(minor, size); /* avoid compiler warnings about unused.. */
}
#else /* RTAI gah! it allows multiple opens on same fifo, so we must figure out what is opened another way */
static int rtf_find_free(unsigned *minor, unsigned size)
{
  unsigned i;
  char dummybuf[8];
  for (i = 0; i < RTF_NO; ++i) {
    int ret = rtf_evdrp(i, dummybuf, sizeof(dummybuf)); /* eavesdrop returns <0 if the fifos is invalid..  which means it's free! */
    if ( ret  < 0 ) {
      ret = rtf_create(i, size);
      if (!ret) *minor = i;
      return 0;
    }
  }
  return -EBUSY;
  rtf_find_free(minor, size); /* avoid compiler warnings about unused.. */
}
#endif

#ifdef RTAI
#  if RTAI_VERSION_CODE == RTAI_MANGLE_VERSION(3,6,0) || RTAI_VERSION_CODE == RTAI_MANGLE_VERSION(3,6,1)
/* Since RTAI headers are broken, we reimplement pthread_create here, and define pthread_create to be rtfsm_pthread_create */
static int rtfsm_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
	pthread_cookie_t *cookie = 0;
    void *cookie_mem = 0;
	cookie_mem = (void *)rt_malloc(sizeof(pthread_cookie_t) + L1_CACHE_BYTES);
	if (cookie_mem) {
        int err;
        /* align memory for RT_TASK to L1_CACHE_BYTES boundary */
        cookie = (pthread_cookie_t *)( (((unsigned long)cookie_mem) + ((unsigned long)L1_CACHE_BYTES)) & ~(((unsigned long)L1_CACHE_BYTES) - 1UL) );

		cookie->cookie = cookie_mem;
		(cookie->task).magic = 0;
		cookie->task_fun = (void *)start_routine;
		cookie->arg = (long)arg;
		if (!(err = rt_task_init(&cookie->task, (void *)posix_wrapper_fun, (long)cookie, (attr) ? attr->stacksize : STACK_SIZE, (attr) ? attr->priority : RT_SCHED_LOWEST_PRIORITY, 1, NULL))) {
			rt_typed_sem_init(&cookie->sem, 0, BIN_SEM | FIFO_Q);
			rt_task_resume(&cookie->task);
			*thread = &cookie->task;
			return 0;
		} else {
            rt_free(cookie->cookie);
            rt_printk(KERN_ERR "rtfsm_pthread_create(): error %d from rt_task_init\n", err);
            return err;
        }
	}
    rt_printk(KERN_ERR "rtfsm_pthread_create(): could not allocate %lu bytes for cookie!\n", sizeof(pthread_cookie_t)+L1_CACHE_BYTES);
	return -ENOMEM;
    rtfsm_pthread_create(0, 0, 0, 0); /* avoid compiler warnings about unused.. */
}
#    define pthread_create rtfsm_pthread_create
#  else
#    warning POSSIBY UNSUPPORTED RTAI VERSION!  We require something like rtai 3.6.0 or 3.6.1 to workaround pthread_create() bugs.. otherwise use at your own risk!
#  endif
#endif /* ifdef RTAI */


#endif
