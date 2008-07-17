/**
 * Rat Experiments FSM, for use at CSHL
 *
 * Calin A. Culianu <calin@ajvar.org>
 * License: GPL v2 or later.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <comedilib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <sys/mman.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

/* 2.4 kernel lacks this function, so we will emulate it using the function
   it does have ffs(), which thinks of the first bit as bit 1, but we want the
   first bit to be bit 0. */
static __inline__ int __ffs(int x) { return ffs(x)-1; }

#include "RatExpFSM.h"
#include "softtask.h" /* for asynchronous  tasks! */
#include "rtfemul.h"
#include "mbuffemul.h"

#define MODULE_NAME "RatExpFSM"

#define LOG_MSG(x...) fprintf(stderr, MODULE_NAME ": "x)
#define WARNING(x...) fprintf(stderr, MODULE_NAME ": WARNING - " x)
#define ERROR(x...) fprintf(stderr, MODULE_NAME": ERROR - " x)
#define ERROR_INT(x... ) fprintf(stderr, MODULE_NAME": INTERNAL ERROR - " x)
#define DEBUG(x... ) do { if (debug) fprintf(stderr, MODULE_NAME": DEBUG - " x); } while (0)


typedef long long hrtime_t;

int init(void);  /**< Initialize data structures and register callback */
void cleanup(void); /**< Cleanup.. */

static int initRunState(void);
static int initShm(void);
static int initBuddyTask(void);
static int initFifos(void);
static int initTaskPeriod(void);
static int initRT(void);
static int initComedi(void);
static void reconfigureDIO(void); /* helper that reconfigures DIO channels 
                                     for INPUT/OUTPUT whenever the state 
                                     matrix, etc changes. */
static int initAISubdev(void); /* Helper for initComedi() */
static int initAOSubdev(void); /* Helper for initComedi() */
static int setupComediCmd(void);
static void cleanupAOWaves(void);
struct AOWaveINTERNAL;
static void cleanupAOWave(volatile struct AOWaveINTERNAL *);

/* The callback called by rtlinux scheduler every task period... */
static void *doFSM (void *);

/*---------------------------------------------------------------------------- 
  Some private 'global' variables...
-----------------------------------------------------------------------------*/
static volatile Shm *shm = 0;

#define DEFAULT_JITTER_TOLERANCE_NS (1000000000/DEFAULT_SAMPLING_RATE)
#define MAX_HISTORY 1000000 /* The maximum number of state transitions we remember */
#define DEFAULT_SAMPLING_RATE 1000
#define DEFAULT_AI_SAMPLING_RATE 10000
#define DEFAULT_AI_SETTLING_TIME 5
#define DEFAULT_TRIGGER_MS 1
#define DEFAULT_AI "synch"
#define MAX_AI_CHANS (sizeof(unsigned)*8)
#define MAX(a,b) ( a > b ? a : b )
#define MIN(a,b) ( a < b ? a : b )
static char COMEDI_DEVICE_FILE[] = "/dev/comediXXXXXXXXXXXXXXX";
int minordev = 0, minordev_ai = -1, minordev_ao = -1,
    sampling_rate = DEFAULT_SAMPLING_RATE, 
    ai_sampling_rate = DEFAULT_AI_SAMPLING_RATE, 
    ai_settling_time = DEFAULT_AI_SETTLING_TIME,  /* in microsecs. */
    trigger_ms = DEFAULT_TRIGGER_MS,    
    debug = 0,
    avoid_redundant_writes = 0,
    jitter_tolerance_ns = DEFAULT_JITTER_TOLERANCE_NS;
char *ai = DEFAULT_AI;

#ifndef STR
#define STR1(x) #x
#define STR(x) STR1(x)
#endif

struct ModParm
{
  void *addr;
  const char *name;
  const char *type;
  const char *descr;
};

#define MODPARMS_START static struct ModParm modParms[] = { 
#define MODPARMS_END \
 {.addr = 0} };
#define MODULE_PARM(x, t) { .addr = &x, .name = STR(x), .type = t, 
#define MODULE_PARM_DESC(x, d) .descr = d },

MODPARMS_START

MODULE_PARM(minordev, "i")
MODULE_PARM_DESC(minordev, "The minor number of the comedi device to use.")
MODULE_PARM(minordev_ai, "i")
MODULE_PARM_DESC(minordev_ai, "The minor number of the comedi device to use for AI.  -1 to probe for first AI subdevice (defaults to -1).")
MODULE_PARM(minordev_ao, "i")
MODULE_PARM_DESC(minordev_ao, "The minor number of the comedi device to use for AO.  -1 to probe for first AO subdevice (defaults to -1).")
MODULE_PARM(sampling_rate, "i")
MODULE_PARM_DESC(sampling_rate, "The sampling rate.  Defaults to " STR(DEFAULT_SAMPLING_RATE) ".")
MODULE_PARM(ai_sampling_rate, "i")
MODULE_PARM_DESC(ai_sampling_rate, "The sampling rate for asynch AI scans.  Note that this rate only takes effect when ai=asynch.  This is the rate at which to program the DAQ boad to do streaming AI.  Defaults to " STR(DEFAULT_AI_SAMPLING_RATE) ".")
MODULE_PARM(ai_settling_time, "i")
MODULE_PARM_DESC(ai_settling_time, "The amount of time, in microseconds, that it takes an AI channel to settle.  This is a function of the max sampling rate of the board. Defaults to " STR(DEFAULT_AI_SETTLING_TIME) ".")
MODULE_PARM(trigger_ms, "i")
MODULE_PARM_DESC(trigger_ms, "The amount of time, in milliseconds, to sustain trigger outputs.  Defaults to " STR(DEFAULT_TRIGGER_MS) ".")
MODULE_PARM(debug, "i")
MODULE_PARM_DESC(debug, "If true, print extra (cryptic) debugging output.  Defaults to 0.")
MODULE_PARM(avoid_redundant_writes, "i")
MODULE_PARM_DESC(avoid_redundant_writes, "If true, do not do comedi DIO writes during scans that generated no new output.  Defaults to 0 (false).")
MODULE_PARM(ai, "s")
MODULE_PARM_DESC(ai, "This can either be \"synch\" or \"asynch\" to determine whether we use asynch IO (comedi_cmd: faster, less compatible) or synch IO (comedi_data_read: slower, more compatible) when acquiring samples from analog channels.  Note that for asynch to work properly it needs a dedicated realtime interrupt.  Defaults to \""DEFAULT_AI"\".")
MODULE_PARM(jitter_tolerance_ns, "i")
MODULE_PARM_DESC(jitter_tolerance_ns, "The amount of jitter to tolerate before issuing warnings and recording a cycle as being \"jittery\".")
MODPARMS_END

/* TODO: Dynamically reconfigure these, and reconfigure comedi, each time
   the FSM changes and these counts change! */
#define FIRST_IN_CHAN (rs.states->routing.first_in_chan)
#define NUM_IN_CHANS (rs.states->routing.num_in_chans)
#define AFTER_LAST_IN_CHAN (FIRST_IN_CHAN+NUM_IN_CHANS)
#define FIRST_OUT_CHAN (FIRST_CONT_CHAN)
#define LAST_OUT_CHAN (FIRST_TRIG_CHAN+NUM_TRIG_CHANS)
#define NUM_AI_CHANS ((const unsigned)n_chans_ai_subdev)
#define NUM_AO_CHANS ((const unsigned)n_chans_ao_subdev)
#define NUM_DIO_CHANS ((const unsigned)n_chans_dio_subdev)
#define NUM_CONT_CHANS (rs.states->routing.num_cont_chans)
#define NUM_TRIG_CHANS (rs.states->routing.num_trig_chans)
#define NUM_CHANS (FIRST_IN_CHAN+NUM_IN_CHANS+NUM_CONT_CHANS+NUM_TRIG_CHANS)
#define FIRST_CONT_CHAN (IN_CHAN_TYPE == AI_TYPE ? 0 : AFTER_LAST_IN_CHAN)
#define FIRST_TRIG_CHAN (FIRST_CONT_CHAN+NUM_CONT_CHANS)
#define READY_FOR_TRIAL_JUMPSTATE (rs.states->ready_for_trial_jumpstate)
#define NUM_VTRIGS (rs.states->routing.num_vtrigs)
#define VTRIG_OFFSET /*(NUM_CHANS)*/ (NUM_TRIG_CHANS)
#define NUM_INPUT_EVENTS  (rs.states->routing.num_evt_cols)
#define NUM_IN_EVT_CHANS (NUM_IN_CHANS)
#define NUM_IN_EVT_COLS (rs.states->routing.num_evt_cols)
#define NUM_ROWS (rs.states->n_rows)
#define NUM_COLS (rs.states->n_cols)
#define TIMER_EXPIRED(state_timeout_us) ( (rs.current_ts - rs.current_timer_start) >= ((int64)(state_timeout_us))*1000LL )
#define RESET_TIMER() (rs.current_timer_start = rs.current_ts)
#define NUM_TRANSITIONS() ((rs.history.cur+1)%MAX_HISTORY)
#define IN_CHAN_TYPE ((const unsigned)rs.states->routing.in_chan_type)
#define AI_THRESHOLD_VOLTS_HI ((const unsigned)4)
#define AI_THRESHOLD_VOLTS_LOW ((const unsigned)3)
enum { SYNCH_MODE = 0, ASYNCH_MODE, UNKNOWN_MODE };
#define AI_MODE ((const unsigned)ai_mode)
#define FSM_PTR() ((struct FSMBlob *)(rs.states))
#define OTHER_FSM_PTR() ((struct FSMBlob *)(FSM_PTR() == &rs.states1 ? &rs.states2 : &rs.states1))
#define INPUT_ROUTING(x) (rs.states->routing.input_routing[(x)])
#define SW_INPUT_ROUTING(x) ( !rs.states->has_sched_waves ? -1 : rs.states->routing.sched_wave_input[(x)] )
#define SW_OUTPUT_ROUTING(x) ( !rs.states->has_sched_waves ? -1 : rs.states->routing.sched_wave_output[(x)] )
static volatile int rt_task_stop = 0; /* Internal variable to stop the RT 
					 thread. */
static volatile int rt_task_running = 0;

static struct SoftTask *buddyTask = 0, /* non-RT kernel-side process context
                                          buddy 'tasklet' */
                       *buddyTaskComedi = 0;
static pthread_t rt_task;
static comedi_t *dev = 0, *dev_ai = 0, *dev_ao = 0;
static unsigned subdev = 0, subdev_ai = 0, subdev_ao = 0, n_chans_ai_subdev = 0, n_chans_dio_subdev = 0, n_chans_ao_subdev = 0, maxdata_ai = 0, maxdata_ao = 0;

static unsigned long fsm_cycle_long_ct = 0, fsm_wakeup_jittered_ct = 0;
static unsigned long ai_n_overflows = 0;

/* Comedi CB stats */
static unsigned long cb_eos_skips = 0, cb_eos_skipped_scans = 0;

/* Remembered state of all DIO channels.  Bitfield array is indexed
   by DIO channel-id. */
unsigned dio_bits = 0, dio_bits_prev = 0, ai_bits = 0, ai_bits_prev = 0;
lsampl_t ai_thresh_hi = 0, /* Threshold, above which we consider it 
                              a digital 1 */
         ai_thresh_low = 0; /* Below this we consider it a digital 0. */

sampl_t ai_asynch_samples[MAX_AI_CHANS]; /* comedi_command callback copies samples to here */
void *ai_asynch_buf = 0; /* pointer to driver's DMA circular buffer for AI. */
comedi_krange ai_krange, ao_krange; 
unsigned long ai_asynch_buffer_size = 0; /* Size of driver's DMA circ. buf. */
unsigned ai_range = 0, ai_mode = UNKNOWN_MODE, ao_range = 0;
unsigned daq_ai_nchans = 0, daq_ai_chanmask = 0;
unsigned int lastTriggers; /* Remember the trigger lines -- these 
                              get shut to 0 after 1 cycle                */
uint64 cycle = 0; /* the current cycle */
uint64 trig_cycle = 0; /* The cycle at which a trigger occurred, useful for deciding when to clearing a trigger (since we want triggers to last trigger_ms) */
#define BILLION 1000000000
#define MILLION 1000000
uint64 task_period_ns = BILLION;
#define Sec2Nano(s) (s*BILLION)
static inline unsigned long Nano2Sec(unsigned long long nano);
static inline unsigned long Nano2USec(unsigned long long nano);
static inline long Micro2Sec(long long micro, unsigned long *remainder);
static inline long long timespec_to_nano(const struct timespec *);
static inline void timespec_add_ns(struct timespec *, long long ns);
static inline hrtime_t getNanosRTC(void);
static inline hrtime_t getNanosTSC(void);
static inline hrtime_t gethrtime(void) { return getNanosTSC(); }
static inline void UNSET_LYNX_TRIG(unsigned trig);
static inline void SET_LYNX_TRIG(unsigned trig);
static inline void CHK_AND_DO_LYNX_TRIG(unsigned trig);
static int my_comedi_get_krange(comedi_t *dev, unsigned subdev, unsigned chan, unsigned range, comedi_krange *out);

#define ABS(n) ( (n) < 0 ? -(n) : (n) )
/*---------------------------------------------------------------------------- 
  More internal 'global' variables and data structures.
-----------------------------------------------------------------------------*/
struct StateHistory
{
  /* The state history is a circular history.  It is a series of 
     state number/timestamp pairs, that keeps growing until it loops around
     to the beginning.                                                       */
  struct StateTransition transitions[MAX_HISTORY];
  unsigned cur;    /* Index into above array                                 */
};

struct RunState {
    /* TODO: verify this is smp-safe.                                        */
    
    /* FSM Specification */
    
    /* The actual state transition specifications -- 
       we have two of them since we swap them when a new FSM comes in from
       userspace to avoid dropping events during ITI. */
    struct FSMBlob    states1;
    struct FSMBlob    states2;
    struct FSMBlob   *states;

    /* End FSM Specification. */

    unsigned current_state;
    int64 current_ts; /* Time elapsed, in ns, since init_ts */
    int64 init_ts; /* Time of initialization, in nanoseconds.  Absolute
                      time derived from the Pentium's monotonically 
                      increasing TSC. */        
    int64 current_timer_start; /* When, in ns, the current timer  started 
                                  -- relative to current_ts. */
    unsigned previous_state; 
    

    int forced_event; /* If non-negative, force this event. */
    int forced_times_up; /* If true, time's up event will be forced this tick,
                            gets set from ShmMsg cmd from userspace..*/

    /* This gets populated from FIFO cmd, and if not empty, specifies
       outputs that are "always on". */
    unsigned int forced_outputs_mask; /**< Bitmask indexed by DIO channel-id */

    int valid; /* If this is true, the FSM task uses the state machine,
                  if false, the FSM task ignores the state machine.
                  useful when userspace wants to 'upload' a new state 
                  machine (acts like a sort of a lock).                      */

    int paused; /* If this is true, input lines do not lead to 
                   state transitions.  Useful when we want to temporarily 
                   pause state progression to play with the wiring, etc.
                   Note that this is almost identical to !valid flag
                   for all intents and purposes, but we differentiate from
                   invalid state machines and paused ones for now..          */

    int ready_for_trial_flg; /* If true, that means we got a 
                                READYFORTRIAL command from userspace.  
                                This indicates that the next time
                                we are in state 35, we should jump to state 0. 
                             */

  /** Mimes of the scheduled waves that came in from the FSMBlob.  These 
      mimics contain times, relative to current_ts, at which each of the
      components of the wave are due to take place: from edge-up, to
      edge-down, to wave termination (after the refractory period). */     
  struct ActiveWave {
    int64 edge_up_ts;   /**< from: current_ts + SchedWave::preamble_ns       */
    int64 edge_down_ts; /**< from: edge_up_ts + SchedWave::sustain_ns        */
    int64 end_ts;       /**< from: edge_down_ts + SchedWave::refractory_ns   */
  } active_wave[FSM_MAX_SCHED_WAVES];
  /** A quick mask of the currently active scheduled waves.  The set bits
      in this mask correspond to array indices of the active_wave
      array above. */
  unsigned active_wave_mask;
  /** A quick mask of the currently active AO waves.  The set bits
      in this mask correspond to array indices of the aowaves array outside 
      this struct. */
  unsigned active_ao_wave_mask; 
  /** Store pointers to analog output wave data -- 
   *  Note: to avoid memory leaks make sure initRunState() is never called when
   *  we have active valid pointers here!  This means that aowaves should be cleaned
   *  up (freed) using cleanupAOWaves() whenever we get a new FSM.  cleanupAoWaves()
   *  needs to be called in Linux process context. */
  struct AOWaveINTERNAL 
  {
    unsigned aoline, nsamples, loop, cur;
    unsigned short *samples;
    signed char *evt_cols;
  } aowaves[FSM_MAX_SCHED_WAVES];

  /* This *always* should be at the end of this struct since we don't clear
     the whole thing in initRunState(), but rather clear bytes up until
     this point! */
    struct StateHistory history;             /* Our state history record.   */
};

static volatile struct RunState rs;


/*---------------------------------------------------------------------------
 Some helper functions
-----------------------------------------------------------------------------*/
static inline volatile struct StateTransition *historyTop(void);
static inline void historyPush(int event_id);

static int gotoState(unsigned state_no, int event_id_for_history); /**< returns 1 if new state, 0 if was the same and no real transition ocurred, -1 on error */
static unsigned long detectInputEvents(void); /**< returns 0 on no input detected, otherwise returns bitfield array of all the events detected -- each bit corresponds to a state matrix "in event column" position, eg center in is bit 0, center out is bit 1, left-in is bit 2, etc */
static int doSanityChecksStartup(void); /**< Checks mod params are sane. */
static int doSanityChecksRuntime(void); /**< Checks FSM input/output params */
static void doOutput(void);
static void clearAllOutputLines(void);
static inline void clearTriggerLines(void);
static void dispatchEvent(unsigned event_id);
static void handleFifos(void);
static inline void dataWrite(unsigned chan, unsigned bit);
static void commitDataWrites(void);
static void grabAllDIO(void);
static void grabAI(void); /* AI version of above.. */
static void doDAQ(void); /* does the data acquisition for remaining channels that grabAI didn't get.  See STARTDAQ fifo cmd. */
static unsigned long processSchedWaves(void); /**< updates active wave state, does output, returns event id mask of any waves that generated input events (if any) */
static unsigned long processSchedWavesAO(void); /**< updates active wave state, does output, returns event id mask of any waves that generated input events (if any) */
static void scheduleWave(unsigned wave_id, int op);
static void scheduleWaveDIO(unsigned wave_id, int op);
static void scheduleWaveAO(unsigned wave_id, int op);
static void stopActiveWaves(void); /* called when FSM starts a new trial */
static void updateHasSchedWaves(void);

/* just like clock_gethrtime but instead it used timespecs */
static inline void nano2timespec(hrtime_t time, struct timespec *t);
/* 64-bit division is not natively supported on 32-bit processors, so it must 
   be emulated by the compiler or in the kernel, by this function... */
/* this function is non-reentrant! */
static const char *uint64_to_cstr(uint64 in);
/* this function is reentrant! */
static int uint64_to_cstr_r(char *buf, unsigned bufsz, uint64 num);
static unsigned long transferCircBuffer(void *dest, const void *src, unsigned long offset, unsigned long bytes, unsigned long bufsize);
/* Place an unsigned int into the debug fifo.  This is currently used to
   debug AI 0 reads.  Only useful for development.. */
static inline void putDebugFifo(unsigned value);
static void printStats(void);
static void buddyTaskHandler(void *arg);
static void buddyTaskComediHandler(void *arg);

/*-----------------------------------------------------------------------------*/

int init (void)
{
  int retval = 0;
  
  if (    (retval = initTaskPeriod())
       || (retval = initShm())
       || (retval = initFifos())
       || (retval = initRunState())
       || (retval = initBuddyTask())
       || (retval = initComedi())
       || (retval = initRT())
       || (retval = doSanityChecksStartup()) )
    {
      cleanup();  
      return retval;
    }  
  
  LOG_MSG("started successfully at %d Hz with %d ms triggers.\n", sampling_rate, trigger_ms);
  
  return retval;
}

void cleanup (void)
{

  if (rt_task_running) {
    rt_task_stop = 1;
    pthread_cancel(rt_task);
    pthread_join(rt_task, 0);
  }
  cleanupAOWaves();

  if (buddyTask) softTaskDestroy(buddyTask);
  buddyTask = 0;
  if (buddyTaskComedi) softTaskDestroy(buddyTaskComedi);
  buddyTaskComedi = 0;

  if (dev) {
    comedi_unlock(dev, subdev);
    comedi_close(dev);
    dev = 0;
  }

  if (dev_ai) {
    comedi_cancel(dev_ai, subdev_ai);
    comedi_unlock(dev_ai, subdev_ai);
    /* Cleanup any comedi_cmd */
    /*if ( AI_MODE == ASYNCH_MODE )
      comedi_register_callback(dev_ai, subdev_ai, 0, 0, 0);*/
    comedi_close(dev_ai);
    dev_ai = 0;
  }

  if (dev_ao) {
    comedi_unlock(dev_ao, subdev_ao);
    comedi_close(dev_ao);
    dev_ao = 0;
  }

  if (shm)  { 
    if (shm->fifo_in >= 0) rtf_destroy(shm->fifo_in);
    if (shm->fifo_out >= 0) rtf_destroy(shm->fifo_out);
    if (shm->fifo_trans >= 0) rtf_destroy(shm->fifo_trans);
    if (shm->fifo_daq >= 0) rtf_destroy(shm->fifo_daq);
    if (shm->fifo_debug >= 0) rtf_destroy(shm->fifo_debug);
    mbuff_free(SHM_NAME, (void *)shm);
    shm = 0; 
  }
  printStats();
}

static void printStats(void)
{
  if (debug && cb_eos_skips) 
    DEBUG("skipped %lu times, %lu scans\n", cb_eos_skips, cb_eos_skipped_scans);
  
  LOG_MSG("unloaded successfully after %s cycles.\n", uint64_to_cstr(cycle));
}

static int initBuddyTask(void)
{
  buddyTask = softTaskCreate(buddyTaskHandler, MODULE_NAME" Buddy Task");
  if (!buddyTask) return -ENOMEM;
  buddyTaskComedi = softTaskCreate(buddyTaskComediHandler, MODULE_NAME" Comedi Buddy Task");
  if (!buddyTaskComedi) return -ENOMEM;
  return 0;
}

static int initShm(void)
{
  shm = mbuff_alloc(SHM_NAME, SHM_SIZE);
  if (!shm) return -EINVAL;
  memset((void *)shm, 0, sizeof(*shm));
  shm->magic = SHM_MAGIC;
  
  shm->fifo_trans = shm->fifo_out = shm->fifo_in = shm->fifo_debug = -1;

  return 0;
}

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
static int initFifos(void)
{
  int32 err, minor;

  /* Open up fifos here.. */
  err = find_free_rtf(&minor, FIFO_SZ);
  if (err < 0) return 1;
  shm->fifo_out = minor;
  err = find_free_rtf(&minor, FIFO_SZ);
  if (err < 0) return 1;
  shm->fifo_in = minor;
  err = find_free_rtf(&minor, FIFO_TRANS_SZ);
  if (err < 0) return 1;
  shm->fifo_trans = minor;
  err = find_free_rtf(&minor, FIFO_DAQ_SZ);
  if (err < 0) return 1;
  shm->fifo_daq = minor;

  if (debug) {
    err = find_free_rtf(&minor, 3072); /* 3kb fifo enough? */  
    if (err < 0) return 1;
    shm->fifo_debug = minor;
    DEBUG("FIFOS: in %d out %d trans %d Debug %d\n", shm->fifo_in, shm->fifo_out, shm->fifo_trans, shm->fifo_debug);
  }

  return 0;  
}

static int initRunState(void)
{
  /* Clear the runstate memory area.. note how we only clear the beginning
     and don't touch the state history since it is rather large! */
  memset((void *)&rs, 0, sizeof(rs) - sizeof(struct StateHistory));
  rs.states = (struct FSMBlob *)&rs.states1;

  /* Now, initialize the history.. */
  rs.history.cur = MAX_HISTORY-1; /* Initially point to 'end' of history 
				     to indicate no history. */
  memset((void *)&rs.history.transitions[0], 0, sizeof(rs.history.transitions[0]));
  
  /* Grab current time from gethrtime() which is really the pentium TSC-based 
     timer  on most systems. */
  rs.init_ts = gethrtime();

  RESET_TIMER();

  rs.forced_event = -1; /* Negative value here means not forced.. this needs
                           to always reset to negative.. */
  rs.paused = 1; /* By default the FSM is paused initially. */
  rs.valid = 0; /* Start out with an 'invalid' FSM since we expect it
                   to be populated later from userspace..               */
  
  return 0;  
}

static int initComedi(void) 
{
  int n_chans = 0, ret;

  ai_mode = 
    (!strcmp(ai, "synch")) 
    ? SYNCH_MODE 
    : ( (!strcmp(ai, "asynch")) 
        ? ASYNCH_MODE 
        : UNKNOWN_MODE );
  
  if (!dev) {
    int sd;

    sprintf(COMEDI_DEVICE_FILE, "/dev/comedi%d", minordev);
    dev = comedi_open(COMEDI_DEVICE_FILE);
    if (!dev) {
      ERROR("Cannot open Comedi device at %s\n", COMEDI_DEVICE_FILE);
      return -comedi_errno();
    }

    sd = comedi_find_subdevice_by_type(dev, COMEDI_SUBD_DIO, 0);    
    if (sd < 0 || (n_chans = comedi_get_n_channels(dev, sd)) <= 0) {
      LOG_MSG("DIO subdevice could not be found.\n");
      comedi_close(dev);
      dev = 0;
      return -ENODEV;
    }

    subdev = sd;
    n_chans_dio_subdev = n_chans;
    comedi_lock(dev, subdev);
  }

  DEBUG("COMEDI: n_chans /dev/comedi%d = %u\n", minordev, n_chans);  

  reconfigureDIO();

  /* Set up AI subdevice for synch/asynch acquisition in case we ever opt to 
     use AI for input. */
  ret = initAISubdev();
  if ( ret ) return ret;    

  /* Probe for or open the AO subdevice for analog output (sched waves to AO)*/
  ret = initAOSubdev();
  if ( ret ) return ret;

  return 0;
}

static void reconfigureDIO(void)
{
  int n_chans, reconf_ct = 0;
  unsigned i;
  static int old_modes[256], old_modes_init = 0;
  hrtime_t start;

  start = gethrtime();

  if (!old_modes_init) {
    for (i = 0; i < sizeof(old_modes)/sizeof(*old_modes); ++i)
      old_modes[i] = -5005;
    old_modes_init = 1;
  }
  n_chans = comedi_get_n_channels(dev, subdev);
  /* First, clear all channels, set them to zero */
  /* NB: don't do this since this can interfere with ITI in when we get a new FSM */
  /*for (i = 0; i < n_chans; ++i) 
    if ( comedi_dio_config(dev, subdev, i, COMEDI_OUTPUT) != 1 )
      WARNING("comedi_dio_config returned error for channel %u mode %d\n", i, (int)COMEDI_OUTPUT);
      
  { 
    unsigned bits = 0;
    if ( comedi_dio_bitfield(dev, subdev, 0xffffffff, &bits) < 0 )
      WARNING("comedi_dio_bitfield returned error for channel %u mode\n", i);
  }
  */
  /* Now, setup channel modes correctly */
  for (i = 0; i < (unsigned)n_chans; ++i) {
    unsigned mode = (i >= FIRST_IN_CHAN && i < AFTER_LAST_IN_CHAN && IN_CHAN_TYPE == DIO_TYPE) ? COMEDI_INPUT : COMEDI_OUTPUT;
    int *old_mode = (i < sizeof(old_modes)/sizeof(*old_modes)) ? &old_modes[i] : 0;
    if (old_mode && *old_mode == (int)mode) continue; /* don't redundantly configure.. */
    ++reconf_ct;
    if ( comedi_dio_config(dev, subdev, i, mode) != 1 )
        WARNING("comedi_dio_config returned error for channel %u mode %d\n", i, (int)mode);
    
    if (debug > 1) DEBUG("COMEDI: comedi_dio_config %u %d\n", i, (int)mode);
    if (old_mode) *old_mode = mode; /* remember old configuration.. */
  }

  if (reconf_ct)
    LOG_MSG("Cycle %lu: Reconfigured %d DIO chans in %lu nanos.\n", (unsigned long)cycle, reconf_ct, (unsigned long)(gethrtime()-start));
}

static int initAISubdev(void)
{
  int m, s = -1, n, i, range = -1;
  int minV = INT_MIN, maxV = INT_MAX;

  if (minordev_ai >= 0) {
      /* They specified an AI subdevice, so try and use it. */

      sprintf(COMEDI_DEVICE_FILE, "/dev/comedi%d", minordev_ai);    
      if ( !(dev_ai = comedi_open(COMEDI_DEVICE_FILE)) ) {
        ERROR("Cannot open Comedi device at %s\n", COMEDI_DEVICE_FILE);
        return -EINVAL;
      }
      s = comedi_find_subdevice_by_type(dev_ai, COMEDI_SUBD_AI, 0);
      if (s < 0) {
        ERROR("Could not find any AI subdevice on %s!\n", COMEDI_DEVICE_FILE);
        return -EINVAL;          
      }
      subdev_ai = s;
  } else { /* They specified probe of AI:  minordev_ai < 0 */

      /* Now, attempt to probe AI subdevice, etc. */
      while (minordev_ai < 0) {
        for (m = 0; m < COMEDI_NDEVICES; ++m) {
          sprintf(COMEDI_DEVICE_FILE, "/dev/comedi%d", m);
          if ( (dev_ai = comedi_open(COMEDI_DEVICE_FILE)) ) {
            s = comedi_find_subdevice_by_type(dev_ai, COMEDI_SUBD_AI, 0);
            if (s >= 0) {
              n = comedi_get_n_channels(dev_ai, s);
              if (n < 1) {
                WARNING("%s, subdev %d not enough AI channels (we require %d but found %d).\n", COMEDI_DEVICE_FILE, s, (int)1, n);
              } else {
                n_chans_ai_subdev = n;
                break; /* Everything's copacetic, skip the close code below */
              }
            } 
            /* AI subdev not found or incompatible, try next minor, closing current dev_ai first */
            comedi_close(dev_ai);
            dev_ai = 0;              
          }
        }
        if (s < 0) {
          /* Failed to probe/find an AI subdev. */
          ERROR("Could not find any AI subdevice on any comedi devices on the system!\n");
          return -EINVAL;
        } else {
          minordev_ai = m;
          subdev_ai = s;
        }
      }
  }
  /* we got a dev_ai and subdev_ai unconditionally at this point, so lock it */
  comedi_lock(dev_ai, subdev_ai);

  /* set up the ranges we want.. which is the closest range to 0-5V */
  n = comedi_get_n_ranges(dev_ai, subdev_ai, 0);
  for (i = 0; i < n; ++i) {
    my_comedi_get_krange(dev_ai, subdev_ai, 0, i, &ai_krange);
    if (RF_UNIT(ai_krange.flags) == UNIT_volt /* If it's volts we're talking 
                                                 about */
        && minV < ai_krange.min && maxV > ai_krange.max /* And this one is 
                                                           narrower than the 
                                                           previous */
        && ai_krange.min <= 0 && ai_krange.max >= 5*1000000) /* And it 
                                                            encompasses 0-5V */
      {
        /* Accept this range, save it, and determine the ai_thresh value
           which is important for making decisions about what a '1' or '0' is. 
        */
        maxdata_ai =  comedi_get_maxdata(dev_ai, subdev_ai, 0);

        ai_range = range = i;
        minV = ai_krange.min;
        maxV = ai_krange.max;
        /* Determine threshold, we set it to 4 Volts below... */
        if (maxV != minV) {
          /* NB: this tmpLL business is awkward but if avoids integer underflow
             and/or overflow and is necessary.  The alternative is to use 
             floating point which is prohibited in the non-realtime kernel! */
          long long tmpLL = AI_THRESHOLD_VOLTS_HI * 1000000LL - (long long)minV;
          tmpLL *= (long long)maxdata_ai;
          ai_thresh_hi =  tmpLL / (maxV - minV);
          tmpLL = AI_THRESHOLD_VOLTS_LOW * 1000000LL - (long long)minV;
          tmpLL *= (long long)maxdata_ai;
          ai_thresh_low = tmpLL / (maxV - minV);
        } else 
          /* This should not occur, just here to avoid divide-by-zero.. */
          ai_thresh_hi = ai_thresh_low = 0;
      }
  }
  if (range < 0) {
    WARNING("Could not determine any valid range for %s AI subdev!  Defaulting to 0-5V with comedi range id = 0!\n", COMEDI_DEVICE_FILE);
    /* now fudge the range settings.. */
    minV = ai_krange.min = 0;
    maxV = ai_krange.max = 5000000;
    ai_krange.flags = UNIT_volt;
    ai_range = 0;
    maxdata_ai  = comedi_get_maxdata(dev_ai, subdev_ai, 0);
    ai_thresh_hi = AI_THRESHOLD_VOLTS_HI * 10 / 5 * maxdata_ai / 10;
    ai_thresh_low = AI_THRESHOLD_VOLTS_LOW * 10 / 5 * maxdata_ai / 10;    
  }
  DEBUG("AI dev: %s subdev: %d range: %d min: %d max: %d thresh (%dV-%dV): %u-%u maxdata: %u \n", COMEDI_DEVICE_FILE, (int)subdev_ai, (int)ai_range, minV, maxV, AI_THRESHOLD_VOLTS_LOW, AI_THRESHOLD_VOLTS_HI, ai_thresh_low, ai_thresh_hi, maxdata_ai);

  /* Setup comedi_cmd */
  if ( AI_MODE == ASYNCH_MODE ) {
    int err = setupComediCmd();
    if (err) return err;
  }

  return 0;
}

static int initAOSubdev(void)
{
  int m, s = -1, n, i, range = -1;
  int minV = INT_MIN, maxV = INT_MAX;

  if (minordev_ao >= 0) {
      /* They specified an AO subdevice, so try and use it. */

      sprintf(COMEDI_DEVICE_FILE, "/dev/comedi%d", minordev_ao);    
      if ( !(dev_ao = comedi_open(COMEDI_DEVICE_FILE)) ) {
        ERROR("Cannot open Comedi device at %s\n", COMEDI_DEVICE_FILE);
        return -EINVAL;
      }
      s = comedi_find_subdevice_by_type(dev_ao, COMEDI_SUBD_AO, 0);
      if (s < 0) {
        ERROR("Could not find any AO subdevice on %s!\n", COMEDI_DEVICE_FILE);
        return -EINVAL;          
      }
      subdev_ao = s;
  } else { /* They specified probe:  minordev_ao < 0 */

      /* Now, attempt to probe AO subdevice, etc. */
      while (minordev_ao < 0) {
        for (m = 0; m < COMEDI_NDEVICES; ++m) {
          sprintf(COMEDI_DEVICE_FILE, "/dev/comedi%d", m);
          if ( (dev_ao = comedi_open(COMEDI_DEVICE_FILE)) ) {
            s = comedi_find_subdevice_by_type(dev_ao, COMEDI_SUBD_AO, 0);
            if (s >= 0) {
              n = comedi_get_n_channels(dev_ao, s);
              if (n < 1) {
                WARNING("%s, subdev %d not enough AO channels (we require %d but found %d).\n", COMEDI_DEVICE_FILE, s, (int)1, n);
              } else {
                n_chans_ao_subdev = n;
                break; /* Everything's copacetic, skip the close code below */
              }
            } 
            /* AO subdev not found or incompatible, try next minor, closing current dev_ao first */
            comedi_close(dev_ao);
            dev_ao = 0;
          }
        }
        if (s < 0) {
          /* Failed to probe/find an AO subdev. */
          ERROR("Could not find any AO subdevice on any comedi devices on the system!\n");
          return -EINVAL;
        } else {
          minordev_ao = m;
          subdev_ao = s;
        }
      }
  }
  /* we got a dev_ao and subdev_ao unconditionally at this point, so lock it */
  comedi_lock(dev_ao, subdev_ao);

  /* set up the ranges we want.. which is the closest range to 0-5V */
  n = comedi_get_n_ranges(dev_ao, subdev_ao, 0);
  for (i = 0; i < n; ++i) {
    my_comedi_get_krange(dev_ao, subdev_ao, 0, i, &ao_krange);
    if (RF_UNIT(ao_krange.flags) == UNIT_volt /* If it's volts we're talking 
                                                 about */
        && minV < ao_krange.min && maxV > ao_krange.max /* And this one is 
                                                           narrower than the 
                                                           previous */
        && ao_krange.min <= 0 && ao_krange.max >= 5*1000000) /* And it 
                                                            encompasses 0-5V */
      {
        /* Accept this range, save it. */
        maxdata_ao =  comedi_get_maxdata(dev_ao, subdev_ao, 0);

        ao_range = range = i;
        minV = ao_krange.min;
        maxV = ao_krange.max;
      }
  }
  if (range < 0) {
    ERROR("Could not determine any valid range for %s AO subdev!\n", COMEDI_DEVICE_FILE);
    return -EINVAL;
  }  
  DEBUG("AO dev: %s subdev: %d range: %d min: %d max: %d maxdata: %u \n", COMEDI_DEVICE_FILE, (int)subdev_ao, (int)ao_range, minV, maxV, maxdata_ao);

  return 0;
}

static inline unsigned long transferCircBuffer(void *dest, 
                                               const void *src, 
                                               unsigned long offset, 
                                               unsigned long bytes, 
                                               unsigned long bufsize)
{
  char *d = (char *)dest;
  const char *s = (char *)src;
  unsigned long nread = 0;
  while (bytes) {
    unsigned long n = bytes;
    if ( offset + n > bufsize) { 
      /* buffer wrap-around condition.. */
      if (debug > 1) DEBUG("transferCircBuffer: buffer wrapped around!\n");
      n = bufsize - offset;
    }
    if (!n) {
      offset = 0;
      continue;
    }
    memcpy(d + nread, s + offset, n);
    if (debug > 1) DEBUG("transferCircBuffer: copied %lu bytes!\n", n);
    bytes -= n;
    offset += n;
    nread += n;
  }  
  return offset;
}

/** Called by comedi during asynch IO to tell us a scan ended. */
static int comediCallback(unsigned int mask, void *ignored)
{
  char timeBuf[22] = {0};
  (void)ignored;

  if (debug > 1) {
    /* used in debug sections below.. */
    uint64_to_cstr_r(timeBuf, sizeof(timeBuf), gethrtime()); 
  }

  if (mask & COMEDI_CB_EOA) {
    /* Ignore EOA */
    DEBUG("comediCallback: got COMEDI_CB_EOA.\n");
  }
  if (mask & COMEDI_CB_ERROR) {
    WARNING("comediCallback: got COMEDI_CB_ERROR!\n");    
  }
  if (mask & COMEDI_CB_OVERFLOW) {
    ++ai_n_overflows;
    WARNING("comediCallback: got COMEDI_CB_OVERFLOW! Attempting to restart acquisition!\n");
    comedi_cancel(dev_ai, subdev_ai);
    /* slow operation to restart the comedi command, so just pend it
       to non-rt buddy task which runs in process context and can take its 
       sweet-assed time.. */
    softTaskPend(buddyTaskComedi, (void *)1); 
    return 0;
  }
  if (mask & COMEDI_CB_BLOCK) {
    if (debug > 2) 
        DEBUG("comediCallback: got COMEDI_CB_BLOCK at abs. time %s.\n", timeBuf);
  }
  if (mask & COMEDI_CB_EOS) {
    /* This is what we want.. EOS. Now copy scans from the comedi driver 
       buffer: ai_asynch_buf, to our local data structure for feeding to 
       RT... */
    int  offset   = comedi_get_buffer_offset(dev_ai, subdev_ai),
         numBytes = comedi_get_buffer_contents(dev_ai, subdev_ai);
    const int oneScanBytes = sizeof(sampl_t)*NUM_AI_CHANS;
    int  numScans = numBytes / (oneScanBytes ? oneScanBytes : numBytes+1),
         lastScanOffset = (offset + ((numScans-1)*oneScanBytes))
                          % ai_asynch_buffer_size;
    char *buf = (char *)ai_asynch_buf;

    if (debug > 2) 
      DEBUG("comediCallback: got COMEDI_CB_EOS at abs. time %s, with %d bytes (offset: %d) of data.\n", timeBuf, numBytes, offset);
    
    if (numScans > 1) {
      if (debug > 2) DEBUG("COMEDI_CB_EOS with more than one scan's data:  Expected %d, got %d.\n", oneScanBytes, numBytes);
      cb_eos_skips++;
      cb_eos_skipped_scans += numScans-1;
    } else if (numScans <= 0) {
      static int onceOnly = 0;
      if (!onceOnly) {
        ERROR("**** BUG in COMEDI driver!  COMEDI_CB_EOS callback called with less than one scan's data available.  Expected: %d, got %d!\n", oneScanBytes, numBytes);
        onceOnly = 1;
        /*comedi_cancel(dev_ai, subdev_ai);*/
      }        
      return -EINVAL;
    }

    /* NB: We *need* to pull exactly one scan.. not more!  It didn't
       occur to me initially that this callback sometimes gets called
       late, when the board already has put some samples of the next
       scan in the DMA buffer for us!  Thus, it is bad bad bad to
       consume all available data.  Instead, we should just consume
       exactly the last full scan available (but not any partial
       scans!).  Note that it is also theoretically possible that we
       were called so late that more than one scan is available, so we
       will take the latest scan.
    */
    
    /* 
       Points to remember:

       1. Consume everything up to and including the last whole scan only. 
       
       2. Leave partial scans in the DMA buffer.  We need to maintain the 
          invariant that the buffer always starts on a scan boundary. 
    */
    { 
      /*      unsigned long flags; */
      
      /*       rtl_critical(flags); /\* Lock machine, disable interrupts.. to avoid */
/*                               race conditions with RT since we write to  */
/*                               ai_asynch_samples. *\/ */

      /* Read only the latest full scan available.. */
      lastScanOffset = 
        transferCircBuffer(ai_asynch_samples, /* dest */
                           buf,  /* src */
                           lastScanOffset, /* offset into src */
                           oneScanBytes, /* num */
                           ai_asynch_buffer_size); /* circ buf size */

      /* consume all full scans up to present... */
      comedi_mark_buffer_read(dev_ai, subdev_ai, numScans * oneScanBytes);
/*       rtl_end_critical(flags); /\* Unlock machine, reenable interrupts... *\/ */

      /* puts chan0 to the debug fifo iff in debug mode */
      putDebugFifo(ai_asynch_samples[0]); 
    }

  } /* end if COMEDI_CB_EOS */
  return 0;
}

/** This function sets up asynchronous streaming acquisition for AI
    input.  It is a pain to get working and doesn't work on all boards. */
static int setupComediCmd(void)
{
  extern int comedi_map(comedi_t *, unsigned, void *); /* prevent compiler warnings */

  comedi_cmd cmd;
  int err, i;
  unsigned int chanlist[MAX_AI_CHANS];

  /* Clear sample buffers.. */
  memset(ai_asynch_samples, 0, sizeof(ai_asynch_samples));

  /* First, setup our callback func. */
  (void)&comediCallback; /* prevent compiler warnings.. */
  //err = comedi_register_callback(dev_ai, subdev_ai,  COMEDI_CB_EOA|COMEDI_CB_ERROR|COMEDI_CB_OVERFLOW|COMEDI_CB_EOS/*|COMEDI_CB_BLOCK*/, comediCallback, 0);
  ERROR("comedi asynch not yet supported!");
  return -1;

  if ( err ) {
    ERROR("comedi_register_callback returned %d, failed to setup comedi_cmd.\n", err);
    return err;
  }

  /* Next, setup our comedi_cmd */
  memset(&cmd, 0, sizeof(cmd));
  cmd.subdev = subdev_ai;
  cmd.flags = TRIG_WAKE_EOS|TRIG_RT|TRIG_ROUND_DOWN; /* do callback every scan, try to use RT, round period down */
  cmd.start_src = TRIG_NOW;
  cmd.start_arg = 0;
  cmd.scan_begin_src = TRIG_TIMER;

  if (ai_sampling_rate < sampling_rate) {
    ai_sampling_rate = sampling_rate;
    WARNING("setupComediCmd: ai_sampling rate was too low.  Forced it to %d.\n", ai_sampling_rate);
  }

  cmd.scan_begin_arg = BILLION / ai_sampling_rate; /* Pray we don't overflow here */
  cmd.convert_src = TRIG_TIMER; 
  cmd.convert_arg =  ai_settling_time * 1000; /* settling time in nanos */ 
/*   cmd.convert_arg = 0; /\* try to avoid intersample delay?? *\/ */
  cmd.scan_end_src = TRIG_COUNT;
  cmd.scan_end_arg = NUM_AI_CHANS;
  cmd.stop_src = TRIG_NONE;
  cmd.stop_arg = 0;
  /*cmd.data = ai_asynch_samples;
    cmd.data_len = NUM_AI_CHANS; */
  
  for (i = 0; i < (int)NUM_AI_CHANS; ++i) 
    chanlist[i] = CR_PACK(i, ai_range, AREF_GROUND);
  cmd.chanlist = chanlist;
  cmd.chanlist_len = i; 

  err = comedi_command_test(dev_ai, &cmd);
  if (err == 3)  err = comedi_command_test(dev_ai, &cmd); 
  if (err != 4 && err != 0) {
    ERROR("Comedi command could not be started, comedi_command_test returned: %d!\n", err);
    return err;
  }

  /* obtain a pointer to the command buffer */
  err = comedi_map(dev_ai, subdev_ai, (void *)&ai_asynch_buf);
  if (err) {
    ERROR("Comedi command could not be started, comedi_map() returned: %d!\n", err);
    return err;
  }
  ai_asynch_buffer_size = comedi_get_buffer_size(dev_ai, subdev_ai);
  DEBUG("Comedi Asynch buffer at 0x%p of size %ld\n", ai_asynch_buf, ai_asynch_buffer_size);

  err = comedi_command(dev_ai, &cmd);
  if (err) {
    ERROR("Comedi command could not be started, comedi_command returned: %d!\n", err);
    return err;
  }

  if (cmd.scan_begin_arg != BILLION / (unsigned)ai_sampling_rate) {
    WARNING("Comedi Asynch IO rate requested was %d, got %lu!\n", BILLION / ai_sampling_rate, (unsigned long)cmd.scan_begin_arg);
  }

  LOG_MSG("Comedi Asynch IO started with period %u ns.\n", cmd.scan_begin_arg);
  
  return 0;
}

static int initTaskPeriod(void)
{
  /* setup task period.. */
  if (sampling_rate <= 0 || sampling_rate >= BILLION) {
    LOG_MSG("Sampling rate of %d seems crazy, going to default of %d\n", sampling_rate, DEFAULT_SAMPLING_RATE);
    sampling_rate = DEFAULT_SAMPLING_RATE;
  }
  task_period_ns = ((unsigned long long)BILLION) / (unsigned long long)sampling_rate;

  return 0;
}

static int initRT(void)
{
#ifdef USE_OWN_STACK
#define TASK_STACKSIZE 8192
  static char TASK_STACK[TASK_STACKSIZE];
#endif
  pthread_attr_t attr;
  struct sched_param sched_param;
  int error;

  /* Setup pthread data, etc.. */
  pthread_attr_init(&attr);
  /*pthread_attr_setfp_np(&attr, 1);*/
#ifdef USE_OWN_STACK
  error = pthread_attr_setstackaddr(&attr, TASK_STACK);
  if (error) return -error;
  error = pthread_attr_setstacksize(&attr, TASK_STACKSIZE);  
  if (error) return -error;
#endif  
  error = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
  if (error) return -error;
  sched_param.sched_priority = sched_get_priority_max(SCHED_FIFO);
  error = pthread_attr_setschedparam(&attr, &sched_param);  
  if (error) return -error;
  error = pthread_create(&rt_task, &attr, doFSM, 0);
  if (error) return -error;
  return 0;
}

/**< Checks that mod params are sane. */
static int doSanityChecksStartup(void)
{
  int ret = 0;
  
  if (AI_MODE != SYNCH_MODE && AI_MODE != ASYNCH_MODE) {
    ERROR("ai= module parameter invalid.  Please pass one of \"asynch\" or \"synch\".\n");
    ret = -EINVAL;
  }
          
  if (ai_settling_time <= 0) {
    WARNING("AI settling time of %d too small!  Setting it to 1 microsecond.\n",            ai_settling_time);
    ai_settling_time = 1;
  }

  return ret;
}

static int doSanityChecksRuntime(void)
{
  unsigned n_chans = n_chans_dio_subdev + NUM_AI_CHANS;
  int ret = 0;
  char buf[512];
  static int first_time = 1;

  if (READY_FOR_TRIAL_JUMPSTATE >= NUM_ROWS || READY_FOR_TRIAL_JUMPSTATE <= 0)  
    WARNING("ready_for_trial_jumpstate of %d need to be between 0 and %d!\n", 
            (int)READY_FOR_TRIAL_JUMPSTATE, (int)NUM_ROWS); 
  
  if (dev && n_chans != NUM_CHANS && debug)
    WARNING("COMEDI devices have %d input channels which differs from the number of input channels indicated by the sum of FSM parameters num_in_chans+num_cont_chans+num_trig_chans=%d!\n", n_chans, NUM_CHANS);

/*   if (NUM_IN_CHANS < NUM_IN_EVT_CHANS)  */
/*     ERROR("num_in_chans of %d is less than hard-coded NUM_IN_EVT_CHANS of %d!\n", (int)NUM_IN_CHANS, (int)NUM_IN_EVT_CHANS), ret = -EINVAL; */

  if ( dev && n_chans < NUM_CHANS) 
    ERROR("COMEDI device only has %d input channels, but FSM parameters require at least %d channels!\n", n_chans, NUM_CHANS), ret = -EINVAL;
  
  if (IN_CHAN_TYPE == AI_TYPE && AFTER_LAST_IN_CHAN > MAX_AI_CHANS) 
    ERROR("The input channels specified (%d-%d) exceed MAX_AI_CHANS (%d).\n", (int)FIRST_IN_CHAN, ((int)AFTER_LAST_IN_CHAN)-1, (int)MAX_AI_CHANS), ret = -EINVAL;
  
  if (ret) return ret;

  if (first_time || debug) {
    LOG_MSG("Number of physical input channels is %d; Number actually used is %d; Virtual triggers start at 2^%d with %d total vtrigs (bit 2^%d is sound stop bit!)\n", n_chans, NUM_CHANS, VTRIG_OFFSET, NUM_VTRIGS, NUM_VTRIGS ? VTRIG_OFFSET+NUM_VTRIGS-1 : 0);

    sprintf(buf, "in chans: %d-%d ", FIRST_IN_CHAN, ((int)AFTER_LAST_IN_CHAN)-1);
    if (NUM_CONT_CHANS)
      sprintf(buf + strlen(buf), "cont chans: %d-%d ", FIRST_CONT_CHAN, FIRST_CONT_CHAN+NUM_CONT_CHANS-1);
    if (NUM_TRIG_CHANS)
      sprintf(buf+strlen(buf), "trig chans: %d-%d",FIRST_CONT_CHAN+NUM_CONT_CHANS,FIRST_CONT_CHAN+NUM_CONT_CHANS+NUM_TRIG_CHANS-1);
    LOG_MSG("%s\n", buf);
    first_time = 0;
  }

  return 0;
}

static inline int triggersExpired(void)
{
  return cycle - trig_cycle >= ((unsigned)sampling_rate/1000)*(unsigned)trigger_ms;
}

static inline void resetTriggerTimer(void)
{
  trig_cycle = cycle;
}

/**
 * This function does the following:
 *  Detects edges, does state transitions, updates state transition history,
 *  and also reads commands off the real-time fifo.
 *  This function is called by rtlinux scheduler every task period... 
 *  @see init()
 */
static void *doFSM (void *arg)
{
  struct timespec next_task_wakeup;
  hrtime_t lastT0 = 0, cycleT0 = 0, cycleTf = 0;
  long long tmpts;

  (void)arg;

  rt_task_running = 1;

  clock_gettime(CLOCK_REALTIME, &next_task_wakeup);
    
  while (! rt_task_stop) {
    lastT0 = cycleT0;
    cycleT0 = gethrtime();
    
    if (cycle++) { 
      /* see if we woke up jittery/late.. */
      //tmpts = timespec_to_nano(&next_task_wakeup);
      tmpts = lastT0 + task_period_ns;
      tmpts = ((long long)cycleT0) - tmpts;
      if ( ABS(tmpts) > jitter_tolerance_ns ) {
        ++fsm_wakeup_jittered_ct;
        WARNING("Jittery wakeup! Magnitude: %ld ns (cycle #%lu)\n",
                (long)tmpts, (unsigned long)cycle);
        if (tmpts > 0) {
          /* fudge the task cycle period to avoid lockups? */
          clock_gettime(CLOCK_REALTIME, &next_task_wakeup);
        }
      }
    }

    timespec_add_ns(&next_task_wakeup, (long)task_period_ns);


    /* Grab time */
    rs.current_ts = cycleT0 - rs.init_ts;

#ifdef DEBUG_CYCLE_TIME
    do {
      static long long last = 0;
      tmpts = gethrtime();
      if (last)  LOG_MSG("%d\n", (int)ts-last);
      last = ts;
    } while(0);
#endif
    
    if (lastTriggers && triggersExpired()) 
      /* Clears the results of the last 'trigger' output done, but only
         when the trigger 'expires' which means it has been 'sustained' in 
         the on position for trigger_ms milliseconds. */
      clearTriggerLines();
    
      /* In DIO mode, grab all available DIO bits at once (up to 32 bits due 
         to limitations in COMEDI).  Otherwise, grab AI channels. */
    switch(IN_CHAN_TYPE) {
    case DIO_TYPE:   grabAllDIO(); break;
    case AI_TYPE:        grabAI(); break;      
    }
    
    handleFifos();
    
    if ( rs.valid ) {

      unsigned long events_bits = 0;
      int got_timeout = 0, n_evt_loops;
      DECLARE_STATE_PTR(state);
      GET_STATE(rs.states, state, rs.current_state);
      
      
      if ( rs.forced_times_up ) {
	
        /* Ok, it was a forced timeout.. */
        if (state->timeout_us != 0) got_timeout = 1;
        rs.forced_times_up = 0;
	
      } 

      if (rs.forced_event > -1) {
	
        /* Ok, it was a forced input transition.. indicate this event in our bitfield array */
        events_bits |= 0x1 << rs.forced_event;
        rs.forced_event = -1;
        
      } 
      
      /* If we aren't paused.. detect timeout events and input events */
      if ( !rs.paused  ) {
                
        /* Check for state timeout -- 
           IF Curent state *has* a timeout (!=0) AND the timeout expired */
        if ( !got_timeout && state->timeout_us != 0 && TIMER_EXPIRED(state->timeout_us) )   {
          
          got_timeout = 1;
          
          if (debug) {
            char buf[22];
            strncpy(buf, uint64_to_cstr(rs.current_ts), 21);
            buf[21] = 0;
            DEBUG("timer expired in state %u t_us: %u timer: %s ts: %s\n", rs.current_state, state->timeout_us, uint64_to_cstr(rs.current_timer_start), buf);
          }

        }
        
        /* Normal event transition code -- detectInputEvents() returns
           a bitfield array of all the input events we have right now.
           Note how we can have multiple ones, and they all get
           acknowledged by the loop later in this function!

           Note we |= this because we may have forced some event to be set 
           as part of the forced_event stuff in this function above.  */
        events_bits |= detectInputEvents();   

        if (debug >= 2) {
          DEBUG("Got input events mask %08lx\n", events_bits);
        }

        /* Process the scheduled waves by checking if any of their components
           expired.  For scheduled waves that result in an input event
           on edge-up/edge-down, they will return those event ids
           as a bitfield array.  */
        events_bits |= processSchedWaves();

        if (debug >= 2) {
          DEBUG("After processSchedWaves(), got input events mask %08lx\n", events_bits);
        }

        /* Process the scheduled AO waves -- do analog output for
           samples this cycle.  If there's an event id for the samples
           outputted, will get a mask of event ids.  */
        events_bits |= processSchedWavesAO();

        if (debug >= 2) {
          DEBUG("After processSchedWavesAO(), got input events mask %08lx\n", events_bits);
        }
	
      }
      
      if (got_timeout) 
        /* Timeout expired, transistion to timeout_state.. */
        gotoState(state->timeout_state, -1);
      
 
      /* Normal event transition code, keep popping ones off our 
         bitfield array, events_bits. */
      for (n_evt_loops = 0; events_bits && n_evt_loops < (int)NUM_INPUT_EVENTS; ++n_evt_loops) {
        unsigned event_id; 
        
        /* use asm/bitops.h __ffs to find the first set bit */
        event_id = __ffs(events_bits);
        /* now clear or 'pop off' the bit.. */
        events_bits &= ~(0x1UL << event_id); 
        
        dispatchEvent(event_id);
      }

      if (NUM_INPUT_EVENTS && n_evt_loops >= (int)NUM_INPUT_EVENTS && events_bits) {
        ERROR_INT("Event detection code in doFSM() tried to loop more than %d times!  DEBUG ME!\n", n_evt_loops);
      }

    }

    commitDataWrites();   
    
    cycleTf = gethrtime();

    if ( cycleTf-cycleT0 + 1000LL > (long long)task_period_ns) {
      WARNING("Cycle %lu took %lu ns (task period is %lu ns)!\n", (unsigned long)cycle, ((unsigned long)(cycleTf-cycleT0)), (unsigned long)task_period_ns);
      ++fsm_cycle_long_ct;
      if ( (cycleTf - cycleT0) > (long long)task_period_ns ) {
        /* If it broke RT constraints, resynch next task wakeup to ensure
           we don't monopolize the CPU */
        clock_gettime(CLOCK_REALTIME, &next_task_wakeup);
        timespec_add_ns(&next_task_wakeup, (long)task_period_ns);
      }
    }
    
    /* do any necessary data acquisition and writing to shm->fifo_daq */
    doDAQ();

    /* Sleep until next period */    
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next_task_wakeup, 0);
  }

  rt_task_running = 0;
  pthread_exit(0);
  return 0;
}

static inline volatile struct StateTransition *historyTop(void)
{
  return &rs.history.transitions[rs.history.cur];
}

static inline void transitionNotifyUserspace(volatile struct StateTransition *transition)
{
  const unsigned long sz = sizeof(*transition);
  int err = rtf_put(shm->fifo_trans, (void *)transition, sz);  
  if (debug && err != (int)sz) {
    DEBUG("error writing to state transition fifo, got %d, expected %lu -- free space is %d\n", err, sz, RTF_FREE(shm->fifo_trans));
  }
}

static inline void historyPush(int event_id)
{
  volatile struct StateTransition * transition;

  /* increment current index.. wrap it around to 0 if the history is full */
  if (++rs.history.cur >= MAX_HISTORY)  rs.history.cur = 0;
  
  transition = historyTop();
  transition->previous_state = rs.previous_state;
  transition->state = rs.current_state;
  transition->ts = rs.current_ts;  
  transition->event_id = event_id;
  transitionNotifyUserspace(transition);
}

static int gotoState(unsigned state, int event_id)
{
  DEBUG("gotoState %u %d s: %u\n", state, event_id, rs.current_state);

  if (state >= NUM_ROWS) {
    ERROR_INT("state id %d is >= NUM_ROWS %d!\n", (int)state, (int)NUM_ROWS);
    return -1;
  }

  if (state == READY_FOR_TRIAL_JUMPSTATE && rs.ready_for_trial_flg) {
    /* Special case of "ready for trial" flag is set so we don't go to 
       state 35, but instead to 0 */    
    state = 0;
    stopActiveWaves(); /* since we are starting a new trial, force any
                          active timer waves to abort! */
  }

  if (state == 0) {
    /* Hack */
    rs.ready_for_trial_flg = 0;
  }

  rs.previous_state = rs.current_state;
  rs.current_state = state;
  historyPush(event_id); /* now add this transition to the history  */

  if (rs.current_state == rs.previous_state) 
  {
      /* Ok, the old state == the new state, that means we:

         1. *DO NOT* do any trigger outputs
         2. *DO NOT* do any continuous lines (keep the same as before)
         3. *DO NOT* reset the timer (unless it was a timeout event)!
         4. *DO* record this state transition. (already happened above..) */

      /* Ok, on timeout event reset the timer anyway.. */
      if (event_id < 0)  RESET_TIMER(); 

          
      return 0; /* No, was not a new state.. */

  } 
  else /* else, a new state .. */
  {    

      /* Ok, the old state != the new state, that means we:

         1. *DO* any triggers
         2. *DO* new continuous lines
         3. *DO* reset the timer!
         4. *DO* record this state transition. (already happened above..) */

      /* Reset the timer.. */
      RESET_TIMER();
      
      /* In new state, do output(s) (trig and cont).. */
      doOutput(); 
    
      return 1; /* Yes, was a new state. */
  }

  return 0; /* Not reached.. */
}

static void doOutput(void)
{
  unsigned i;
  DECLARE_STATE_PTR(state);
  unsigned trigs, conts; 
  
  GET_STATE(rs.states, state, rs.current_state);
  trigs = state->trigger_outputs<<FIRST_TRIG_CHAN;
  conts = state->continuous_outputs<<FIRST_CONT_CHAN;

  if ( trigs ) {
    /* Do trigger outputs */
    /* is calling clearTriggerLines here really necessary?? It can interfere
       with really fast state transitioning but 'oh well'.. */
    clearTriggerLines(); /* Just in case we got 2 events real fast and 
                            the trigger lines are still uncleared. */
    for (i = FIRST_TRIG_CHAN; i < FIRST_TRIG_CHAN+NUM_TRIG_CHANS && i < NUM_DIO_CHANS; ++i) 
      if ( 0x1<<i & trigs ) { 
        dataWrite(i, 1);  
        lastTriggers |= 0x1<<i; 
      }
    resetTriggerTimer();

    /* Do Lynx 'virtual' triggers... */
    CHK_AND_DO_LYNX_TRIG(state->trigger_outputs);

  }

  /* Do continuous outputs */
  for (i = FIRST_CONT_CHAN; i < FIRST_CONT_CHAN+NUM_CONT_CHANS && i < NUM_DIO_CHANS; ++i) 
    if ( 0x1<<i & conts )
      dataWrite(i, 1);
    else  
      dataWrite(i, 0); 

  {
    /* HACK!! Untriggering is funny since you need to do:
       -(2^wave1_id+2^wave2_id) in your FSM to untrigger! */

    int swBits = (int)state->sched_wave, op = 1;
    
    /* if it's negative, invert the bitpattern so we can identify
       the wave id in question, then use 'op' to supply a negative waveid to
       scheduleWave() */
    if (swBits < 0)  { op = -1; swBits = -swBits; }
                    
    /* Do scheduled wave outputs, if any */
    while (swBits) {
      int wave = __ffs(swBits);
      swBits &= ~(0x1<<wave);
      scheduleWave(wave, op);
    }
  }
}

static inline void clearTriggerLines(void)
{
  unsigned i;
  for (i = FIRST_TRIG_CHAN; i < FIRST_TRIG_CHAN+NUM_TRIG_CHANS; ++i)
    if ( (0x1 << i) & lastTriggers ) 
      dataWrite(i, 0);
  lastTriggers = 0;
}

static unsigned long detectInputEvents(void)
{
  unsigned i, bits, bits_prev;
  unsigned long events = 0;
  
  switch(IN_CHAN_TYPE) { 
  case DIO_TYPE: /* DIO input */
    bits = dio_bits;
    bits_prev = dio_bits_prev;
    break;
  case AI_TYPE: /* AI input */
    bits = ai_bits;
    bits_prev = ai_bits_prev;
    break;
  default:  
    /* Should not be reached, here to suppress warnings. */
    bits = bits_prev = 0; 
    break;
  }
  
  /* Loop through all our event channel id's comparing them to our DIO bits */
  for (i = FIRST_IN_CHAN; i < AFTER_LAST_IN_CHAN; ++i) {
    int bit = ((0x1 << i) & bits) != 0, 
        last_bit = ((0x1 << i) & bits_prev) != 0,
        event_id_edge_up = INPUT_ROUTING(i*2), /* Even numbered input event 
                                                  id's are edge-up events.. */
        event_id_edge_down = INPUT_ROUTING(i*2+1); /* Odd numbered ones are 
                                                      edge-down */

    /* Edge-up transitions */ 
    if (event_id_edge_up > -1 && bit && !last_bit) /* before we were below, now we are above,  therefore yes, it is an blah-IN */
      events |= 0x1 << event_id_edge_up; 
    
    /* Edge-down transitions */ 
    if (event_id_edge_down > -1 /* input event is actually routed somewhere */
        && last_bit /* Last time we were above */
        && !bit ) /* Now we are below, therefore yes, it is event*/
      events |= 0x1 << event_id_edge_down; /* Return the event id */		   
  }
  return events; 
}


static void dispatchEvent(unsigned event_id)
{
  unsigned next_state = 0;
  DECLARE_STATE_PTR(state);
  GET_STATE(rs.states, state, rs.current_state);
  if (event_id > NUM_INPUT_EVENTS) {
    ERROR_INT("event id %d is > NUM_INPUT_EVENTS %d!\n", (int)event_id, (int)NUM_INPUT_EVENTS);
    return;
  }
  next_state = (event_id == NUM_INPUT_EVENTS) ? state->timeout_state : state->input[event_id]; 
  gotoState(next_state, event_id);
}

/* Set everything to zero to start fresh */
static void clearAllOutputLines(void)
{
  uint i;

  for (i = FIRST_OUT_CHAN; i <= LAST_OUT_CHAN; ++i) dataWrite(i, 0);
}

static void handleFifos(void)
{
  static int buddyTaskCmd = 0;
# define BUDDY_TASK_BUSY (buddyTaskCmd > 0 ? buddyTaskCmd : 0)
# define BUDDY_TASK_DONE (buddyTaskCmd < 0 ? -buddyTaskCmd : 0)
# define BUDDY_TASK_CLEAR (buddyTaskCmd = 0)
# define BUDDY_TASK_PEND(arg) \
  do { \
    buddyTaskCmd = arg; \
    softTaskPend(buddyTask, (void *)&buddyTaskCmd); \
  } while(0)

  FifoNotify_t dummy = 1;
  int errcode;
  int do_reply = 0;

  if (BUDDY_TASK_BUSY) {

    /* while our buddy task is processing,  do *NOT* handle any fifos! */
    return; 

  } else if (BUDDY_TASK_DONE) {
    /* our buddy task just finished its processing, now do:
       1. Any RT processing that needs to be done
       2. Indicate we should reply to user fifo by setting do_reply */

    switch(BUDDY_TASK_DONE) {
    case FSM:
      if (doSanityChecksRuntime()) {

          /* uh-oh.. it's a bad FSM?  Reject it.. */
          initRunState();
          reconfigureDIO();

        } else {

          /*LOG_MSG("Cycle: %lu  Got new FSM\n", (unsigned long)cycle);*/
          rs.states = OTHER_FSM_PTR(); /* now the temporary 'swap' fsm 
                                          that the non-realtime buddytask
                                          wrote to becomes permanent by
                                          swapping the rs.states pointer. */
          updateHasSchedWaves(); /* just updates rs.states->has_sched_waves flag*/
          reconfigureDIO(); /* to have new routing take effect.. */
          rs.valid = 1; /* Unlock FSM.. */

        } 
      do_reply = 1;
      break;

    case GETFSM:
      do_reply = 1;
      break;

    case RESET:
      /* these may have had race conditions with non-rt, so set them again.. */
      rs.current_ts = 0;
      RESET_TIMER();
      reconfigureDIO(); /* to reset DIO config since our routing spec 
                           changed */
      do_reply = 1;
      break;

    case AOWAVE:
      /* need up upadte rs.states->has_sched_waves flag as that affects 
       * whether we check the last column of the FSM for sched wave triggers. */
      updateHasSchedWaves();
      /* we just finished processing/allocating an AO wave, reply to 
         userspace now */
      do_reply = 1;
      break;

    default:
      ERROR_INT(" Got bogus reply %d from non-RT buddy task!\n", BUDDY_TASK_DONE);
      break;
    }

    BUDDY_TASK_CLEAR; /* clear pending result */

  } else {     /* !BUDDY_TASK_BUSY */

    /* See if a message is ready, and if so, take it from the SHM */
    struct ShmMsg *msg = (struct ShmMsg *)&shm->msg;

    if (!RTF_EMPTY(shm->fifo_in)) 
      errcode = rtf_get(shm->fifo_in, &dummy, sizeof(dummy));
    else 
      /*  no data available, that's ok */
      return;

    if (errcode != sizeof(dummy)) {
      static int once_only = 0;
      if (!once_only)
        ERROR_INT("got return value of (%d) when reading from fifo_in %d\n", 
                  errcode, shm->fifo_in), once_only++;
      return;
    }
      
    switch (msg->id) {
        
    case RESET:
      /* Just to make sure we start off fresh ... */
      clearAllOutputLines();
      stopActiveWaves();
      rs.valid = 0; /* lock fsm so buddy task can safely manipulate it */
      
      BUDDY_TASK_PEND(RESET); /* slow operation because it clears FSM blob,
                                 pend it to non-RT buddy task. */
      reconfigureDIO(); /* to reset DIO config since our routing spec 
                           changed */
      break;
        
    case TRANSITIONCOUNT:
      msg->u.transition_count = NUM_TRANSITIONS();
      do_reply = 1;
      break;
      
    case TRANSITIONS:
      {
        unsigned *from = &msg->u.transitions.from; /* Shorthand alias.. */
        unsigned *num = &msg->u.transitions.num; /* alias.. */
        if ( *from > rs.history.cur) 
          *from = rs.history.cur;
        if (*num + *from > rs.history.cur)
          *num = rs.history.cur - *from + 1;
        
        if (*num > MSG_MAX_TRANSITIONS)
          *num = MSG_MAX_TRANSITIONS;
        
        memcpy(msg->u.transitions.transitions, 
               (void *) ( rs.history.transitions + *from ), 
               *num * sizeof(struct StateTransition));
        do_reply = 1;
      }
      break;
      
      
    case PAUSEUNPAUSE:
      rs.paused = !rs.paused;
      /* We reset the timer, just in case the new FSM's rs.current_state 
         had a timeout defined. */
      RESET_TIMER();
      /* Notice fall through to next case.. */
    case GETPAUSE:
      msg->u.is_paused = rs.paused;
      do_reply = 1;
      break;
      
    case INVALIDATE:
      rs.valid = 0;
      /* Notice fall through.. */
    case GETVALID:
      msg->u.is_valid = rs.valid;
      do_reply = 1;
      break;
      
    case FSM:
      /* rs.valid = 0; / * don't lock fsm since it will cause dropped input/output events! instead we have the buddy task write to the alternate fsm which we swap in as current when the buddy task completes (see beginning of this function) */
      
      DEBUG("handleFifos new FSM at cycle %s currentstate: %u\n", 
            uint64_to_cstr(cycle), 
            rs.current_state);

      /*  NB: don't do this because ITI states might need these! */
      /*clearAllOutputLines();
        stopActiveWaves(); */
      
      BUDDY_TASK_PEND(FSM);      /* Since this is a *slow* operation,
                                    let's defer processing to non-RT
                                    buddy task. */
      break;
      
    case GETFSM:

      BUDDY_TASK_PEND(GETFSM); /* Another slow operation.. */

      break;
      
    case GETFSMSIZE:
      if (rs.valid) {
        
        rs.valid = 0; /* lock fsm? */       
        
        msg->u.fsm_size[0] = rs.states->n_rows;
        msg->u.fsm_size[1] = rs.states->n_cols;
        
        rs.valid = 1; /* Unlock FSM.. */
        
      } else {
        
        memset((void *)msg->u.fsm_size, 0, sizeof(msg->u.fsm_size));
        
      }
      
      do_reply = 1;
      
      break;        
    case GETNUMINPUTEVENTS:
      if (rs.valid) {
        rs.valid = 0; /* lock fsm? */
        msg->u.num_input_events = NUM_INPUT_EVENTS;
        rs.valid = 1; /* unlock fsm */
      } else {
        memset((void *)msg->u.num_input_events, 0, sizeof(msg->u.num_input_events));        
      }
      do_reply = 1;
      break;
    case FORCEEVENT:
        if (msg->u.forced_event < NUM_INPUT_EVENTS)
          rs.forced_event = msg->u.forced_event;
        do_reply = 1;
        break;
        
    case FORCETIMESUP:
        rs.forced_times_up = 1;
        do_reply = 1;
        break;

    case FORCETRIGGER:
      {
          unsigned  chan;

          for (chan = FIRST_TRIG_CHAN; chan < FIRST_TRIG_CHAN+NUM_TRIG_CHANS; ++chan) {
            if ( (0x1 << chan) & (msg->u.forced_triggers<<FIRST_TRIG_CHAN) ) {
              lastTriggers |= 0x1 << chan;
              /* Do trigger immediately... */
              dataWrite(chan, 1);
            }
          }
          
          /* Check for 'virtual' triggers that get put into lynxVirtShm.. 
             and do them if they match */
          CHK_AND_DO_LYNX_TRIG(msg->u.forced_triggers);
      }
      do_reply = 1;
      break;
        
    case FORCEOUTPUT:
      {
          unsigned  chan;
          unsigned forced_mask = 0;
          for (chan = FIRST_CONT_CHAN; chan < FIRST_CONT_CHAN+NUM_CONT_CHANS; ++chan) {
            forced_mask |= (0x1 << chan) & (msg->u.forced_outputs<<FIRST_CONT_CHAN);
            if ( (0x1<<chan) & rs.forced_outputs_mask  && ! ( (0x1<<chan) & forced_mask) )
              /* Special case, clear forced outputs that were just turned off.. */
              dataWrite(chan, 0);
          }
          rs.forced_outputs_mask = forced_mask;

      }
      do_reply = 1;
      break;

      case GETRUNTIME:
        shm->msg.u.runtime_us = Nano2USec(rs.current_ts);
        do_reply = 1;
        break;

      case READYFORTRIAL:
        if (rs.current_state == READY_FOR_TRIAL_JUMPSTATE) {
          rs.ready_for_trial_flg = 0;
          gotoState(0, -1);
        } else
          rs.ready_for_trial_flg = 1;
        do_reply = 1;
        break;

      case GETCURRENTSTATE:
        shm->msg.u.current_state = rs.current_state;
        do_reply = 1;
        break;

      case FORCESTATE:
        if ( gotoState(shm->msg.u.forced_state, -1) < 0 )
          shm->msg.u.forced_state = -1; /* Indicate error.. */
        do_reply = 1;
        break;
	
      case STARTDAQ:
        shm->msg.u.start_daq.range_min = ai_krange.min;        
        shm->msg.u.start_daq.range_max = ai_krange.min;
        shm->msg.u.start_daq.maxdata = maxdata_ai;
        {
          unsigned ch;
          daq_ai_chanmask = 0;
          daq_ai_nchans = 0;
          for (ch = 0; ch < NUM_AI_CHANS; ++ch)
            if (shm->msg.u.start_daq.chan_mask & (0x1<<ch)) {
              daq_ai_chanmask |= 0x1<<ch;
              ++daq_ai_nchans;
            }
        }
        shm->msg.u.start_daq.started_ok = daq_ai_chanmask;
        shm->msg.u.start_daq.chan_mask = daq_ai_chanmask;
        do_reply = 1;
        break;

      case STOPDAQ:
        daq_ai_nchans = daq_ai_chanmask = 0;
        do_reply = 1;
        break;
        
      case GETAOMAXDATA:
        shm->msg.u.ao_maxdata = maxdata_ao;
        do_reply = 1;
        break;

    case AOWAVE:
        
        BUDDY_TASK_PEND(AOWAVE); /* a slow operation -- it has to allocate
                                    memory, etc, so pend to a linux process
                                    context buddy task 
                                    (see buddyTaskHandler()) */

        break;

      default:
        ERROR("Got unknown msg id '%d' in handleFifos()!\n", 
              msg->id);
        do_reply = 0;
        break;
    }
  }

  if (do_reply) {
    errcode = rtf_put(shm->fifo_out, &dummy, sizeof(dummy));
    if (errcode != sizeof(dummy)) {
      static int once_only = 0;
      if (!once_only) 
        ERROR_INT("rtos_fifo_put to fifo_out %d returned %d!\n", 
                  shm->fifo_out, errcode), once_only++;
      return;
    }
  }

}

static unsigned pending_output_bits = 0, pending_output_mask = 0;

static inline void dataWrite(unsigned chan, unsigned bit)
{
  register unsigned bitpos = 0x1 << chan;
  pending_output_mask |= bitpos;
  if (bit) /* set bit.. */
    pending_output_bits |= bitpos;
  else /* clear bit.. */
    pending_output_bits &= ~bitpos;
}

static void commitDataWrites(void)
{
  hrtime_t dio_ts = 0, dio_te = 0;

  /* Override with the 'forced' bits. */
  pending_output_mask |= rs.forced_outputs_mask;
  pending_output_bits |= rs.forced_outputs_mask;

  if ( avoid_redundant_writes && (pending_output_bits & pending_output_mask) == (dio_bits & pending_output_mask) )
    /* Optimization, only do the writes if the bits we last saw disagree
       with the bits as we would like them */
    return;
 
  if (debug > 2)  dio_ts = gethrtime();
  comedi_dio_bitfield(dev, subdev, pending_output_mask, &pending_output_bits);
  if (debug > 2)  dio_te = gethrtime();
  
  if(debug > 2)
    DEBUG("WRITES: dio_out mask: %x bits: %x for event count %d cycle %s took %u ns\n", pending_output_mask, pending_output_bits, (int)rs.history.cur, uint64_to_cstr(cycle), (unsigned)(dio_te - dio_ts));

  pending_output_bits = 0;
  pending_output_mask = 0;
}

static void grabAllDIO(void)
{
  /* Remember previous bits */
  dio_bits_prev = dio_bits;
  /* Grab all the input channels at once */
  comedi_dio_bitfield(dev, subdev, 0, (unsigned int *)&dio_bits);

  /* Debugging comedi reads.. */
  if (dio_bits && (cycle % sampling_rate) == 0 && debug > 1)
    DEBUG("READS 0x%x\n", dio_bits);
}

static void grabAI(void)
{
  int i;
  /* Remember previous bits */
  ai_bits_prev = ai_bits;
  /* Grab all the AI input channels */
  for (i = (int)FIRST_IN_CHAN; i < (int)AFTER_LAST_IN_CHAN; ++i) {
    lsampl_t sample;

    if (AI_MODE == SYNCH_MODE) {
      /* Synchronous AI, so do the slow comedi_data_read() */
      int err = comedi_data_read(dev_ai, subdev_ai, i, ai_range, AREF_GROUND, &sample);
      if (err != 1) {
        WARNING("comedi_data_read returned %d on AI chan %d!\n", err, i);
        return;
      }
      ai_asynch_samples[i] = sample; /* save the sample for doDAQ() function.. */

      if (i == 0 && debug) putDebugFifo(sample); /* write AI 0 to debug fifo*/

    } else { /* AI_MODE == ASYNCH_MODE */
      /* Asynch IO, read samples from our ai_aynch_samples which was populated
         by our comediCallback() function. */
      sample = ai_asynch_samples[i];  
    }

    /* At this point, we don't care anymore about synch/asynch.. we just
       have a sample. 

       Next, we translate the sample into either a digital 1 or a digital 0.

       To do this, we have two threshold values, ai_thesh_hi and ai_thesh_low.

       The rule is: If we are above ai_thresh_hi, we are considered to have a 
                    digital '1', and if we are below ai_thresh_low, we consider
                    it a digital '0'.  Otherwise, we take the digital value of 
                    what we had the last scan.

       This avoids jittery 1/0/1/0 transitions.  Your thermostat works
       on the same principle!  :)
    */
    
    if (sample >= ai_thresh_hi) {
      /* It's above the ai_thresh_hi value, we think of it as a digital 1. */ 
      ai_bits |= 0x1<<i; /* set the bit, it's above our arbitrary thresh. */
    } else if (sample <= ai_thresh_low) {
      /* We just dropped below ai_thesh_low, so definitely clear the bit. */
      ai_bits &= ~(0x1<<i); /* clear the bit, it's below ai_thesh_low */
    } else {
      /* No change from last scan.
         This happens sometimes when we are between ai_thesh_low 
         and ai_thresh_high. */
    }
  } /* end for loop */
}

static void doDAQ(void)
{
  static unsigned short samps[MAX_AI_CHANS];
  unsigned mask = daq_ai_chanmask;
  unsigned ct = 0;
  while (mask) {
    unsigned ch = __ffs(mask);
    int have_chan = (ch >= FIRST_IN_CHAN && ch < AFTER_LAST_IN_CHAN);
    mask &= ~(0x1<<ch); /* clear bit */
    if (have_chan && IN_CHAN_TYPE == AI_TYPE) {
      /* we already have this channel's  sample from the grabAI() function */
      samps[ct] = ai_asynch_samples[ch];
      ++ct;
    } else if (ch < NUM_AI_CHANS) {
      /* we don't have this sample yet, so read it */
      lsampl_t samp;
      comedi_data_read(dev_ai, subdev_ai, ch, ai_range, AREF_GROUND, &samp);
      ai_asynch_samples[ch] = samp; /* 'cache' the sample.. */
      samps[ct] = samp;
      ++ct;
    }
  }
  if (ct) {
    struct DAQScan scan;
    scan.magic = DAQSCAN_MAGIC;
    scan.ts_nanos = rs.current_ts;
    scan.nsamps = ct;
    rtf_put(shm->fifo_daq, &scan, sizeof(scan));
    rtf_put(shm->fifo_daq, samps, sizeof(samps[0])*ct);
  }
}

static inline void nano2timespec(hrtime_t time, struct timespec *t)
{
  t->tv_sec = time / 1000000000ULL; 
  t->tv_nsec = time % 1000000000ULL;
}
static inline unsigned long Nano2Sec(unsigned long long nano)
{
  return nano/(unsigned long long)BILLION;
}

static inline unsigned long Nano2USec(unsigned long long nano)
{
  return nano/1000ULL;
}

static int uint64_to_cstr_r(char *buf, unsigned bufsz, uint64 num)
{
  static const uint64 ZEROULL = 0ULL;
  static const uint32 dividend = 10;
  int ct = 0, i;
  uint64 quotient = num;
  unsigned long remainder;
  char reversebuf[21];
  unsigned sz = 21;

  if (bufsz < sz) sz = bufsz;
  if (sz) buf[0] = 0;
  if (sz <= 1) return 0;

  /* convert to base 10... results will be reversed */
  do {
    remainder = quotient % dividend;
    quotient /= dividend;
    reversebuf[ct++] = remainder + '0';
  } while (quotient != ZEROULL && ct < (int)sz-1);

  /* now reverse the reversed string... */
  for (i = 0; i < ct && i < (int)sz-1; i++) 
    buf[i] = reversebuf[(ct-i)-1];
  
  /* add nul... */
  buf[ct] = 0;
  
  return ct;
}

static const char *uint64_to_cstr(uint64 num)
{
  static char buf[21];
  uint64_to_cstr_r(buf, sizeof(buf), num);
  return buf;
}

static inline long Micro2Sec(long long micro, unsigned long *remainder)
{
  int neg = micro < 0;
  unsigned long rem;
  if (neg) micro = -micro;
  long ans = micro / 1000000LL;
  rem = micro % 1000000LL;
  if (neg) ans = -ans;
  if (remainder) *remainder = rem;
  return ans;
}

static inline void UNSET_LYNX_TRIG(unsigned trig)
{
  DEBUG("Virtual Trigger %d unset\n", trig); 
}

static inline void SET_LYNX_TRIG(unsigned trig)
{
  DEBUG("Virtual Trigger %d set\n", trig); 
}

static inline void CHK_AND_DO_LYNX_TRIG(unsigned trig) 
{
  if (!NUM_VTRIGS) return;

  /* Grab only bits within the range VTRIG_OFFSET -> VTRIG_OFFSET+NUM_VTRIGS */
  trig = (trig & (((0x1<<(NUM_VTRIGS+VTRIG_OFFSET))-1)) )>>VTRIG_OFFSET;
  if (!trig) return;
  if (trig & (0x1<<(NUM_VTRIGS-1))) /* Is 'last' bit set? */ 
    /* Untrig if 'last' bit is set (take the 2s complement) */ 
    /*UNSET_LYNX_TRIG( trig & ~(0x1<<(NUM_VTRIGS-1)) ); */
      UNSET_LYNX_TRIG( (~trig + 1) & ((0x1<<NUM_VTRIGS)-1) );
    else 
      SET_LYNX_TRIG(trig); 
}

static inline void putDebugFifo(unsigned val)
{
  if (shm->fifo_debug >= 0) {
        /* DEBUGGING AI CHANNEL 0 .. put it to shm->fifo_debug.. */
        char line[64];
        int len;
        snprintf(line, sizeof(line), "%u\n", val);
        line[sizeof(line)-1] = 0;
        len = strlen(line);
        rtf_put(shm->fifo_debug, line, len);
  }
}

static unsigned long processSchedWaves(void)
{  
  unsigned wave_mask = rs.active_wave_mask;
  unsigned long wave_events = 0;

  while (wave_mask) {
    unsigned wave = __ffs(wave_mask);
    struct ActiveWave *w = &((struct RunState *)&rs)->active_wave[wave];
    wave_mask &= ~(0x1<<wave);

    if (w->edge_up_ts && w->edge_up_ts <= rs.current_ts) {
      /* Edge-up timer expired, set the wave high either virtually or
         physically or both. */
          int id = SW_INPUT_ROUTING(wave*2);
          if (id > -1 && id <= (int)NUM_IN_EVT_COLS) {
            wave_events |= 0x1 << id; /* mark the event as having occurred
                                         for the in-event of the matrix, if
                                         it's routed as an input event wave
                                         for edge-up transitions. */
          }
          id = SW_OUTPUT_ROUTING(wave);

          if (id > -1 && id >= (int)FIRST_OUT_CHAN && id <= (int)LAST_OUT_CHAN) 
            dataWrite(id, 1); /* if it's routed to do output, do the output. */

          w->edge_up_ts = 0; /* mark this component done */
    }
    if (w->edge_down_ts && w->edge_down_ts <= rs.current_ts) {
      /* Edge-down timer expired, set the wave high either virtually or
         physically or both. */
          int id = SW_INPUT_ROUTING(wave*2+1);
          if (id > -1 && id <= (int)NUM_IN_EVT_COLS) {
            wave_events |= 0x1 << id; /* mark the event as having occurred
                                         for the in-event of the matrix, if
                                         it's routed as an input event wave
                                         for edge-up transitions. */
          }
          id = SW_OUTPUT_ROUTING(wave);

          if (id > -1 && id >= (int)FIRST_OUT_CHAN && id <= (int)LAST_OUT_CHAN) 
            dataWrite(id, 0); /* if it's routed to do output, do the output. */

          w->edge_down_ts = 0; /* mark this wave component done */
    } 
    if (w->end_ts && w->end_ts <= rs.current_ts) {
          /* Refractory period ended and/or wave is deactivated */
          rs.active_wave_mask &= ~(0x1<<wave); /* deactivate the wave */
          w->end_ts = 0; /* mark this wave component done */
    }
  }
  return wave_events;
}

static unsigned long processSchedWavesAO(void)
{  
  unsigned wave_mask = rs.active_ao_wave_mask;
  unsigned long wave_events = 0;

  while (wave_mask) {
    unsigned wave = __ffs(wave_mask);
    volatile struct AOWaveINTERNAL *w = &rs.aowaves[wave];
    wave_mask &= ~(0x1<<wave);

    if (w->cur >= w->nsamples && w->loop) w->cur = 0;
    if (w->cur < w->nsamples) {
      int evt_col = w->evt_cols[w->cur];
      if (dev_ao && w->aoline < NUM_AO_CHANS) {
        lsampl_t samp = w->samples[w->cur];
        comedi_data_write(dev_ao, subdev_ao, w->aoline, ao_range, 0, samp);
      }
      if (evt_col > -1 && evt_col <= (int)NUM_IN_EVT_COLS)
        wave_events |= 0x1 << evt_col;
      w->cur++;
    } else { /* w->cur >= w->nsamples, so wave ended.. unschedule it. */
      scheduleWaveAO(wave, -1);
      rs.active_ao_wave_mask &= ~(1<<wave);
    }
  }
  return wave_events;
}

static void scheduleWave(unsigned wave_id, int op)
{
  if (wave_id >= FSM_MAX_SCHED_WAVES)
  {
      DEBUG("Got wave request %d for an otherwise invalid scheduled wave %u!\n", op, wave_id);
      return;
  }
  scheduleWaveDIO(wave_id, op);
  scheduleWaveAO(wave_id, op);
}

static void scheduleWaveDIO(unsigned wave_id, int op)
{
  struct ActiveWave *w;
  const struct SchedWave *s;

  if (wave_id >= FSM_MAX_SCHED_WAVES  /* it's an invalid id */
      || !rs.states->sched_waves[wave_id].enabled /* wave def is not valid */ )
    return; /* silently ignore invalid wave.. */
  
  if (op > 0) {
    /* start/trigger a scheduled wave */
    
    if ( rs.active_wave_mask & 0x1<<wave_id ) /*  wave is already running! */
    {
        DEBUG("Got wave start request %u for an already-active scheduled DIO wave!\n", wave_id);
        return;  
    }
    /* ugly casts below are to de-volatile-ify */
    w = &((struct RunState *)&rs)->active_wave[wave_id];
    s = &((struct RunState *)&rs)->states->sched_waves[wave_id];
    
    w->edge_up_ts = rs.current_ts + (int64)s->preamble_ms*MILLION;
    w->edge_down_ts = w->edge_up_ts + (int64)s->sustain_ms*MILLION;
    w->end_ts = w->edge_down_ts + (int64)s->refraction_ms*MILLION;  
    rs.active_wave_mask |= 0x1<<wave_id; /* set the bit, enable */

  } else {
    /* Prematurely stop/cancel an already-running scheduled wave.. */

    if (!(rs.active_wave_mask & 0x1<<wave_id) ) /* wave is not running! */
    {
        DEBUG("Got wave stop request %u for an already-inactive scheduled DIO wave!\n", wave_id);
        return;  
    }
    rs.active_wave_mask &= ~(0x1<<wave_id); /* clear the bit, disable */
  }
}

static void scheduleWaveAO(unsigned wave_id, int op)
{
  volatile struct AOWaveINTERNAL *w = 0;
  if (wave_id >= FSM_MAX_SCHED_WAVES  /* it's an invalid id */
      || !rs.aowaves[wave_id].nsamples /* wave def is not valid */ )
    return; /* silently ignore invalid wave.. */
  w = &rs.aowaves[wave_id];
  if (op > 0) {
    /* start/trigger a scheduled AO wave */    
    if ( rs.active_ao_wave_mask & 0x1<<wave_id ) /* wave is already running! */
    {
        DEBUG("Got wave start request %u for an already-active scheduled AO wave!\n", wave_id);
        return;  
    }
    w->cur = 0;
    rs.active_ao_wave_mask |= 0x1<<wave_id; /* set the bit, enable */

  } else {
    /* Prematurely stop/cancel an already-running scheduled wave.. */
    if (!(rs.active_ao_wave_mask & 0x1<<wave_id) ) /* wave is not running! */
    {
        DEBUG("Got wave stop request %u for an already-inactive scheduled AO wave!\n", wave_id);
        return;  
    }
    rs.active_ao_wave_mask &= ~(0x1<<wave_id); /* clear the bit, disable */
    if (dev_ao && w->nsamples && w->aoline < NUM_AO_CHANS)
      /* write 0 on wave stop */
      comedi_data_write(dev_ao, subdev_ao, w->aoline, ao_range, 0, 0);
    w->cur = 0;
  }
}

static void stopActiveWaves(void) /* called when FSM starts a new trial */
{  
  while (rs.active_wave_mask) { 
    unsigned wave = __ffs(rs.active_wave_mask);
    struct ActiveWave *w = &((struct RunState *)&rs)->active_wave[wave];
    rs.active_wave_mask &= ~(1<<wave); /* clear bit */
    if (w->edge_down_ts && w->edge_down_ts <= rs.current_ts) {
          /* The wave was in the middle of a high, so set the line back low
             (if there actually is a line). */
          int id = SW_OUTPUT_ROUTING(wave);

          if (id > -1 && id >= (int)FIRST_OUT_CHAN && id <= (int)LAST_OUT_CHAN) 
            dataWrite(id, 0); /* if it's routed to do output, set it low. */
    }
    memset(w, 0, sizeof(*w));  
  }
  while (rs.active_ao_wave_mask) {
    unsigned wave = __ffs(rs.active_ao_wave_mask);
    scheduleWaveAO(wave, -1); /* should clear bit in rs.active_ao_wave_mask */
    rs.active_ao_wave_mask &= ~(0x1<<wave); /* just in case */
  }
}

static void buddyTaskHandler(void *arg)
{
  int *req = (int *)arg;
  struct ShmMsg *msg = (struct ShmMsg *)&shm->msg;
  if (!req) return; /* happens sometimes in userspace fsm mode... why? */
  switch (*req) {
  case FSM:
    /* use alternate FSM as temporary space during this interruptible copy 
       realtime task will swap the pointers when it realizes the copy
       is done */
    memcpy(OTHER_FSM_PTR(), (void *)&msg->u.fsm, sizeof(*OTHER_FSM_PTR()));  
    cleanupAOWaves(); /* we have to free existing AO waves here because a new
			 FSM might not have a sched_waves column.. */
    break;
  case GETFSM:
    if (!rs.valid) {      
      memset((void *)&msg->u.fsm, 0, sizeof(msg->u.fsm));      
    } else {      
      memcpy((void *)&msg->u.fsm, FSM_PTR(), sizeof(*FSM_PTR()));      
    }
    break;
  case RESET:
    cleanupAOWaves(); /* frees any allocated AO waves.. */
    initRunState();
    break;
  case AOWAVE: {
      struct AOWave *w = &msg->u.aowave;
      if (w->id < FSM_MAX_SCHED_WAVES && w->aoline < NUM_AO_CHANS) {
        if (w->nsamples > AOWAVE_MAX_SAMPLES) w->nsamples = AOWAVE_MAX_SAMPLES;
        volatile struct AOWaveINTERNAL *wint = &rs.aowaves[w->id];
        cleanupAOWave(wint);
        if (w->nsamples) {
          int ssize = w->nsamples * sizeof(*wint->samples),
              esize = w->nsamples * sizeof(*wint->evt_cols);
          wint->samples = malloc(ssize);
          wint->evt_cols = malloc(esize);
          if (wint->samples && wint->evt_cols) {
            wint->aoline = w->aoline;
            wint->nsamples = w->nsamples;
            wint->loop = w->loop;
            wint->cur = 0;
            memcpy((void *)(wint->samples), w->samples, ssize);
            memcpy((void *)(wint->evt_cols), w->evt_cols, esize);
            DEBUG("AOWave: allocated %d bytes for AOWave %u\n", ssize+esize, w->id);
          } else {
            ERROR("In AOWAVE Buddy Task Handler: failed to allocate memory for an AO wave! Argh!!\n");
            cleanupAOWave(wint);
          }
        }
      }
    }
    break;
  }

  /* indicate to RT task that request is done.. */
  *req = -*req;
}

static void buddyTaskComediHandler(void *arg)
{
  int err;
  if (!arg) return;
  err = setupComediCmd();    
  if (err)
    ERROR("comediCallback: failed to restart acquisition after COMEDI_CB_OVERFLOW %ld: error: %d!\n", ai_n_overflows, err);
  else
    LOG_MSG("comediCallback: restarted acquisition after %ld overflows.\n", ai_n_overflows);
}

static inline long long timespec_to_nano(const struct timespec *ts)
{
  return ((long long)ts->tv_sec) * 1000000000LL + (long long)ts->tv_nsec;
}

static void cleanupAOWaves(void)
{
  unsigned i;
  for (i = 0; i < FSM_MAX_SCHED_WAVES; ++i) 
    cleanupAOWave(&rs.aowaves[i]);  
}

static void cleanupAOWave(volatile struct AOWaveINTERNAL *wint)
{
  int freed = 0, nsamps = 0;
  if (!wint) return;
  nsamps = wint->nsamples;
  wint->loop = wint->cur = wint->nsamples = 0; /* make fsm not use this? */
  if (wint->samples) free(wint->samples), wint->samples = 0, freed += nsamps*sizeof(*wint->samples);
  if (wint->evt_cols) free(wint->evt_cols), wint->evt_cols = 0, freed += nsamps*sizeof(*wint->evt_cols);
  if (freed) 
    DEBUG("AOWave: freed %d bytes for AOWave wave buffer %d\n", freed, wint - rs.aowaves);
}

static void updateHasSchedWaves(void)
{
	unsigned i;
	int yesno = 0;
	for (i = 0; !yesno && i < FSM_MAX_SCHED_WAVES; ++i) {
		yesno = rs.states->sched_waves[i].enabled 
			|| (rs.aowaves[i].nsamples && rs.aowaves[i].samples);  
	}
	rs.states->has_sched_waves = yesno;
}


static inline hrtime_t getNanosTSC(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);  
  return ((long long)ts.tv_sec)*1000000000 + (long long)ts.tv_nsec;
}

static inline hrtime_t getNanosRTC(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);  
  return ((long long)ts.tv_sec)*1000000000 + (long long)ts.tv_nsec;
}

static inline void timespec_add_ns(struct timespec *ts, long long ns)
{
  long secs = ns / 1000000000;
  ns %= 1000000000;
  ts->tv_nsec += ns;
  ts->tv_sec += secs;
  if (ts->tv_nsec < 0) {
    ts->tv_nsec = 1000000000 - ts->tv_nsec;
    --ts->tv_sec;
  } else if (ts->tv_nsec >= 1000000000) {
    ++ts->tv_sec;
    ts->tv_nsec %= 1000000000;
  } 
}

static int my_comedi_get_krange(comedi_t *dev, unsigned subdev, unsigned chan, unsigned range, comedi_krange *out)
{
  comedi_range *rng;
  rng = comedi_get_range(dev, subdev, chan, range);
  if (!rng) return -1;
  out->min = rng->min*1e6;
  out->max = rng->max*1e6;
  out->flags = rng->unit;
  return 0;
}



static int parseParms(int argc, char *argv[])
{
  while (argc--) {
    char *name = *argv, *value = *argv++;
    struct ModParm *mp = modParms;
    name = strsep(&value, "=");
    if (!strcmp(name, "help") || !strcmp(name, "--help") || !strcmp(name, "-h")) {
    err_help:
      mp = modParms; /* this is needed due to goto below.. */
      /* print help.. */
      printf("Usage: arg=value ...");
      while (mp->addr) {
        printf("Param Name:\t%s\nType:\t\t%s\nDescription:\t%s\n\n", mp->name, mp->type, mp->descr);
        ++mp;
      }
      return 0;
    }
    if (!value) continue;
    while (mp->addr) {
      if (!strcmp(name, mp->name)) {
        LOG_MSG("Got arg %s=%s\n", name, value);
        if (!strcmp(mp->type, "i")) {
          int *valp = (int *)mp->addr;
          *valp = atoi(value);
          break;
        } else if (!strcmp(mp->type, "s")) {
          char **valp = (char **)mp->addr;
          *valp = value;
          break;
        } else {
          ERROR_INT("Unknown modparm type: %s!\n", mp->type);
          return 0;
        }
      }
      ++mp;
    }
    if (!mp) {
      ERROR("Unknown arg: %s!\n", name);
      goto err_help;
    }
  }
  return 1;
}

static void sh(int sig) { (void)sig; }

int main(int argc, char *argv[]) 
{
  int ret;
  struct sched_param p = { .sched_priority = sched_get_priority_max(SCHED_FIFO) };
  if (geteuid()) 
    WARNING("This program works best if run as root so that it can set realtime scheduling and lock memory pages into RAM!\n");
  if (sched_setscheduler(0, SCHED_FIFO, &p)) 
    WARNING("sched_setscheduler(SCHED_FIFO) failed: %s\n", strerror(errno));  
  if ( mlockall(MCL_CURRENT|MCL_FUTURE) ) 
    WARNING("mlockall failed: %s\n", strerror(errno));
  
  signal(SIGINT, sh);
  if (!parseParms(argc, argv)) return 1;
  ret = init(); 
  if (!ret) { 
    pause(); 
    cleanup();
  } else {
    if (ret < 0) ret = -ret;
    ERROR("Could not initialize, error was: %s\n", strerror(ret));
  }
  return ret; 
}
