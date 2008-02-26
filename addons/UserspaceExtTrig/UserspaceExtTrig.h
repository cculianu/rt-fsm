#ifndef USERSPACE_TRIG_H
#  define USERSPACE_TRIG_H

#include "IntTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 
    Just contains the fifo minor that userspace should open.
*/
struct UTShm
{ 
    int magic;               /**< Should always equal SHM_MAGIC */
    int fifo_out;            /**< The kernel-to-user FIFO */
};

/** This is the struct that gets written to the fifo from kernel to userspace*/
struct UTFifoMsg
{
    unsigned target;
    int      data;
};

#ifndef __cplusplus
  typedef struct UTShm UTShm;
  typedef struct UTFifoMsg UTFifoMsg;
#endif

#define UT_FIFO_SZ (sizeof(UTFifoMsg)*100)

#define UT_SHM_NAME "UTShmNRT"
#define UT_SHM_MAGIC ((int)(0xf00d0617)) /*< Magic no. for shm... 'food0617'  */
#define UT_SHM_SIZE (sizeof(struct UTShm))

#ifdef __cplusplus
}
#endif

#endif
