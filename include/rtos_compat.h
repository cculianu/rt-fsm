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
#elif defined(RTAI) && !defined(RTLINUX) /* RTAI */
#  include <rtai.h>
#  include <rtai_sched.h>
#  include <rtai_fifos.h>
#  include <rtai_posix.h>
#  include <rtai_shm.h>
#  define RTF_NO MAX_FIFOS
#  define mbuff_alloc(name, size) rt_shm_alloc(nam2num(name), size, USE_VMALLOC)
#  define mbuff_free(name, ptr) rt_shm_free(nam2num(name))
#  define mbuff_attach(name, size) rt_shm_alloc(nam2num(name), size, USE_VMALLOC)
#  define mbuff_detach(name, ptr) rt_shm_free(nam2num(name))
   typedef RTIME hrtime_t;
   extern spinlock_t FSM_GLOBAL_SPINLOCK;
   #define rtl_critical(flags) do { flags = rt_spin_lock_irqsave(&FSM_GLOBAL_SPINLOCK); } while (0)
   #define rtl_end_critical(flags) do { rt_spin_unlock_irqrestore(flags, &FSM_GLOBAL_SPINLOCK); } while (0)
   #define RTF_FREE(f) (-1) /* unsupported in RTAI */
   static inline hrtime_t gethrtime(void) { return count2nano(rt_get_time()); }
   #define sched_get_priority_max(x) MAX_PRIO
   #define sched_get_priority_min(x) MIN_PRIO
#else
#  error Need to define exactly one of RTLINUX or RTAI to use this header!
#endif

/** find_free_rtf - Tries to find the first free Realtime-Fifo.
 * @param minor - an out parameter -- the unsigned int to save the found fifo minor to.  On error, value is undefined.
 * @param size - the desired size of the fifo
 * @return 0 on succes, or a -errno value if there is a problem allocating or finding the fifo.
 */
#ifdef RTLINUX
static int find_free_rtf(unsigned *minor, unsigned size)
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
}
#else /* RTAI gah! it allows multiple opens on same fifo, so we must figure out what is opened another way */
static int find_free_rtf(unsigned *minor, unsigned size)
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
}
#endif


#endif
