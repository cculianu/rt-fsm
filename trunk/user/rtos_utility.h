/*
 * This file is part of the RT-Linux Multichannel Data Acquisition System
 *
 * Copyright (C) 1999-2003 David Christini, Calin Culianu
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see COPYRIGHT file); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA, or go to their website at
 * http://www.gnu.org.
 */
#ifndef RTOS_UTILITY_H
#define RTOS_UTILITY_H

#include <sys/types.h>
#include <string>

namespace RTOS
{
  enum RTOS {
    Unknown = 0,
    None = Unknown,
    RTLinux, 
    RTAI    
  };

  extern RTOS determine();  
  extern const char *name(); // returns the string name of the current RTOS

  enum ShmStatus {
    Ok = 0,
    WrongSize,
    NotFound,
    InvalidDevFile,
    MissingDevFile,
    NoRTOSFound
  };
  
  extern void *shmAttach(const char *shm_name, size_t size, ShmStatus *s=0, bool create = false);
  extern void shmDetach(const void *shm, ShmStatus *status=0, bool destroy = false);

  /* true iff /dev/mbuff or /dev/rtai_shm is valid and openable for RDWR */
  extern bool shmDevFileIsValid(); 
  /* true iff /dev/mbuff or /dev/rtai_shm at least exists */
  extern bool shmDevFileExists(); 

  extern const char *shmDevFile();  // returns filename string of shm dev file
  extern const char *shmDriverName(); // returns name of shm driver

  struct Fifo;
  typedef Fifo * FIFO;
  extern Fifo * const INVALID_FIFO;

  // opens /dev/rtf[minor no] and returns its fd or -errno on error            
  // in emulator mode actually opens an IPC handle
  enum ModeFlag { Read = 1, Write = 2, ReadWrite = Read|Write };
  extern FIFO openFifo(unsigned key_no, ModeFlag m = Read, std::string *errmsg = 0); 
  extern void closeFifo(FIFO);
  extern void closeFifo(unsigned key);
#ifdef EMULATOR
  extern FIFO createFifo(unsigned & key_out, unsigned size);
#endif
  extern int readFifo(FIFO, void *buf, unsigned long bufsz, bool blocking = true);
  extern int writeFifo(FIFO, const void *buf, unsigned long bufsz, bool blocking = true);
  extern int readFifo(unsigned key, void *buf, unsigned long bufsz, bool blocking = true);
  extern int writeFifo(unsigned key, const void *buf, unsigned long bufsz, bool blocking = true);
  extern int fifoNReadyForReading(FIFO);
  extern int fifoNReadyForReading(unsigned);

  // /dev/rtf in RTOS mode, if emulator, \\.\pipe\fsm_pipe_no_ for win32
  // or /tmp/fsm_pipe_no_ otherwise
  extern const char *fifoFilePrefix(int rtos = -1);

  extern const char *statusString(ShmStatus s);

  // call this to explicitly close all fifos -- will free them regardless
  // of reference count!
  extern void destroyAllFifos();
};

#endif
