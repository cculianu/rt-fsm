#ifndef SOUND_TRIG_H
#  define SOUND_TRIG_H

#include "IntTypes.h"

#ifdef __cplusplus
#include <string.h>
extern "C" {
#endif

#define SNDNAME_SZ (8) 
#define MAX_SND_ID 0x80

#define MAX_CARDS 6 /* Keep this the same as L22_MAX_DEVS in LynxTWO-RT.h! */

enum FifoMsgID {
    GETPAUSE,  /**< Query to find out if it is paused. */
    PAUSEUNPAUSE, /**< Halt the trigger detect (temporarily).  No variables
                     are cleared but events cannot generate new snd play. */
    INITIALIZE, /**< Reset/Initialize. */
    GETVALID,  /**< Query to find out if it has any valid sound.*/
    INVALIDATE, /**< Invalidate (clear) the state machine specification, 
                   but preserve other system variables. */
    SOUND, /**< Load a sound. */    
    /*    GETSOUND, */
    FORCEEVENT, /**< Force a particular event to have occurred. */
    GETLASTEVENT, /**< Query the event we are playing/last played, if any.. */
    GETRUNTIME, /**< The time we have been running.. */
    ALLOCSOUND, /**< Allocate space for a sound buffer in Kernel */
    SOUNDTRIG_MSG_ID_MAX
};

struct FifoMsg {
    int id; /**< One of FifoMsgID above.. */
    
    /** Which element of union is used depends on id above. */
    union {

      /** For id == SOUND || id == ALLOCSOUND */
      struct {
        /* Sound Specification */   
        char name[SNDNAME_SZ]; /**< The name of the shm to attach to */
        unsigned long size; /**< The size of the shm to attach to */
        
        unsigned bits_per_sample; /**< 8, 16, 24, or 32 are allowed */
        unsigned chans; /**< 1 or 2 */
        unsigned rate; /**< 8kHz - 200 kHz */
        unsigned id; /**< The trigger id.  This corresponds to the digital lines 
                          used to trigger the sound.. */
        unsigned stop_ramp_tau_ms; /**< The number of ms to do the cosine-squared
                                      amplitude ramp-down when stopping a sound
                                      prematurely. 0 to disable this. */
        int is_looped; /**< if true, this sound should play in a loop whenever
                          it is triggered.  Otherwise the sound will stop
                          normally after 1 playing. */
        int transfer_ok; /**< True is written by kernel to indicate transter was 
                            ok, otherwise writes 0 to indicate error..  */
      } sound;

      /** For id == GETPAUSE */
      unsigned is_paused; 

      /** For id == GETVALID */
      unsigned is_valid;

      /** For id == FORCEEVENT */
      int forced_event;

      /** For id == GETRUNTIME */ 
      int64 runtime_us; /**< Time since last reset, in seconds */

      /** For id == GETLASTEVENT */ 
      int last_event; /**< The current/last event, or 0 if none */

    } u;
#ifdef __cplusplus
    FifoMsg() { memset(this, 0, sizeof(*this)); }
#endif
  };
  
  /**
   * The shared memory for RT sound triggering.  Contains minors for in/out
   * fifos as well as the FifoMsg struct itself. */
  struct Shm
  { 
    int    magic;            /**< Should always equal SHMRT_MAGIC  */
    int fifo_out[MAX_CARDS]; /**< The kernel-to-user FIFO */
    int fifo_in[MAX_CARDS];  /**< The user-to-kernel FIFO */
    
    struct FifoMsg msg[MAX_CARDS]; /**< When fifo_in gets an int, this value is read
                           by kernel-process.  (The alternative would have been
                           to write this msg to a FIFO but that's a lot of
                           wasteful double-copying.  It's faster to use
                           the shm directly, and only use the FIFO for 
                           synchronization and notification.                 */
    volatile unsigned long ctr; /**< Used to generate unique sound buffer shm names */
    const unsigned num_cards;
  };


  typedef int FifoNotify_t;  /**< Write one of these to the fifo to notify
                                that a new msg is available in the SHM       */
#define FIFO_SZ (sizeof(FifoNotify_t))

#ifndef __cplusplus
  typedef struct Shm Shm;
#endif

#define SND_SHM_NAME "STrigShm"
#define SND_SHM_MAGIC ((int)(0xf00d0609)) /*< Magic no. for shm... 'food0609'  */
#define SND_SHM_SIZE (sizeof(struct Shm))

#ifdef __cplusplus
}
#endif

#endif
