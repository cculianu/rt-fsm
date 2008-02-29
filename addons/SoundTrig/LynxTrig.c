/*
 * Copyright (C) 2005-2008 Calin Culianu <calin@ajvar.org>
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

/**
 * Trigger RT program that knows how to talk to the LynxTWO-RT driver
 * for triggering sounds.  To avoid any latency, all soundfiles are
 * stored in memory, and they come from userspace via a SHM buff.  Our only 
 * hope is that the rtos doesn't limit the size and number of RT_SHMs.
 * On RTAI this shouldn't be much of a problem...
 */

#include <linux/module.h> 
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <asm/semaphore.h> /* for synchronizing the exported functions */
#include <asm/bitops.h>    /* for set/clear bit                        */
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h> /* some memory copyage                       */
#include <linux/fs.h> /* for the determine_minor() functionality       */
#include <linux/proc_fs.h>
#include <asm/div64.h> /* for do_div 64-bit division macro             */
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/comedilib.h>
#include <linux/seq_file.h>
#include <asm/system.h>

#include "rtos_compat.h"
#include "IntTypes.h"
#include "SoundTrig.h"
#include "FSMExternalTrig.h"
#include "LynxTWO-RT.h"
#include "ChanMapper.h"
#include "softtask.h"

#ifdef RTAI
/* this is a noop on rtai -- all rt tasks use fpu.  */
static inline int pthread_attr_setfp_np(pthread_attr_t *a, int b) {(void)(a); (void)(b); return 0; }
#endif

#if MAX_CARDS != L22_MAX_DEVS
#error  MAX_CARDS needs to equal L22_MAX_DEVS.  Please fix either LynxTWO-RT.h or SoundTrig.h to make these #defines equal!
#endif

#define MODULE_NAME KBUILD_MODNAME
#ifdef MODULE_LICENSE
  MODULE_LICENSE("GPL");
#endif
#ifndef STR
#  define STR1(x) #x
#  define STR(x) STR1(x)
#endif
#ifndef ABS
#  define ABS(x) ( (x) < 0 ? -(x) : (x) )
#endif
#define LOG_MSG(x...) rt_printk(KERN_INFO MODULE_NAME": "x)
#define DEBUG(x...)  do { if(debug) { rt_printk(KERN_DEBUG MODULE_NAME":(thr: %p)", (void *)pthread_self()); rt_printk(" DEBUG: "x); } } while(0)
#define WARNING(x...)  rt_printk(KERN_WARNING MODULE_NAME ": WARNING: "x)
#define ERROR(x...)  rt_printk(KERN_ERR MODULE_NAME ": INTERNAL ERROR: "x)

MODULE_AUTHOR("Calin A. Culianu");
MODULE_DESCRIPTION(MODULE_NAME ": A component for the rtfsm to trigger the Lynx22 board (requires the LynxTWO-RT driver).");

typedef unsigned CardID_t;

int init(void);  /**< Initialize data structures and register callback */
void cleanup(void); /**< Cleanup.. */

static int initRunStates(void);
static int initRunState(CardID_t);
static int initShm(void);
static int initFifos(void);
static int initRT(void);
static int initComedi(void);

static void *doRT (void *);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,29)

/* Called whenever the /proc/LynxTrig proc file is read */
static int myseq_show (struct seq_file*, void *data);
static int myseq_open(struct inode *, struct file *);
static struct file_operations myproc_fops =
{
  open:    myseq_open,
  read:    seq_read,
  llseek:  seq_lseek,
  release: single_release
};

#endif

/* Spawns one of the below threads in software triggers mode, or 
   calls the function directly in hardware triggers mode.. 
   Pass a command of the type seen in enum FifoMsgID */
enum {
  PLAY_CMD = SOUNDTRIG_MSG_ID_MAX+1,
  STOP_CMD,
  ALLOC_KMEM_CMD,
  FREE_KMEM_CMD,
  ALLOC_MBUFF_CMD,
  FREE_MBUFF_CMD,
  DETACH_MBUFF_CMD,
  ATTACH_MBUFF_CMD
};
static void *doCmdInRTThread(int cmd, void *arg);
static void *doCmdInSoftThread(int cmd, void *arg);


module_init(init);
module_exit(cleanup);


/*---------------------------------------------------------------------------- 
  Some private 'global' variables...
-----------------------------------------------------------------------------*/
static volatile struct Shm *shm = 0;
static volatile struct FSMExtTrigShm *virtShm = 0;

/** array of L22Dev_t for all the devices we have open */
L22Dev_t *l22dev = 0;
/** the size of the above array */
unsigned n_l22dev = 0;
        
#define DEFAULT_SAMPLING_RATE 10000
#define DEFAULT_TRIGGER_MS 1

static char COMEDI_DEVICE_FILE[] = "/dev/comediXXXXXXXXXXXXXXX";
int minordev = 0, 
    sampling_rate = DEFAULT_SAMPLING_RATE, 
    debug = 0,
    first_trig_chan = 0,
    num_trig_chans = 8,
    comedi_triggers = 0,
    trig_on_event_chan = -1;
module_param(minordev, int, 0444);
MODULE_PARM_DESC(minordev, "The minor number of the comedi device to use.");
module_param(sampling_rate, int, 0444);
MODULE_PARM_DESC(sampling_rate, "The sampling rate, or frequency of the periodic polling task if using comedi_triggers.  Note: Do not confuse this with the sound board's sampling rate which is specified via the userspace program LynxTrigServer. Defaults to " STR(DEFAULT_SAMPLING_RATE) ".");
module_param(debug, int, 0444);
MODULE_PARM_DESC(debug, "If true, print extra (cryptic) debugging output.  Defaults to 0.");
module_param(first_trig_chan, int, 0444);
MODULE_PARM_DESC(first_trig_chan, "The first DIO channel to use for board triggers.  Note that this plus num_trig_chans determine exact lines we scan to get the binary trigger id.  Defaults to 0.");
module_param(num_trig_chans, int, 0444);
MODULE_PARM_DESC(num_trig_chans, "The number of DIO channels from first_trig_chan to scan for triggers. Defaults to 8.  (0 means take all channels left until the last).");
module_param(comedi_triggers, int, 0444);
MODULE_PARM_DESC(comedi_triggers, "If set, use COMEDI DIO device to do triggering.  Otherwise use a SHM which contains a singe int for 'virtual' triggering from another RT module.  The SHM name is \""FSM_EXT_TRIG_SHM_NAME"\".  Defaults to 0 (virtual).");
module_param(trig_on_event_chan, int, 0444);
MODULE_PARM_DESC(trig_on_event_chan, "If non-negative, the DIO channel (not related to first_trig_chan or num_trig_chans at all!) to 'trigger' for one cycle whenever a play event occurs. This feature is intended to be used for debugging the timing of the play command. Defaults to -1 (off).");

#define NUM_CHANS_REQUIRED (first_trig_chan+(num_trig_chans-1))

static struct proc_dir_entry *proc_ent = 0;
static atomic_t rt_task_stop = ATOMIC_INIT(0); /* Internal variable to stop the RT 
                                         thread. */
static volatile int rt_task_running = 0;
static pthread_t rttask = 0;

static struct RamperThread
{
  pthread_t thread;
  atomic_t tauParam;
  atomic_t stop;
  sem_t sem;
  unsigned dev; /* The L22 device this thread is responsible for */
  unsigned chan; /* The L22 channel this thread is responsible for */
} ramperThreads[L22_MAX_DEVS][L22_NUM_CHANS]; /**< TODO/FIXME put this in RunState */

struct DevChan 
{
  union {
    struct {
      unsigned short dev;
      unsigned short chan;
    };
    unsigned devchan;
    void *devchan_voidp;
  };
};
typedef struct DevChan DevChan;

struct DevEvt
{
  union {
    struct {
      unsigned short dev;
      short evt;
    };
    unsigned devevt;
    void *devevt_voidp;
  };
};
typedef struct DevEvt DevEvt;


static comedi_t *dev = 0;
static unsigned subdev = 0;

/* Remembered state of all DIO channels.  */
unsigned dio_bits = 0, dio_bits_prev = 0;  
uint64 cycle = 0; /* the current cycle */

#define MILLION 1000000
#define BILLION 1000000000
uint64 task_period_ns = BILLION;

#define Sec2Nano(s) (s*BILLION)
static inline unsigned long Nano2Sec(unsigned long long nano);
static inline unsigned long Nano2USec(unsigned long long nano);
static inline long Micro2Sec(long long micro, unsigned long *remainder);
/*---------------------------------------------------------------------------- 
  More internal 'global' variables and data structures.
-----------------------------------------------------------------------------*/
struct AudioBuf {
  unsigned char *data; /**< pointer to audio data -- usually an rt-shm */
  char name[SNDNAME_SZ]; /**< used for attaching/detaching the shm */
  unsigned long len; /**< The length of the buffer, in bytes */
  unsigned rate;
  unsigned short bits;
  unsigned short nchans;
  unsigned short stop_ramp_tau_ms;
  unsigned short is_looped_flag;
  unsigned trig_play_ct;
  unsigned trig_stop_ct;
};

struct RunState {
    /* NB: this is not SMP-safe and also suffers from race conditions 
       with userspace in case another CPU writes to these values on behalf of 
       userspace writes to these values.. TODO: add smp-safe locks.          */
    
    int64 current_ts; /* Time elapsed, in ns, since init_ts */
    int64 init_ts; /* Time of initialization, in nanoseconds.  Absolute
                      time derived from the Pentium's monotonically 
                      increasing TSC. */            

    int forced_event; /* If positive, force this trigger. */
    int last_event;    /* The last event.. 0 for none, but if anything ever 
			  happeened it's usually nonzero.  Negative for a 
			  stop event. */
    int current_event ; /* The currently playing event.. quickly settles to 0 */
  
    int paused; /* If this is true, input lines do not lead to 
		   sound playing. Useful when we want to temporarily 
		   pause triggering, etc. */

    int invalid; /* If this is true, the kernel ignores the sound file(s).
                    Useful when userspace wants to 'upload' a new sound file.*/

    struct AudioBuf *last_played_buf[L22_NUM_CHANS];
    struct AudioBuf audio_buffers[MAX_SND_ID];    
};

static struct RunState rs[L22_MAX_DEVS];

#define SOFTWARE_TRIGGERS()  (!comedi_triggers)
#define COMEDI_TRIGGERS() (comedi_triggers != 0)
static DECLARE_MUTEX(trigger_mutex);
/** Note locking only happens in non-rt context (SOFTWARE_TRIGGERS() mode).
    In COMEDI_TRIGGERS() mode, we don't lock anything as all trigger access 
    is synchronous. */
#define LOCK_TRIGGERS() do { if (SOFTWARE_TRIGGERS()) down_interruptible(&trigger_mutex); } while(0)
#define UNLOCK_TRIGGERS() do { if (SOFTWARE_TRIGGERS()) up(&trigger_mutex); } while(0)
#define TRIGGERS_LOCKED() (SOFTWARE_TRIGGERS() && atomic_read(&trigger_mutex.count) <= 0)

/*---------------------------------------------------------------------------
 Some helper functions
-----------------------------------------------------------------------------*/
static int detectEvent(CardID_t); /**< returns 0 on no input detected, positive for play id or negative for stop id */
static pthread_mutex_t dispatch_Mut;
static void dispatchEvent(CardID_t, int trig); /**< Do triggers (if any).. */
static int virtualTriggerFunction(CardID_t, int); /* Do virtual 'soft' triggers (if any) */      
static int createRamperThreads(void);
static void destroyRamperThreads(void);
static void doPossiblyRampedStop(unsigned card, unsigned chan);
static void *cosine2RamperThread(void *);
static void cancelAnyRunningRampedStops(unsigned card, unsigned chan);

static int fifoHandler0(unsigned f);
static int fifoHandler1(unsigned f);
static int fifoHandler2(unsigned f);
static int fifoHandler3(unsigned f);
static int fifoHandler4(unsigned f);
static int fifoHandler5(unsigned f);
static int (*handlerFuncMap[])(unsigned) = { fifoHandler0, fifoHandler1, fifoHandler2, fifoHandler3, fifoHandler4, fifoHandler5 };
static int handleFifos_MapCardID(unsigned int fifo_no); /* gets called as rtf handler, maps rtf id to card id and calls handleFifos(cardId */
static int handleFifos(CardID_t card);
static void grabAllDIO(void);
static inline void updateTS(CardID_t c) { rs[c].current_ts = gethrtime() - rs[c].init_ts; }
static void freeAudioBuffer(struct AudioBuf *);
static void freeAudioBuffers(CardID_t c);
#ifdef RTLINUX
/* just like clock_gethrtime but instead it used timespecs */
static inline void clock_gettime(clockid_t clk, struct timespec *ts);
static inline void nano2timespec(hrtime_t time, struct timespec *t);
#endif
/* 64-bit division is not natively supported on 32-bit processors, so it must 
   be emulated by the compiler or in the kernel, by this function... */
static inline unsigned long long ulldiv_(unsigned long long dividend, unsigned long divisor, unsigned long *remainder);
/* Just like above, but does modulus */
static inline unsigned long ullmod_(unsigned long long dividend, unsigned long divisor);
/* this function is non-reentrant! */
static const char *uint64_to_cstr(uint64 in);

static struct Channels *channels[MAX_CARDS];
static int initChanMapper(void); 
static void destroyChanMapper(void);
static void stopAllChans(CardID_t c);
static int initL22Devs(void);
static void cleanupL22Devs(void);
static int initSems(void);
static void cleanupSems(void);
static sem_t *get_unused_sem(void);
static struct SoftTask *get_unused_st(void);
static int initSTs(void);
static void cleanupSTs(void);
#define NUM_SOFTS_SEMS 16 /* the number of semaphores and softtasks we open up at module load time.. */
static sem_t misc_sems[NUM_SOFTS_SEMS];
static atomic_t misc_sem_idx = ATOMIC_INIT(0);
static pthread_mutex_t misc_sem_mut;
static int setup_sems = 0;

static struct SoftTask *soft_tasks[NUM_SOFTS_SEMS] = {0};
static atomic_t soft_task_idx = ATOMIC_INIT(0);
static pthread_mutex_t soft_task_mut;
static int setup_soft_tasks = 0;

static void mrSoftTask(void *arg); /**< soft task handler for soft_task */
typedef struct AllocArg {
    const char *name;
    union {
        unsigned long volatile size;
        void * volatile ptr;
    };
} AllocArg;
static void *doAllocMbuffCmd(void *);
static void *doAllocKmemCmd(void *);
static void *doFreeMbuffCmd(void *);
static void *doFreeKmemCmd(void *);
static void *doAttachMbuffCmd(void *);
static void *doDetachMbuffCmd(void *);
/** Used to keep track of rt-shm's allocated by ALLOCSOUND but not yet 
 *  used in SOUND cmds.  This is to make sure that if userspace crashes it doesn't
 *  leave around  shms.. */
typedef struct ShmList
{
    struct list_head list;
    char name[SNDNAME_SZ];
    void *mem;
    unsigned long size;
} ShmList;
static struct list_head shmList = LIST_HEAD_INIT(shmList);
static pthread_mutex_t shmListMut;
/** Call in any context -- rt or not -- auto-allocates shm in rt-safe manner and returns a pointer to it*/
static void *ShmListNew(const char *name, unsigned long size);
/** Call in any context -- rt or not -- see if a particular shm is in our list */
static void *ShmListFind(const char *name, unsigned long size);
/** Call in any context-- rt or not -- deletes a specific mem form the shm list and deallocates the rtshm */
static void ShmListDel(void *mem);
/** Call from module_exit -- cleans up any orphaned shms. */
static void ShmListCleanup(void);
        
/*-----------------------------------------------------------------------------*/

int init (void)
{
  int retval = -ENOMEM;
  
  pthread_mutex_init(&dispatch_Mut, 0); /* do this regardles.. */
  pthread_mutex_init(&shmListMut, 0); /* do this regardles.. */
    
#ifdef RTAI
  DEBUG("linux prio: %x\n", rt_get_prio(rt_whoami()));
#endif
  if (    (retval = initSTs())
       || (retval = initSems())
       || (retval = initL22Devs())
       || (retval = initChanMapper())
       || (retval = initShm())
       || (retval = initFifos())
       || (retval = initRunStates())
       || (retval = initComedi())
       || (retval = initRT())  ) 
    {
      cleanup();  
      return -ABS(retval);
    }
#ifdef RTAI
  if (!rt_is_hard_timer_running()) {
    rt_set_oneshot_mode();
    start_rt_timer(0); /* 0 period means what? its ok for oneshot? */
  }
  /* Must mark linux as using FPU because of C code startup? */
  rt_linux_use_fpu(1);
#endif
  
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,29)
 
  proc_ent = create_proc_entry(MODULE_NAME, S_IFREG|S_IRUGO, 0);
  if (proc_ent)  {/* if proc_ent is zero, we silently ignore... */
    proc_ent->owner = THIS_MODULE;
    proc_ent->uid = 0;
    proc_ent->proc_fops = &myproc_fops;
  }

#endif

  if ( SOFTWARE_TRIGGERS() ) {
    LOG_MSG("started successfully, using passive 'virtual' triggers at SHM \"%s\", for a total of 2^31 possible triggered sounds!\n", FSM_EXT_TRIG_SHM_NAME);
  } else {
    LOG_MSG("started successfully at %d Hz, using COMEDI DIO for triggering, first trig: %d, num trigs: %d.\n", sampling_rate, first_trig_chan, num_trig_chans);
  }

  if (virtShm)  /* virtShm not set if COMEDI_TRIGGERS() */
    atomic_set(&virtShm->valid, 1); 
  
  return retval;
}

void cleanup (void)
{
  unsigned c;
  
  LOCK_TRIGGERS();

  for (c = 0; c < n_l22dev; ++c) stopAllChans(c);
  
  /* TODO: Worry about possible race conditions with RT thread in
     virtualTriggerFunction() (it's possible for an RT thread to 
     be in that function here?) */
  if (virtShm) {
    atomic_set(&virtShm->valid, 0);
    virtShm->function = 0; /* Clear the function pointer just in case other
			      rt tasks are looking at it. */
    virtShm->magic = 0;
    mbuff_free(FSM_EXT_TRIG_SHM_NAME, (void *)virtShm);
    virtShm = 0;
  }

  if (proc_ent)
    remove_proc_entry(MODULE_NAME, 0);
  
  if (rt_task_running) {
    DEBUG("about to stop rt-task..\n");
    atomic_set(&rt_task_stop, 1);
    /*pthread_cancel(rttask);*/
    pthread_join(rttask, 0);
    DEBUG("stopped rt-task..\n");
  }
  
  destroyRamperThreads();

  if (dev) {
    comedi_close(dev);
    dev = 0;
  }

  if (shm)  { 
    for (c = 0; c < n_l22dev; ++c) {
      if (shm->fifo_in[c] >= 0) rtf_destroy(shm->fifo_in[c]);
      if (shm->fifo_out[c] >= 0) rtf_destroy(shm->fifo_out[c]);
    }
    memset((void *)shm, 0, sizeof(*shm));
    mbuff_free(SND_SHM_NAME, (void *)shm); 
    shm = 0; 
  }

  destroyChanMapper();

  if (COMEDI_TRIGGERS())
    LOG_MSG("unloaded successfully after %s cycles.\n",
            uint64_to_cstr(cycle));
  else
    LOG_MSG("unloaded successfully after %s virtual trigger events.\n", 
            uint64_to_cstr(cycle));

  UNLOCK_TRIGGERS();

  cleanupL22Devs();
  
  /* detach all shared mems for each card */
  for (c = 0; c < n_l22dev; ++c) 
    freeAudioBuffers(c); 
  
  ShmListCleanup(); /**< free any lingering 'orphaned' shms.. */
  
  /* these need to happen last or almost last as they are used by the of the functions above.. */
  cleanupSems();
  cleanupSTs();

  pthread_mutex_destroy(&shmListMut); 
  pthread_mutex_trylock(&dispatch_Mut);
  pthread_mutex_unlock(&dispatch_Mut);
  pthread_mutex_destroy(&dispatch_Mut);
}

static int initL22Devs(void)
{
  unsigned i, arr_bytes = 0;
  n_l22dev = L22GetNumDevices();
  if (!n_l22dev) { 
    LOG_MSG("No Lynx L22 devices found!\n"); 
    return -EINVAL; 
  }
  arr_bytes = sizeof(*l22dev)*n_l22dev;
  l22dev = kmalloc(arr_bytes, GFP_KERNEL);
  if (!l22dev) { 
    ERROR("Could not allocate memory for an internal buffer!\n"); 
    n_l22dev = 0; 
    return -ENOMEM; 
  }
  for (i = 0; i < n_l22dev; ++i) /* clear array.. */
    l22dev[i] = L22DEV_OPEN_ERROR;
  for (i = 0; i < n_l22dev; ++i) {
    if ( (l22dev[i] = L22Open(i)) == L22DEV_OPEN_ERROR ) {
      n_l22dev = i;
      return -EINVAL;
    }
  }
  return 0;
}

static void cleanupL22Devs(void)
{
  unsigned i;  
  if (!l22dev) return;
  for (i = 0; l22dev[i] != L22DEV_OPEN_ERROR && i < n_l22dev; ++i) 
    L22Close(l22dev[i]), l22dev[i] = L22DEV_OPEN_ERROR;
  kfree(l22dev);
  l22dev = 0;
}


static int initShm(void)
{
  unsigned i;

  shm = (volatile struct Shm *) mbuff_alloc(SND_SHM_NAME, SND_SHM_SIZE);
  if (! shm)  {
    ERROR("Could not mbuff_alloc the shared memory structure.  Aborting!\n");
    return -ENOMEM;
  }
  
  memset((void *)shm, 0, sizeof(*shm));
  shm->magic = SND_SHM_MAGIC;

  for (i = 0; i < MAX_CARDS; ++i)
    shm->fifo_out[i] = shm->fifo_in[i] = -1;

  if ( SOFTWARE_TRIGGERS() ) 
  {
      virtShm = (struct FSMExtTrigShm *) 
        mbuff_alloc(FSM_EXT_TRIG_SHM_NAME, FSM_EXT_TRIG_SHM_SIZE);
      if (!virtShm) return -ENOMEM;
      if (FSM_EXT_TRIG_SHM_IS_VALID(virtShm)) {
          ERROR("The FSM already has an external trigger handler installed!\n");
          return -EBUSY;
      }
      virtShm->magic = FSM_EXT_TRIG_SHM_MAGIC;
      virtShm->function = virtualTriggerFunction;
  }

  /* erm.. a const_cast basically */
  *((unsigned *)&shm->num_cards) = n_l22dev;

  return 0;
}

static int initFifos(void)
{
  int32 err, minor, i;

  for (i = 0; i < n_l22dev; ++i) {
    /* Open up fifos here.. */
    err = rtf_find_free(&minor, sizeof(FIFO_SZ));
    if (err < 0) return 1;
    shm->fifo_out[i] = minor;
    err = rtf_find_free(&minor, sizeof(FIFO_SZ));
    if (err < 0) return 1;
    shm->fifo_in[i] = minor;

    if ( SOFTWARE_TRIGGERS() ) 
      {
        /* In soft trigger mode, install an RT handler since there is no 
           rt-thread polling the fifos.. */
        if ( rtf_create_handler(shm->fifo_in[i], handlerFuncMap[i]) ) {
          LOG_MSG("rtf_create_handler returned error!\n");
          return 1;
        }
      }
  }

  return 0;  
}

static int initRunStates(void)
{
  unsigned i;
  for (i = 0; i < L22_MAX_DEVS; ++i) initRunState(i);
  return 0;
}

static int initRunState(CardID_t c)
{
  /* Clear the runstate memory area.. */
  memset((void *)&rs[c], 0, sizeof(rs[c]));

  /* Grab current time from rtlinux */
  rs[c].init_ts = gethrtime();

  rs[c].invalid = 1; /* Start out with an 'invalid' FSM since we expect it
                     to be populated later from userspace..               */

  return 0;  
}

static int initComedi(void) 
{
  int i, n_chans = 0;

  if ( SOFTWARE_TRIGGERS() ) {
    /* Virtual triggers, don't initialize comedi just setup the 
       trigger vars instead.. */
    return 0;
  }

  /* If we got to this point, we aren't using virtual triggers to open 
     comedi device, etc.. */

  if (first_trig_chan < 0 || num_trig_chans < 0) {
    LOG_MSG("first_trig_chan (%d) and num_trig_chans (%d) must be positive!",
            first_trig_chan, num_trig_chans);
    return -EINVAL;
  }

  if (!dev) {
    int sd;

    sprintf(COMEDI_DEVICE_FILE, "/dev/comedi%d", minordev);
    dev = comedi_open(COMEDI_DEVICE_FILE);
    if (!dev) {
      LOG_MSG("Cannot open Comedi device at %s\n", COMEDI_DEVICE_FILE);
      return -EINVAL/*comedi_errno()*/;
    }

    sd = comedi_find_subdevice_by_type(dev, COMEDI_SUBD_DIO, 0);    
    if (sd >= 0) n_chans = comedi_get_n_channels(dev, sd);
    if (num_trig_chans <= 0) num_trig_chans = (n_chans-first_trig_chan);
    if (sd < 0 || n_chans < first_trig_chan || n_chans < NUM_CHANS_REQUIRED) {
      LOG_MSG("DIO subdevice requires at least %d+%d DIO channels.\n", first_trig_chan, num_trig_chans-1);
      comedi_close(dev);
      dev = 0;
      return -ENODEV;
    }
    subdev = sd;
  }
  DEBUG("COMEDI: n_chans = %u\n", n_chans);
  
  /* Now, setup channel modes correctly */
  for (i = first_trig_chan; i < first_trig_chan+num_trig_chans; ++i) 
    if ( comedi_dio_config(dev, subdev, i, COMEDI_INPUT) != 1 )
      WARNING("comedi_dio_config returned error for channel %d mode %d\n", i, (int)COMEDI_INPUT);
      
  { 
    unsigned bits = 0;
    if ( comedi_dio_bitfield(dev, subdev, (unsigned)-1, &bits) < 0 ) 
      WARNING("comedi_dio_bitfield returned error\n");
  }

  if (trig_on_event_chan >= 0)
    comedi_dio_config(dev, subdev, trig_on_event_chan, COMEDI_OUTPUT); 

  return 0;
}

static int initRT(void)
{
  pthread_attr_t attr;
  struct sched_param sched_param;
  int error = 0;
  unsigned long rem;

  error = createRamperThreads();

  if (error) return -error;

  /* No rt task is run in 'virtual' or software trigger mode.. */
  if ( SOFTWARE_TRIGGERS() ) return 0;

  /* setup task period.. */
  if (sampling_rate <= 0 || sampling_rate >= BILLION) {
    LOG_MSG("Sampling rate of %d seems crazy, going to default of %d\n", sampling_rate, DEFAULT_SAMPLING_RATE);
    sampling_rate = DEFAULT_SAMPLING_RATE;
  }
  task_period_ns = ulldiv_(BILLION, sampling_rate, &rem);

  /* Setup pthread data, etc.. */
  pthread_attr_init(&attr);
  pthread_attr_setfp_np(&attr, 1);
  sched_param.sched_priority = 1;
  error = pthread_attr_setschedparam(&attr, &sched_param);  
  if (error) { error = -error; goto end_out; }
  error = pthread_create(&rttask, &attr, doRT, 0);  
  if (error) { error = -error; goto end_out; }
end_out:
  pthread_attr_destroy(&attr);
  return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,29)
static int myseq_open(struct inode *i, struct file *f)
{
  return single_open(f, myseq_show, 0);
}

static int myseq_show (struct seq_file *m, void *dummy)
{  
  unsigned c;
  (void) dummy;

  seq_printf(m, "%s Module\n\n", MODULE_NAME);
  
  seq_printf(m, "shm addr: %p\tshm magic: %08x\n", (void *)shm, shm->magic);

  for (c = 0; c < n_l22dev; ++c) {
    seq_printf(m, "\nSound Card %u:", c);
    if (rs[c].invalid) {
      seq_printf(m, "\tSound is not specified or is invalid.\n");
    } else {
      int i;
      seq_printf(m, "\n"); /* extra newline */
      for (i = 0; i < MAX_SND_ID; ++i) {
        struct AudioBuf *buf = &rs[c].audio_buffers[i];
        if (buf->data)
          seq_printf(m, "\tSound buffer %d: name: %s, buf_ptr: %p, rate - %u,  len - %lu,  bits - %hu,  nchans - %hu,  ramp_ms - %hu,  plays - %u,  stops - %u\n", i, buf->name, buf->data, buf->rate, buf->len, buf->bits, buf->nchans, buf->stop_ramp_tau_ms, buf->trig_play_ct, buf->trig_stop_ct);
      }      
    }
  }
  return 0;
}
#endif

/**
 * This function does the following:
 *  Detects edges, plays sounds via external LynxPlay(), updates Shm,
 *  reads from userspace fifos..
 */
static void *doRT (void *arg)
{
  struct timespec next_task_wakeup;
  (void)arg;

  rt_task_running = 1;

  clock_gettime(CLOCK_REALTIME, &next_task_wakeup);
  
  
  while (! atomic_read(&rt_task_stop) ) {

    /*#define TIME_IT*/

    timespec_add_ns(&next_task_wakeup, (long)task_period_ns);

    ++cycle;

    /* Grab current time from hard realtime clock */
    updateTS(0);
        
    /* Grab all available DIO bits at once (up to 32 bits due to limitations 
       in COMEDI) */
    grabAllDIO();
   
    handleFifos(0/*only card0 is in rt..*/);
    
    if ( ! rs[0].invalid && ! rs[0].paused ) {
      
      int event_id = 0;
	  
      /* Normal event transition code, event_id is non-negative on event 
         detection. */
      event_id = detectEvent(0);

      if (COMEDI_TRIGGERS() && trig_on_event_chan >= 0) 
	/* set this DIO line high to indicate an event detection, 
	   used to time the latency of the play command.. */
        comedi_dio_write(dev, subdev, trig_on_event_chan, event_id!=0); 
      
      dispatchEvent(0, event_id);
      
    }
    
    
    /* Sleep until next period */
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next_task_wakeup, 0); 
  }

  rt_task_running = 0;
  pthread_exit(0);
  return 0;
}


static void dispatchEvent(CardID_t c, int event_id)
{
  int chan = -1, snd_id;
  struct AudioBuf * buf;

  rs[c].current_event = 0;

  if (!event_id || ABS(event_id) >= MAX_SND_ID) return;

  DEBUG("dispatchEvent card=%u event_id=%d\n", c, event_id);
  
  rs[c].last_event = event_id;
  rs[c].current_event = event_id;


  if (event_id < 0) { /* Handle stop events.. */
      pthread_mutex_lock(&dispatch_Mut); 
      snd_id = -event_id;
      buf = &rs[c].audio_buffers[snd_id];
      chan = CM_LookupSound(channels[c], snd_id);
      if (chan > -1) doPossiblyRampedStop(c, chan);
      CM_ClearSound(channels[c], snd_id);
      ++buf->trig_stop_ct; /* increment trig stop count */
      pthread_mutex_unlock(&dispatch_Mut); 
      return;
  }
  snd_id = event_id; /* at this point the snd_id == the event_id */

  /* else..   Event_id > 0, play event.. */
  buf = &rs[c].audio_buffers[snd_id];

  pthread_mutex_lock(&dispatch_Mut); 

  chan = CM_GrabChannel(channels[c], snd_id);

  if (!buf->data || !buf->len) {
    DEBUG("Triggered event %d skipped due to missing audio buffer.\n",
          snd_id);
    pthread_mutex_unlock(&dispatch_Mut); 
    return;
  }

  /* Schedule a play to happen ASAP */
  L22SetAudioBuffer(l22dev[c], buf->data, buf->len, (buf->bits/8-1)|((buf->nchans-1)*4), chan, buf->is_looped_flag);
  
  ++buf->trig_play_ct; /* increment trig play count */
  
  rs[c].last_played_buf[chan] = buf;

  cancelAnyRunningRampedStops(c, chan);

  pthread_mutex_unlock(&dispatch_Mut); 
}

/** Called from the function pointer in the shm, by another rt-task,
    in rt-context, to do the 'software' virtual triggering.  
    (RatExpFSM.c knows how to call this, for example). */
static int virtualTriggerFunction(CardID_t card, int trig)
{
  int ret = 0;

  if (card >= n_l22dev) {
    WARNING("virtual trigger function called with an invalid card id %u!\n", card);
    return ret;
  }

  MINC;

  /* TODO: Worry about race conditions with cleanup() here? */
  
  if ( !TRIGGERS_LOCKED() ) 
  {
      DEBUG("virtual trigger function called with %d\n", trig);
      dispatchEvent(card, trig);
      ++cycle;
      ret = 1;
  }
  else
    DEBUG("virtual trigger function called with %d, skipping due to locked triggers\n", trig);

  MDEC;
  
  return ret;
}

static void freeAudioBuffer(struct AudioBuf *buf)
{
    if (buf->data && buf->len && buf->name[0]) {        
        AllocArg arg;
        DEBUG("Freeing buffer named %s size %lu\n", buf->name, buf->len);
        arg.name = buf->name;
        arg.size = buf->len;
        ShmListDel(buf->data); /**< implicitly deletes the audio buffer using rt-shm mechanisms */
    }
    memset((void *)buf, 0, sizeof(*buf));
}

static void freeAudioBuffers(CardID_t c)
{
  unsigned i;
  for (i = 0; i < MAX_SND_ID; ++i) 
    freeAudioBuffer(&rs[c].audio_buffers[i]);  
}

static int fifoHandler0(unsigned f) { (void)f; if (shm) return handleFifos_MapCardID(shm->fifo_in[0]); return -1; }
static int fifoHandler1(unsigned f) { (void)f; if (shm) return handleFifos_MapCardID(shm->fifo_in[1]); return -1; }
static int fifoHandler2(unsigned f) { (void)f; if (shm) return handleFifos_MapCardID(shm->fifo_in[2]); return -1; }
static int fifoHandler3(unsigned f) { (void)f; if (shm) return handleFifos_MapCardID(shm->fifo_in[3]); return -1; }
static int fifoHandler4(unsigned f) { (void)f; if (shm) return handleFifos_MapCardID(shm->fifo_in[4]); return -1; }
static int fifoHandler5(unsigned f) { (void)f; if (shm) return handleFifos_MapCardID(shm->fifo_in[5]); return -1; }
static int handleFifos_MapCardID(unsigned int fifo_no) /* gets called as rtf handler, maps rtf id to card id and calls handleFifos(cardId) */
{
  DEBUG("handleFifos_MapCardID(%p)\n", (void *)(unsigned long)fifo_no);
  if (shm) {
    unsigned card;
    for (card = 0; card < n_l22dev; ++card) 
      if (fifo_no == shm->fifo_in[card])
        return handleFifos(card);
  }
  ERROR("handleFifos_MapCardID could not map %u to any card!! Wtf?!\n", fifo_no);
  return -EINVAL;
}

/* Note: in comedi_triggers mode this is run in realtime context, otherwise
   it is called as a non-realtime rt-fifo handler in Linux context.          */
static int handleFifos(CardID_t c)
{
  FifoNotify_t dummy = 1;
  int errcode;

  /** locking only does something for software trigger mode (in which case
      this function is in non-realtime context), since that is the
      only case where there is asynchronous access to trigger data.. */
  LOCK_TRIGGERS();

  if (SOFTWARE_TRIGGERS()) 
    DEBUG("handleFifos called asynchronously in Linux context.\n");
  

  errcode = rtf_get(shm->fifo_in[c], &dummy, sizeof(dummy));

  if (errcode == sizeof(dummy)) {
      /* Ok, a message is ready, so take it from the SHM */
      struct FifoMsg *msg = (struct FifoMsg *)&shm->msg[c];
      int do_reply = 0;
      
      DEBUG("handleFifos(%u)\n", (unsigned)c);
#ifdef RTAI
      DEBUG("prio: %x\n", rt_get_prio(rt_whoami()));
#endif

      switch (msg->id) {
        
      case INITIALIZE:
          /* spawn a new thread and wait for it if we are not in rt-context..
             otherwise call function immediately.. */
        doCmdInRTThread(INITIALIZE, (void *)(unsigned long)c); 
        do_reply = 1;
        break;
                
      case PAUSEUNPAUSE:
        rs[c].paused = !rs[c].paused;
        /* Notice fall through to next case.. */
      case GETPAUSE:
        msg->u.is_paused = rs[c].paused;
        do_reply = 1;
        break;
        
      case INVALIDATE:
        rs[c].invalid = 1;
        /* Notice fall through.. */
      case GETVALID:
        msg->u.is_valid = !rs[c].invalid;
        do_reply = 1;
        break;
      case ALLOCSOUND: {
           /** NB: this potentially blocks this thread for a while!  If we are in RT it will break RT, but that's ok since this is not a realtime operation anyway */
          void *mem = ShmListNew(msg->u.sound.name, msg->u.sound.size);
          if (mem) {
              DEBUG("ALLOCSOND: allocated shm for %s of size %lu\n", msg->u.sound.name, msg->u.sound.size);
          } else {
              ERROR("ALLOCSOND: could not allocate shm %s size %lu\n", msg->u.sound.name, msg->u.sound.size);
          }
          msg->u.sound.transfer_ok = mem != 0;
      }
          break;
      case SOUND:
        rs[c].invalid = 1; /* lock? */
        mb();
        DEBUG("handleFifos: SOUND cmd received\n", (unsigned)c);
        
        {
          unsigned id = msg->u.sound.id;
          msg->u.sound.transfer_ok = 0; /* Assume not until otherwise proven to be ok.. */
          if (id < MAX_SND_ID) {
            struct AudioBuf *buf = &rs[c].audio_buffers[id];
            unsigned devRate = 0;
            /* TODO HACK BUG HELP! XXX NOTE -- We really REALLY need to handle the case where we are 
               freeing a sound that is currently playing here.  What do we do?! NEEDS TESTING. */
            { /* ok, we'll try and stop the sound here.  BUT it may not be obvious if it's still playing
                 it *definitely isn't* playing if: 
                   - CM_LookupSound() fails
                    *or*
                   - CM_LookupSound() succeeds, but rs[c].last_played_buf[chan] != buf
                
                Otherwise... the sound might be playing or may have recently played and it stopped and no other sound is playing on that channel.
                
                Note the above checks ensure that we don't stop a different sound that might be playing.. */
                int chan = CM_LookupSound(channels[c], id);
                 
                if (chan > -1 && chan < L22_NUM_CHANS && rs[c].last_played_buf[chan] == buf) {
                    DevChan dc;
                    dc.dev = c;
                    dc.chan = chan;
                    DEBUG("Stopping sound on dev %hu chan %hu before loading a new sound...\n", dc.dev, dc.chan);
                    doCmdInRTThread(STOP_CMD, dc.devchan_voidp);
                } else
                    DEBUG("Sound was not playing, does not need stoppage.\n");
                
                 
            }
            /*L22Stop(); / * Just in case it was playing a sound.. */
            
            /* free and clear previous buffer.. a noop if it wasn't allocated */
            freeAudioBuffer(buf);
            
            buf->len = msg->u.sound.size;
            buf->stop_ramp_tau_ms = msg->u.sound.stop_ramp_tau_ms;
            strncpy(buf->name, msg->u.sound.name, sizeof(buf->name));
            buf->name[sizeof(buf->name)-1] = 0;/* force NUL termination */
            DEBUG("Looking up buffer %s of length %lu in ShmList\n", buf->name, (unsigned long)buf->len);
            msg->u.sound.transfer_ok = 0;
            if ( (buf->data = ShmListFind(buf->name, buf->len)) ) {
              DEBUG("ShmList buffer found!\n");
              buf->nchans = msg->u.sound.chans;
              buf->rate = msg->u.sound.rate;
              buf->bits = msg->u.sound.bits_per_sample;
              buf->is_looped_flag = msg->u.sound.is_looped;
              buf->trig_play_ct = buf->trig_stop_ct = 0;
              msg->u.sound.transfer_ok = 1;

              /** Verify the sampling rate matches the L22 device.. */
              if ( (devRate = L22GetSampleRate(c)) != buf->rate) {
                WARNING("Lynx device hard-coded sampling rate of %u differs "
                        "from specified rate of %u!\n", 
                        devRate, (unsigned)buf->rate);
              }

              /* not supported anymore.. L22SetSampleRate(l22dev, buf->rate); */

            } else {
              ERROR("Sound buffer %u named %s of size %lu could not be found! Did it fail to allocate via ALLOCSOUND?\n", id, buf->name, buf->len);
            }
            msg->u.sound.size = 0; /* Clear it, just in case? */
          } else {
            /* ID is too high.. */
            ERROR("Sound buffer specified with invalid/too high an id %u.  Limit is %u.\n", id, MAX_SND_ID);
          }
        }
        rs[c].invalid = 0; /* Unlock.. */
        mb();
        
        do_reply = 1;
        break;
        
      case FORCEEVENT:
        rs[c].forced_event = msg->u.forced_event;

        /* If the rt task isn't running, dispatch the forced event
         *now* in non-RT context.  Otherwise, it will get picked up
         by the RT task.. */
        if ( SOFTWARE_TRIGGERS() ) 
          {
            DevEvt de;
            de.dev = c;
            de.evt = rs[c].forced_event;
            doCmdInRTThread(FORCEEVENT, de.devevt_voidp);
            rs[c].forced_event = 0;
          }

        do_reply = 1;
        break;
        
      case GETRUNTIME:
        if ( SOFTWARE_TRIGGERS() ) updateTS(c);
        msg->u.runtime_us = Nano2USec(rs[c].current_ts);
        do_reply = 1;
        break;

        
      case GETLASTEVENT:
        msg->u.last_event = rs[c].last_event;
        do_reply = 1;
        break;
        
      default:
        WARNING("Got unknown msg id '%d' in handleFifos()!\n", msg->id);
        do_reply = 0;
        break;
      }
      
      errcode = rtf_put(shm->fifo_out[c], &dummy, sizeof(dummy));
      if (errcode != sizeof(dummy)) {
        static int once_only = 0;
        if (!once_only) 
          ERROR("rtos_fifo_put to fifo_out %d returned %d!\n", shm->fifo_out[c], errcode), once_only++;
      }
  } else if (errcode != 0) {
      static int once_only = 0;
      if (!once_only)
        ERROR("got return value of (%d) when reading from fifo_in %d\n", errcode, shm->fifo_in[c]), once_only++;
  }

  /** locking only does something for non-realtime! */
  UNLOCK_TRIGGERS();

  return 0;
}

static void grabAllDIO(void)
{
  /* Remember previous bits */
  dio_bits_prev = dio_bits;

  if ( COMEDI_TRIGGERS() ) 
  {
      /* Grab all the input channels at once */
      comedi_dio_bitfield(dev, subdev, 0, (unsigned int *)&dio_bits);
      dio_bits &= ((1 << (num_trig_chans+first_trig_chan)) - 1) & ~((1 << first_trig_chan) - 1) ; /* Mask out unwanted channels */

      if (debug && dio_bits && ullmod_(cycle, sampling_rate) == 0)
        DEBUG("READS 0x%x\n", dio_bits);
  }

}

/**< returns 0 on no input detected, positive for play id 
   or negative for stop id */
static int detectEvent(CardID_t c)
{
  int ret = 0;

  if (rs[c].forced_event) {
    ret = rs[c].forced_event;
    rs[c].forced_event = 0;
    return ret;
  }

  if (COMEDI_TRIGGERS() && dio_bits != dio_bits_prev) {

      unsigned trig_tmp;
      /* Grab the exact channels we want.. */
      trig_tmp = (dio_bits>>first_trig_chan) & ((0x1<<num_trig_chans) - 1);
      ret = trig_tmp;
      /* Special case, 1 trig chan means 0/1 is stop/start.. */
      if (num_trig_chans == 1 && trig_tmp == 0) ret = -1; 
      /* Check for sign bit? Last bit is 'sign' bit, and it it's set, we are supposed to return the negative value of trig_tmp.. */
      else if (num_trig_chans > 1 && trig_tmp & (0x1<<(num_trig_chans-1))) 
        ret = -((int)(trig_tmp & ((0x1<<(num_trig_chans-1))-1)));
      
  }

  return ret;
}

static void doPossiblyRampedStop(unsigned c, unsigned chan)
{
  DEBUG("doPossiblyRampedStop called for card %u channel %u\n", c, chan);
  if (chan >= L22_NUM_CHANS || c >= n_l22dev) {
    DEBUG("doPossiblyRampedStop got invalid card %u chan %u!\n", c, chan);
    return;
  }
  if (!rs[c].last_played_buf[chan] || !rs[c].last_played_buf[chan]->stop_ramp_tau_ms || !L22IsPlaying(l22dev[c], chan)) /* paranoia */ {
    /* call L22stop just in case.. */
    L22Stop(l22dev[c], chan);
    CM_ClearChannel(channels[c], chan);

    if (rs[c].last_played_buf[chan] && !rs[c].last_played_buf[chan]->stop_ramp_tau_ms)
      DEBUG("doPossiblyRampedStop skipped a ramping stop because tau_ms is 0!\n");
    else
      DEBUG("doPossiblyRampedStop skipped a ramping stop because there is no sound playing!");

  } else if (rs[c].last_played_buf[chan]) {
    struct RamperThread *thr = &ramperThreads[c][chan];
    if ( !atomic_read(&thr->tauParam) ) {
      /* give ramper thread its tau_ms param.. */
      atomic_set(&thr->tauParam, rs[c].last_played_buf[chan]->stop_ramp_tau_ms);
      /* wake up ramper thread.. */
      sem_post(&thr->sem); 
    } else {
      DEBUG("doPossiblyRampedStop skipped a ramping stop because there already is a ramping stop running!\n");
    }
    rs[c].last_played_buf[chan] = 0; /* need to clear this because it should only get set once when a sound is played */
  }
}

static int createRamperThreads(void)
{
    int chan, error = 0;
    unsigned dev;
    for (dev = 0; dev < n_l22dev; ++dev) 
      for (chan = 0; chan < L22_NUM_CHANS; ++chan) 
        {
          /* Create a separate threads that run in realtime every .1 ms
             and do the cosine2 ramping function -- they sleep on their
             semaphore when not needed. */
          pthread_attr_t attr;
          struct sched_param sched_param;      
          struct RamperThread *thr = &ramperThreads[dev][chan];
          
          memset((void *)thr, 0, sizeof(*thr)); /* clear/init the data structure.. */
          
          thr->chan = chan;
          thr->dev = dev;
          error = sem_init(&thr->sem, 0, 0); /* init the semaphore */
          
          if (!error) {
            
            /* Setup pthread data, etc.. */
            pthread_attr_init(&attr);
            error = pthread_attr_setfp_np(&attr, 1);
            if (!error) {
              sched_param.sched_priority = 1;
              error = pthread_attr_setschedparam(&attr, &sched_param);  
              if (!error) {
                error = pthread_create(&thr->thread, &attr, 
                                       cosine2RamperThread, (void *)thr);
              }
            }
            pthread_attr_destroy(&attr);
            
          }
          
          if (error) {
            WARNING("createRamperThreads() could not create the cosine2RamperThread #%d (%d)!\n", chan, error);
            break;
          }
        }
    return error;
}

static void destroyRamperThreads(void)
{
  int chan;
  unsigned dev;
  DEBUG("destroyRamperThreads()\n");
  for (dev = 0; dev < n_l22dev; ++dev) 
    for (chan = 0; chan < L22_NUM_CHANS; ++chan) {
      struct RamperThread *thr = &ramperThreads[dev][chan];
      if (thr->thread) {
        void *count = 0;
        atomic_set(&thr->stop, 1);
        sem_post(&thr->sem);
        pthread_join(thr->thread, &count);
        DEBUG("ramper thread %u:%d deleted after %lu successful rampings!\n", dev, chan, (unsigned long)count);
        thr->thread = 0;
      }
      sem_destroy(&thr->sem);
    }
}

#ifdef RTLINUX
/* just like clock_gethrtime but instead it used timespecs */
static inline void clock_gettime(clockid_t clk, struct timespec *ts)
{
  hrtime_t now = clock_gethrtime(clk);
  nano2timespec(now, ts);  
}

static inline void nano2timespec(hrtime_t time, struct timespec *t)
{
  unsigned long rem = 0;
  t->tv_sec = ulldiv_(time, 1000000000, &rem);
  t->tv_nsec = rem;
}
#endif

static inline unsigned long long ulldiv_(unsigned long long ull, unsigned long uld, unsigned long *r)
{
    if (r) *r = do_div(ull, uld);
    else do_div(ull, uld);
    return ull;
}
static inline unsigned long ullmod_(unsigned long long ull, unsigned long uld)
{         
        unsigned long ret;
        ulldiv_(ull, uld, &ret);
        return ret;
}

static inline unsigned long Nano2Sec(unsigned long long nano)
{
  unsigned long rem = 0;
  return ulldiv_(nano, BILLION, &rem);
}

static inline unsigned long Nano2USec(unsigned long long nano)
{
  unsigned long rem = 0;
  return ulldiv_(nano, 1000, &rem);
}


static const char *uint64_to_cstr(uint64 num)
{
  static const uint64 ZEROULL = 0ULL;
  static const uint32 dividend = 10;
  static char buf[21];
  int ct = 0, i;
  uint64 quotient = num;
  unsigned long remainder;
  char reversebuf[21];

  buf[0] = 0;

  /* convert to base 10... results will be reversed */
  do {
    remainder = do_div(quotient, dividend);
    reversebuf[ct++] = remainder + '0';
  } while (quotient != ZEROULL);

  /* now reverse the reversed string... */
  for (i = 0; i < ct; i++) 
    buf[i] = reversebuf[(ct-i)-1];
  
  /* add null... */
  buf[ct] = 0;
  
  return buf;
}

static inline long Micro2Sec(long long micro, unsigned long *remainder)
{
  int neg = micro < 0;
  unsigned long rem;
  long ans;
  if (neg) micro = -micro;
  ans = ulldiv_(micro, 1000000, &rem);
  if (neg) ans = -ans;
  if (remainder) *remainder = rem;
  return ans;
}

extern double cos(double);

static inline double cos2(double r) { double c = cos(r); return c*c; }

static void *cosine2RamperThread(void *arg)
{
  static const unsigned long task_period_nanos = 400000; /* .4 ms task period */
#define HALF_PI 1.570796326794896619 /* PI/2*/
  struct RamperThread *me = (struct RamperThread *)arg;
  unsigned long num_handled = 0;
  unsigned short tau_ms;
  sem_t *mySemaphore = &me->sem;
  atomic_t *myStopFlg = &me->stop, *myParam = &me->tauParam;
  const unsigned chan = me->chan;
  const unsigned dev = me->dev;
  const L22Dev_t ldev = l22dev[dev];
  const unsigned v_init = L22GetVolume(ldev, chan);

  while ( !atomic_read(myStopFlg) ) {
    if ( !(tau_ms = atomic_read(myParam)) )  sem_wait(mySemaphore); 
    else {
      const unsigned total_loops = (tau_ms*MILLION)/task_period_nanos;
      const double t_incr = 1.0/(total_loops ? ((double)total_loops) : 1.0);
      double t = 0.0;
      struct timespec next_task_wakeup;
      
      clock_gettime(CLOCK_REALTIME, &next_task_wakeup);
      
      /* Keep looping until either:
         1. the device is stopped (could be from a call outside this code)
         2. someone reset the tau_ms (from dispatchEvent() when another
            sound was triggered while we were looping)
         3. We finish the ramp-down.. */
      while (L22IsPlaying(ldev, chan) && t <= 1.0 && atomic_read(myParam)) {
        unsigned vol = v_init * cos2(t*HALF_PI);
        t += t_incr;
        L22SetVolume(ldev, chan, vol);
        if (debug > 1) DEBUG("ramper%u:%u: setvolume: %u\n", dev, chan, vol);

        timespec_add_ns(&next_task_wakeup, task_period_nanos);
        /* Sleep until next period */
        clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next_task_wakeup, 0); 
      }
      if (L22IsPlaying(ldev, chan)) { 
        L22Stop(ldev, chan);
        CM_ClearChannel(channels[dev], chan); 
      }
      L22SetVolume(ldev, chan, v_init); 
      atomic_set(myParam, 0);
      ++num_handled;
    }
  }
  DEBUG("cosine2Ramper about to return %lu\n", num_handled);
  return (void *)num_handled;
#undef HALF_PI
}

static void cancelAnyRunningRampedStops(unsigned c, unsigned chan)
{
  if (c < n_l22dev && chan < L22_NUM_CHANS) 
    atomic_set(&ramperThreads[c][chan].tauParam, 0);  
}

static void stopAllChans(CardID_t c)
{
  unsigned chan;
  
  for (chan = 0; chan < L22_NUM_CHANS; ++chan) {
      DevChan dc;
      dc.dev = c;
      dc.chan = chan;
      doCmdInRTThread(STOP_CMD, dc.devchan_voidp);
      /*L22Stop(chan);  above is better -- since l22 funcs now have to be 
        called from RT only! */
  }
  CM_Reset(channels[c]);
}

static int initChanMapper(void) 
{ 
  unsigned i;
  memset((void *)channels, 0, sizeof(channels));
  for (i = 0; i < n_l22dev; ++i) {
    channels[i] = CM_Init(l22dev[i], debug ? (void *)rt_printk : 0);
    if (!channels[i]) return -ENOMEM; 
  }
  return 0;
}

static void destroyChanMapper(void)
{
  unsigned i;
  for (i = 0; i < n_l22dev; ++i) 
    if (channels[i]) CM_Destroy(channels[i]);
  memset((void *)channels, 0, sizeof(channels));
}

/* Some thread functions used to implement some fifo commands.  These are
   called from either a real-time thread spawned by handleFifos in 
   SOFTWARE_TRIGGERS mode or from the rt-task in hardware triggers mode. */
static void *doPlayCmd(void *arg) 
{ 
  DevChan dc;
#ifdef RTAI
  DEBUG("doPlayCmd(%p) prio: %x\n", arg, rt_get_prio(rt_whoami()));
#endif  
  dc.devchan_voidp = arg;
  L22Play(l22dev[dc.dev], dc.chan);
  return (void *)1; 
}
static void *doStopCmd(void *arg) 
{
  DevChan dc;
  dc.devchan_voidp = arg;
  L22Stop(l22dev[dc.dev], dc.chan);
  return (void *)1; 
}

static void *doResetCmd(void *arg) 
{
  unsigned card = (unsigned long)arg;
  
  /* Just to make sure we start off fresh ... */
  stopAllChans(card);
  freeAudioBuffers(card);
  initRunState(card);
  return (void *)1;
}

static void *doEventCmd(void *arg) 
{
  DevEvt de;  
#ifdef RTAI
  DEBUG("doEventCmd(%p) prio: %x\n", arg, rt_get_prio(rt_whoami()));
#endif  
  de.devevt_voidp = arg;
  dispatchEvent(de.dev, de.evt);
  return (void *)1;
}

static void *doCmdInRTThread(int cmd, void *arg)
{
  void *(*func)(void *) = 0;
  DEBUG("doCmdInRTThread(%d, %p)\n", cmd, arg);
  switch (cmd) {
  case FORCEEVENT: func = doEventCmd; break;
  case INITIALIZE: func = doResetCmd; break;
  case STOP_CMD: func = doStopCmd; break;
  case PLAY_CMD: func = doPlayCmd; break;
  default:
    ERROR("*** Internal Error *** Invalid command passed to doCmdInRTThread: %d!\n", cmd);
    return 0;
  }
  if (func) {
    if ( CURRENT_IS_LINUX() ) {
      /* we are not in an rt thread, so spawn a new thread and wait for it */
      pthread_t thr = 0;
      void * ret;
      int err = pthread_create(&thr, 0, func, arg);
      DEBUG("doCmdInRTThread - CURRENT IS LINUX, creating RT thread\n");
      if (err) {
        ERROR("*** Internal Error *** Could not create RT thread in doCmdInRTThread: %d!\n", cmd);
        return 0;
      }
      pthread_join(thr, &ret);
      return ret;
    } else {  /* already in an rt-thread, just call the func */
        DEBUG("doCmdInRTThread - CURRENT IS NOT LINUX, no RT thread needed\n");
        return func(arg);  
    }
  }
  /* not _normally_ reached.. */
  return 0;
}

struct MrSoftTaskArg
{
    void *(*volatile func)(void *);
    void * volatile arg;
    void * volatile ret;
    sem_t *sem;
};
static void *doCmdInSoftThread(int cmd, void *arg)
{
    void *(*func)(void *) = 0;
    DEBUG("doCmdInSoftThread(%d, %p)\n", cmd, arg);
    switch(cmd) {
    case ALLOC_KMEM_CMD: func = doAllocKmemCmd; break;
    case ALLOC_MBUFF_CMD: func = doAllocMbuffCmd; break;
    case ATTACH_MBUFF_CMD: func = doAttachMbuffCmd; break;
    case FREE_KMEM_CMD: func = doFreeKmemCmd; break;
    case FREE_MBUFF_CMD: func = doFreeMbuffCmd; break;
    case DETACH_MBUFF_CMD: func = doDetachMbuffCmd; break;
    default:
        ERROR("*** Internal Error *** Invalid command passed to doCmdInSoftThread: %d!\n", cmd);
        return 0;
    }
    if (func) {
        if ( CURRENT_IS_LINUX() ) {
            DEBUG("doCmdInSoftThread -- CURRENT IS LINUX, so calling func directly\n");
            return func(arg);
        } else {
            struct MrSoftTaskArg taskarg = { func, arg, 0, get_unused_sem() };
            int err;
            DEBUG("doCmdInSoftThread -- CURRENT IS NOT LINUX, pending softtask\n");
            if (! (err = softTaskPend(get_unused_st(), &taskarg))) {
                DEBUG("doCmdInSoftThread ... and waiting on sem...\n");
                sem_wait(taskarg.sem);
                DEBUG("doCmdInSoftThread got return value %p\n", taskarg.ret);
            } else {
                ERROR("got error return %d from softTaskPend!\n", err);
            }
            return taskarg.ret;
        }
    }
    return 0; /* not reached */
}

static void mrSoftTask(void *arg)
{
    struct MrSoftTaskArg *taskArg = (struct MrSoftTaskArg *)arg;
    DEBUG("mrSoftTask(%p)\n", arg);
    taskArg->ret = taskArg->func(taskArg->arg);
    DEBUG("mrSoftTask - returning %p\n", taskArg->ret);
    sem_post(taskArg->sem);
    DEBUG("mrSoftTask gunna return\n");
}

static void *doAllocMbuffCmd(void *arg)
{
    AllocArg *allocArg = (AllocArg *)arg;
    DEBUG("doAllocMbuffCmd(%p) -> %s %lu\n", arg, allocArg->name, allocArg->size);
    return mbuff_alloc(allocArg->name, allocArg->size);
}

static void *doAttachMbuffCmd(void *arg)
{
    AllocArg *allocArg = (AllocArg *)arg;
    DEBUG("doAttachMbuffCmd(%p) -> %s %lu\n", arg, allocArg->name, allocArg->size);
    return mbuff_attach(allocArg->name, allocArg->size);
}

static void *doAllocKmemCmd(void *arg)
{
    unsigned long size = (unsigned long)arg;
    DEBUG("doAllocKmemCmd(%p) -> %lu\n", arg, size);
    return kmalloc(size, GFP_KERNEL);
}


static void *doFreeKmemCmd(void *arg)
{
    DEBUG("doFreeKmemCmd(%p)\n", arg);
    kfree(arg);
    return 0;
}

static void *doFreeMbuffCmd(void *arg)
{
    AllocArg *allocArg = (AllocArg *)arg;
    DEBUG("doFreeMbuffCmd(%p) -> %s %p\n", arg, allocArg->name, allocArg->ptr);
    mbuff_free(allocArg->name, allocArg->ptr);
    return 0;
}

static void *doDetachMbuffCmd(void *arg)
{
    AllocArg *allocArg = (AllocArg *)arg;
    DEBUG("doDetachMbuffCmd(%p) -> %s %p\n", arg, allocArg->name, allocArg->ptr);
    mbuff_detach(allocArg->name, allocArg->ptr);
    return 0;
}

static void *ShmListNew(const char *name, unsigned long size)
{
    struct ShmList *entry = doCmdInSoftThread(ALLOC_KMEM_CMD, (void *)sizeof(*entry));
    AllocArg arg;
    if (!entry) {
        ERROR("ShmListNew: could not allocate a list entry data structure!\n");
        return 0;
    }
    arg.name = name;
    arg.size = size;
    entry->mem = doCmdInSoftThread(ALLOC_MBUFF_CMD, (void *)&arg);
    if (!entry->mem) {
        ERROR("ShmListNew: failed to allocated mbuff %s size %lu\n", arg.name, arg.size);
        doCmdInSoftThread(FREE_KMEM_CMD, (void *)entry);
        return 0;
    }
    strncpy(entry->name, name, SNDNAME_SZ);
    entry->name[SNDNAME_SZ-1] = 0;
    entry->size = size;
    pthread_mutex_lock(&shmListMut);
    list_add(&entry->list, &shmList);
    pthread_mutex_unlock(&shmListMut);
    return entry->mem;
}
static void ShmListDel_nolock(void *mem)
{
    struct list_head *pos;
    list_for_each(pos, &shmList) {
        struct ShmList *entry = (struct ShmList *)pos;
        if (entry->mem == mem) {
            AllocArg arg;
            arg.name = entry->name;
            arg.ptr = entry->mem;
            list_del(pos);
            doCmdInSoftThread(FREE_MBUFF_CMD, &arg);
            doCmdInSoftThread(FREE_KMEM_CMD, (void *)entry);
            break;
        }
    }
}
static void ShmListDel(void *mem)
{
    pthread_mutex_lock(&shmListMut);
    ShmListDel_nolock(mem);
    pthread_mutex_unlock(&shmListMut);
}
static void ShmListCleanup(void)
{
    struct list_head *pos, *next;
    pthread_mutex_lock(&shmListMut);
    list_for_each_safe(pos, next, &shmList) {
        ShmList *entry = (ShmList *)pos;
        ShmListDel_nolock(entry->mem);
    }
    pthread_mutex_unlock(&shmListMut);
}
static void *ShmListFind(const char *name, unsigned long size)
{
    struct list_head *pos;
    void *found = 0;
    pthread_mutex_lock(&shmListMut);
    list_for_each(pos, &shmList) {
        ShmList *entry = (ShmList *)pos;
        if (!strcmp(entry->name, name)) {
            if (entry->size != size) 
                WARNING("ShmListFind found entry %s buf entry->size != passed-in size, (%lu != %lu)\n", entry->size, size);
            found = entry->mem;
            break;
        }
    }
    pthread_mutex_unlock(&shmListMut);
    return found;
}

static int initSems(void)
{
    int i, ret;
    setup_sems = 0;
    for (i = 0; i < NUM_SOFTS_SEMS; ++i) {
        if ( (ret = sem_init(&misc_sems[i], 0, 0)) ) return ret;
        ++setup_sems;
    }
    if ( !pthread_mutex_init(&misc_sem_mut, 0) )
        ++setup_sems;
    else
        return -EINVAL;
    return 0;
}
static void cleanupSems(void)
{
    int i;
    if (setup_sems == NUM_SOFTS_SEMS+1) { pthread_mutex_destroy(&misc_sem_mut); setup_sems--; }
    for (i = 0; i < setup_sems; ++i) 
            sem_destroy(&misc_sems[i]);
    setup_sems = 0;
}
static sem_t *get_unused_sem(void)
{
    int i;
    pthread_mutex_lock(&misc_sem_mut);
    atomic_inc(&misc_sem_idx);
    i = atomic_read(&misc_sem_idx) % NUM_SOFTS_SEMS;
    pthread_mutex_unlock(&misc_sem_mut);
    return &misc_sems[i];
}

static struct SoftTask *get_unused_st(void)
{
    int i;
    pthread_mutex_lock(&soft_task_mut);
    atomic_inc(&soft_task_idx);
    i = atomic_read(&soft_task_idx) % NUM_SOFTS_SEMS;
    pthread_mutex_unlock(&soft_task_mut);
    return soft_tasks[i];
}

static int initSTs(void)
{
    int i;
    setup_soft_tasks = 0;
    for (i = 0; i < NUM_SOFTS_SEMS; ++i) {
        char nam[] = { 's', 't', i+'0', 0 };
        if ( !(soft_tasks[i] = softTaskCreate(mrSoftTask, nam)) ) return -ENOMEM;
        ++setup_soft_tasks;
    }
    if ( !pthread_mutex_init(&soft_task_mut, 0) )
        ++setup_soft_tasks;
    else
        return -EINVAL;
    return 0;
}

static void cleanupSTs(void)
{
    int i;
    if (setup_soft_tasks == NUM_SOFTS_SEMS+1) { pthread_mutex_destroy(&soft_task_mut); setup_soft_tasks--; }
    for (i = 0; i < setup_soft_tasks; ++i) {
            softTaskDestroy(soft_tasks[i]);
            soft_tasks[i] = 0;
    }
    setup_soft_tasks = 0;
}

#ifdef FAKE_L22

/* Dummy funcs for debugging */
int L22SetVolume(L22Dev_t dev, unsigned chan, unsigned long volume)
{
    DEBUG("L22SetVolume(%d, %u, %lu)\n", dev, chan, volume);
    return 1;
}
void L22Close(L22Dev_t dev) 
{
    DEBUG("L22Close(%d)\n", dev);
}
unsigned long L22GetVolume(L22Dev_t dev, unsigned chan)
{
    DEBUG("L22GetVolume(%d,%u)\n", dev, chan);
    return 0xffff;
}
unsigned L22GetSampleRate(L22Dev_t dev)
{
    DEBUG("L22GetSampleRate(%d)\n", dev);
    return 200000;
}
int L22Play(L22Dev_t dev, unsigned chan)
{
    DEBUG("L22Play(%d, %x)\n", dev, chan);
    return 1;
}
L22Dev_t L22Open(int devno)
{
    DEBUG("L22Open(%d)\n", devno);
    return devno;
}
void L22Stop(L22Dev_t dev, unsigned chan)
{
    DEBUG("L22Stop(%d, %x)\n", dev, chan);
}
int L22IsPlaying(L22Dev_t dev, unsigned chan)
{
    DEBUG("L22IsPlaying(%d, %x)\n", dev, chan);
    return 1;
}
int L22GetNumDevices(void)
{
    DEBUG("L22GetNumDevices(void)\n");
    return 4;
}
int L22SetAudioBuffer(L22Dev_t dev, void *buffer, unsigned long num_bytes, int format_code, unsigned chan_id, int looped)
{
    DEBUG("L22SetAudioBuffer(%d, %p, %lu, %d, %u, %d)\n", dev, buffer, num_bytes, format_code, chan_id, looped);
    return 1;
}
#endif
