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
#ifndef FSM_EXT_TRIG_H
#define FSM_EXT_TRIG_H

#ifndef __KERNEL__
#   error This is a kernel header, and is not really compatible or intended to be used by userspace programs!
#endif

#include <linux/module.h>
#include <asm/atomic.h>

typedef int (* FSMExtTrigFn_t)(unsigned, int); 
struct FSMExtTrigShm
{
  int magic;
  atomic_t valid;
  FSMExtTrigFn_t function;
};

/** The shm will contain a function pointer that should be
    called in RT context to do triggers! */
#define FSM_EXT_TRIG_SHM_NAME "FTRGShm"
#define FSM_EXT_TRIG_SHM_SIZE sizeof(struct FSMExtTrigShm)
#define FSM_EXT_TRIG_SHM_MAGIC 0x220bce03


static inline int FSM_EXT_TRIG_SHM_IS_VALID(volatile struct FSMExtTrigShm *shm)
{
  return  shm && shm->magic == FSM_EXT_TRIG_SHM_MAGIC
          && atomic_read(&shm->valid) && shm->function;
}

#if defined(MOD_INC_USE_COUNT) && defined(MOD_DEC_USE_COUNT)
/* Kernel 2.4 mechanism allowed for simple atomic increase of module use count
   to avoid race conditions.  */
#  define MINC MOD_INC_USE_COUNT
#  define MDEC MOD_DEC_USE_COUNT
#else 
/* Kernel 2.6 uses a different mechanism .. which *ISN'T* callable from RT-Context so we
   have to scrap this whole thing!
   FIXME: figure out how to do proper module increment counts here to avoid losing the module
          from underneath us! */
#  define MINC do {  } while(0)
#  define MDEC do {  } while(0)
#endif

static inline void FSM_EXT_TRIG(volatile struct FSMExtTrigShm *shm, unsigned target, int trig)
{
  if (FSM_EXT_TRIG_SHM_IS_VALID(shm)) 
  {
      MINC;
      shm->function(target, trig); 
      MDEC;
  }
  return;
}

static inline void FSM_EXT_UNTRIG(volatile struct FSMExtTrigShm *shm, unsigned target, int trig) 
{
  /* Make sure trig is negative.. */
  FSM_EXT_TRIG(shm, target, trig < 0 ? trig : -trig);
  return;
  FSM_EXT_UNTRIG(0, target, 0); /* Avoid compiler warnings for unused.. */
}

#endif
