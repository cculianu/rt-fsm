/**
 * FSM for Experiment Control
 *
 * Calin A. Culianu <calin@ajvar.org>
 * License: GPL v2 or later.
 */

#include <linux/module.h> 
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <asm/semaphore.h> /* for synchronization primitives           */
#include <asm/bitops.h>    /* for set/clear bit                        */
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h> /* some memory copyage                       */
#include <linux/proc_fs.h>
#include <asm/div64.h>    /* for do_div 64-bit division macro          */
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/comedilib.h>
#include <linux/seq_file.h>
#include <linux/random.h>
#include <linux/kmod.h> /* for call_usermodehelper                     */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
#  error This sourcecode really requires Kernel 2.6.20 or newer!  Sorry!
#endif

#include "rtos_compat.h"

#if defined(RTLINUX) && !defined(RTAI)
#elif defined(RTAI) && !defined(RTLINUX) /* RTAI */
   spinlock_t FSM_GLOBAL_SPINLOCK = SPIN_LOCK_UNLOCKED;
   EXPORT_SYMBOL(FSM_GLOBAL_SPINLOCK);
#else
#  error Need to define exactly one of RTLINUX or RTAI to compile this file!
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)
/* 2.4 kernel lacks this function, so we will emulate it using the function
   it does have ffs(), which thinks of the first bit as bit 1, but we want the
   first bit to be bit 0. */
static __inline__ int __ffs(int x) { return ffs(x)-1; }
#endif

#include "FSM.h"
#include "FSMExternalTrig.h" /* for the external triggering -- for now it's the sound stuff.. */
#include "FSMExternalTime.h" /* for the external time shm stuff */
#include "softtask.h" /* for asynchronous process context kernel tasks! */
#include "float_vsnprintf.h"

#ifdef RTLINUX
#include "rt_math/kmath.h" /* For math library.. hmm */
#else /* RTAI */
/* XXX HACK FIXME */
#undef __attribute_pure__ /* prevent compiler warnings about redefinition */
#undef __attribute_used__
#undef __always_inline
#include <rtai_math.h>
#include "extra_mathfuncs.h" /**< RTAI's rtai_math is missing some stuff we need, so we reimplement it.*/
#endif

#define NO_DEFLATE /* avoid using deflate as we never actually call it in kernel */
#include "deflate_helper.h"
#include "Version.h" /* for VersionSTR and VersionNUM macros */
#undef NO_DEFLATE

#define MODULE_NAME KBUILD_MODNAME
#ifdef MODULE_LICENSE
  MODULE_LICENSE("GPL");
#endif
#define NO_EMBC_TYPEDEFS
#include "EmbC.h"

#define LOG_MSG(x...) rt_printk(KERN_INFO MODULE_NAME ": "x)
#define WARNING(x...) rt_printk(KERN_WARNING MODULE_NAME ": WARNING - " x)
#define ERROR(x...) rt_printk(KERN_ERR MODULE_NAME": ERROR - " x)
#define ERROR_INT(x... ) rt_printk(KERN_CRIT MODULE_NAME": INTERNAL ERROR - " x)
#define DEBUG(x... ) do { if (debug) rt_printk(KERN_DEBUG MODULE_NAME": DEBUG - " x); } while (0)
#define DEBUG_VERB(x... ) do { if (debug > 1) rt_printk(KERN_DEBUG MODULE_NAME": DEBUG VERB - " x); } while (0)
#define DEBUG_CRAZY(x...) do { if (debug > 2) rt_printk(KERN_DEBUG MODULE_NAME": DEBUG CRAZY - " x); } while (0)

#undef RESET /* make *sure* RTAI's stupid RESET define is gone! */

MODULE_AUTHOR("Calin A. Culianu <calin@ajvar.org>");
MODULE_DESCRIPTION(MODULE_NAME ": The core of the rtfsm project -- executes a finite state machine spec. in hard real-time.  Optionally, can execute arbitrary C code embedded in the FSM spec.");

int init(void);  /**< Initialize data structures and register callback */
void cleanup(void); /**< Cleanup.. */

/** Index into RunState rs array declared below.. */
typedef unsigned FSMID_t;

static int initRunStates(void);
static int initRunState(FSMID_t);
static int initShm(void);
static int initBuddyTask(void);
static int initFifos(void);
static int initTaskPeriod(void);
static int initRT(void);
static int initComedi(void);
static void reconfigureIO(void); /* helper that reconfigures DIO channels 
                                    for INPUT/OUTPUT whenever the state 
                                    matrix, etc changes, computes ai_chans_in_use_mask, di_chans_in_use_mask, etc */
static int initAISubdev(void); /* Helper for initComedi() */
static int initAOSubdev(void); /* Helper for initComedi() */
static int setupComediCmd(void);
static void restartAIAcquisition(long flags);
static void cleanupAOWaves(volatile struct FSMSpec *);
struct AOWaveINTERNAL;
static void cleanupAOWave(volatile struct AOWaveINTERNAL *, const char *name, int bufnum);

/* The callback called by rtos scheduler every task period... */
static void *doFSM (void *);

/* Called whenever the /proc/RealtimeFSM proc file is read */
static int myseq_show (struct seq_file *m, void *d);
static int myseq_open(struct inode *, struct file *);
static struct file_operations myproc_fops =
{
  open:    myseq_open,
  read:    seq_read,
  llseek:  seq_lseek,
  release: single_release
};

module_init(init);
module_exit(cleanup);


/*---------------------------------------------------------------------------- 
  Some private 'global' variables...
-----------------------------------------------------------------------------*/
static volatile Shm *shm = 0;
static volatile struct FSMExtTrigShm *extTrigShm = 0; /* For external triggering (sound, etc..) */
static volatile struct FSMExtTimeShm *extTimeShm = 0; /* for external time synch. */

#define JITTER_TOLERANCE_NS 60000
#define MAX_HISTORY 65536 /* The maximum number of state transitions we remember -- note that the struct StateTransition is currently 16 bytes so the memory we consume (in bytes) is this number times 16! */
#define DEFAULT_TASK_RATE 6000
#define DEFAULT_AI_SAMPLING_RATE 10000
#define DEFAULT_AI_SETTLING_TIME 4
#define DEFAULT_TRIGGER_MS 1
#define DEFAULT_AI "synch"
#define MAX_AI_CHANS (sizeof(unsigned)*8)
#define MAX_AO_CHANS MAX_AI_CHANS
#define MAX_DIO_CHANS MAX_AI_CHANS
#define MAX_DIO_SUBDEVS 4
#define MAX(a,b) ( (a) > (b) ? (a) : (b) )
#define MIN(a,b) ( (a) < (b) ? (a) : (b) )
static char COMEDI_DEVICE_FILE[] = "/dev/comediXXXXXXXXXXXXXXX";
int minordev = 0, minordev_ai = -1, minordev_ao = -1,
    task_rate = DEFAULT_TASK_RATE, 
    ai_sampling_rate = DEFAULT_AI_SAMPLING_RATE, 
    ai_settling_time = DEFAULT_AI_SETTLING_TIME,  /* in microsecs. */
    trigger_ms = DEFAULT_TRIGGER_MS,    
    debug = 0;
char *ai = DEFAULT_AI;

#ifndef STR
#define STR1(x) #x
#define STR(x) STR1(x)
#endif

module_param(minordev, int, 0444);
MODULE_PARM_DESC(minordev, "The minor number of the comedi device to use.");
module_param(minordev_ai, int, 0444);
MODULE_PARM_DESC(minordev_ai, "The minor number of the comedi device to use for AI.  -1 to probe for first AI subdevice (defaults to -1).");
module_param(minordev_ao, int, 0444);
MODULE_PARM_DESC(minordev_ao, "The minor number of the comedi device to use for AO.  -1 to probe for first AO subdevice (defaults to -1).");
module_param(task_rate, int, 0444);
MODULE_PARM_DESC(task_rate, "The task rate of the FSM.  Defaults to " STR(DEFAULT_TASK_RATE) ".");
module_param(ai_sampling_rate, int, 0444);
MODULE_PARM_DESC(ai_sampling_rate, "The sampling rate for asynch AI scans.  Note that this rate only takes effect when ai=asynch.  This is the rate at which to program the DAQ boad to do streaming AI.  Defaults to " STR(DEFAULT_AI_SAMPLING_RATE) ".");
module_param(ai_settling_time, int, 0444);
MODULE_PARM_DESC(ai_settling_time, "The amount of time, in microseconds, that it takes an AI channel to settle.  This is a function of the max sampling rate of the board. Defaults to " STR(DEFAULT_AI_SETTLING_TIME) ".");
module_param(trigger_ms, int, 0444);
MODULE_PARM_DESC(trigger_ms, "The amount of time, in milliseconds, to sustain trigger outputs.  Defaults to " STR(DEFAULT_TRIGGER_MS) ".");
module_param(debug, int, 0444);
MODULE_PARM_DESC(debug, "If true, print extra (cryptic) debugging output.  Defaults to 0.");
module_param(ai, charp, 0444);
MODULE_PARM_DESC(ai, "This can either be \"synch\" or \"asynch\" to determine whether we use asynch IO (comedi_cmd: faster, less compatible) or synch IO (comedi_data_read: slower, more compatible) when acquiring samples from analog channels.  Note that for asynch to work properly it needs a dedicated realtime interrupt.  Defaults to \""DEFAULT_AI"\".");

#define FIRST_IN_CHAN(f) (rs[(f)].states->routing.first_in_chan)
#define NUM_IN_CHANS(f) (rs[(f)].states->routing.num_in_chans)
#define OUTPUT_ROUTING(f,i) ((struct OutputSpec *)&rs[(f)].states->routing.output_routing[i])
#define NUM_OUT_COLS(f) (rs[(f)].states->routing.num_out_cols)
#define AFTER_LAST_IN_CHAN(f) (FIRST_IN_CHAN(f)+NUM_IN_CHANS(f))
#define NUM_AI_CHANS ((const unsigned)n_chans_ai_subdev)
#define NUM_AO_CHANS ((const unsigned)n_chans_ao_subdev)
#define NUM_DIO_CHANS ((const unsigned)n_chans_dio_total)
#define NUM_DIO_SUBDEVS ((const unsigned)n_dio_subdevs)
#define READY_FOR_TRIAL_JUMPSTATE(f) (rs[(f)].states->ready_for_trial_jumpstate)
#define NUM_INPUT_EVENTS(f)  (rs[(f)].states->routing.num_evt_cols)
#define NUM_IN_EVT_CHANS(f) (NUM_IN_CHANS(f))
#define NUM_IN_EVT_COLS(f) (rs[(f)].states->routing.num_evt_cols)
#define NUM_ROWS(f) (rs[(f)].states->n_rows)
#define NUM_COLS(f) (rs[(f)].states->n_cols)
#define TIMER_EXPIRED(f,state_timeout_us) ( (rs[(f)].current_ts - rs[(f)].current_timer_start) >= ((int64)(state_timeout_us))*1000LL )
#define RESET_TIMER(f) (rs[(f)].current_timer_start = rs[(f)].current_ts)
#define NUM_TRANSITIONS(f) (rs[(f)].history.num_transitions)
#define NUM_LOG_ITEMS(f) (rs[(f)].history.num_log_items)
#define IN_CHAN_TYPE(f) ((const unsigned)rs[(f)].states->routing.in_chan_type)
#define AI_THRESHOLD_VOLTS_HI ((const unsigned)4) /* Note: embedded-C might do its own thing here.. */
#define AI_THRESHOLD_VOLTS_LOW ((const unsigned)3)
enum { SYNCH_MODE = 0, ASYNCH_MODE, UNKNOWN_MODE };
#define AI_MODE ((const unsigned)ai_mode)
#define FSM_PTR(_f) ((struct FSMSpec *)(rs[(_f)].states))
#define OTHER_FSM_PTR(_f) ((struct FSMSpec *)(FSM_PTR(_f) == &rs[(_f)].states1 ? &rs[(_f)].states2 : &rs[(f)].states1))
#define EMBC_PTR(_f) ((struct EmbC *)(FSM_PTR(_f)->embc))
#define OTHER_EMBC_PTR(_f) ((struct EmbC *)(OTHER_FSM_PTR(_f)->embc))
#define EMBC_HAS_FUNC(_f, _func) ( EMBC_PTR(_f) && EMBC_PTR(_f)->_func )
#define CALL_EMBC(_f, _func) do { if (EMBC_HAS_FUNC(_f, _func)) EMBC_PTR(_f)->_func(); } while (0)
#define CALL_EMBC1(_f, _func, _p1) do { if (EMBC_HAS_FUNC(_f, _func)) EMBC_PTR(_f)->_func(_p1); } while (0)
#define CALL_EMBC_RET(_f, _func) ( EMBC_HAS_FUNC(_f, _func) ? EMBC_PTR(_f)->_func() : 0 )
#define CALL_EMBC_RET1(_f, _func, _p1) ( EMBC_HAS_FUNC(_f, _func) ? EMBC_PTR(_f)->_func(_p1) : 0 )
#define CALL_EMBC_RET2(_f, _func, _p1, _p2) ( EMBC_HAS_FUNC(_f, _func) ? EMBC_PTR(_f)->_func(_p1, _p2) : 0 )
#define EMBC_MATRIX_GET(_fsm, _r, _c) ( (_fsm)->embc && (_fsm)->embc->get_at ? (_fsm)->embc->get_at((_r), (_c)) : 0 )
#define FSM_MATRIX_GET(_fsm, _r, _c) ( (_fsm)->type == FSM_TYPE_EMBC \
                                    ? EMBC_MATRIX_GET(_fsm, (_r), (_c)) \
                                    : ( (_fsm)->type == FSM_TYPE_PLAIN \
                                        ? FSM_MATRIX_AT(_fsm, (_r), (_c)) \
                                        : 0  )  )
#define INPUT_ROUTING(_f,_x) (rs[(_f)].states->routing.input_routing[(_x)])
#define SW_INPUT_ROUTING(_f,_x) ( !rs[(_f)].states->has_sched_waves ? -1 : rs[(_f)].states->routing.sched_wave_input[(_x)] )
#define SW_DOUT_ROUTING(_f,_x) ( !rs[(_f)].states->has_sched_waves ? -1 : rs[(_f)].states->routing.sched_wave_dout[(_x)] )
#define SW_EXTOUT_ROUTING(_f,_x) ( !rs[(_f)].states->has_sched_waves ? 0 : rs[(_f)].states->routing.sched_wave_extout[(_x)] )
#define STATE_TIMEOUT_US(_fsm, _state) (FSM_MATRIX_GET((_fsm), (_state), TIMEOUT_TIME_COL(_fsm)))
#define STATE_TIMEOUT_STATE(_fsm, _state) (FSM_MATRIX_GET((_fsm), (_state), TIMEOUT_STATE_COL(_fsm)))
#define STATE_OUTPUT(_fsm, _state, _i) (FSM_MATRIX_GET((_fsm), (_state), (_i)+FIRST_OUT_COL(_fsm)))
#define STATE_COL(_fsm, _state, _col) (FSM_MATRIX_GET((_fsm), (_state), (_col)))
#define SAMPLE_TO_VOLTS(sample) (((double)(sample)/(double)maxdata_ai) * (ai_range_max_v - ai_range_min_v) + ai_range_min_v)
#define VOLTS_TO_SAMPLE_AO(v) ((lsampl_t)( (((double)(v))-ao_range_min_v)/(ao_range_max_v-ao_range_min_v) * (double)maxdata_ao ))
#define DEFAULT_EXT_TRIG_OBJ_NUM(f) (FSM_PTR((f))->default_ext_trig_obj_num)
/* Channel locking stuff -- locked chans are used by the scheduled waves to 
   prevent normal SM states from overwriting DIO lines that scheduled waves 
   are currently writing to -- this is a bit of a hack since ideally we want
   a real ownership pattern here, but for now, we'll just say that everything else
   that writes to DO lines uses dataWrite() (which checks the lock) and scheduled waves
   use dataWrite_NoLock().  The scheduled waves then have to make sure to clear
   their locked channels, if any. */
#define LOCK_DO_CHAN(chan) do { do_chans_locked_mask |= 1U<<chan; } while (0)
#define UNLOCK_DO_CHAN(chan) do { do_chans_locked_mask &= ~(1U<<chan); } while (0)
#define UNLOCK_DO_ALL() do { do_chans_locked_mask = 0; } while (0)
#define IS_DO_CHAN_LOCKED(chan) (do_chans_locked_mask&(1<<chan))

static struct proc_dir_entry *proc_ent = 0;
static atomic_t rt_task_stop = ATOMIC_INIT(0); /* Internal variable to stop the RT 
                                                  thread. */
static atomic_t rt_task_running = ATOMIC_INIT(0);

static struct SoftTask *buddyTask[NUM_STATE_MACHINES] = {0}, /* non-RT kernel-side process context buddy 'tasklet' */
                       *buddyTaskComedi = 0;
static pthread_t rt_task_handle;
static comedi_t *dev = 0, *dev_ai = 0, *dev_ao = 0;
static unsigned subdev[MAX_DIO_SUBDEVS] = {0}, subdev_ai = 0, subdev_ao = 0, n_chans_ai_subdev = 0, n_chans_dio[MAX_DIO_SUBDEVS] = {0}, n_chans_ao_subdev = 0, maxdata_ai = 0, maxdata_ao = 0, n_dio_subdevs = 0, n_chans_dio_total = 0;

static unsigned long fsm_cycle_long_ct = 0, fsm_wakeup_jittered_ct = 0, fsm_cycles_skipped = 0;
static unsigned long ai_n_overflows = 0;

/* Comedi CB stats */
static unsigned long cb_eos_skips = 0, cb_eos_skipped_scans = 0, cb_eos_num_scans = 0;
static int64 cb_eos_cyc_max = ~0ULL, cb_eos_cyc_min = 0x7fffffffffffffffLL, cb_eos_cyc_avg = 0, cb_eos_cyc_ct = 0;

/* Remembered state of all DIO channels.  Bitfield array is indexed
   by DIO channel-id. */
unsigned dio_bits = 0, dio_bits_prev = 0, ai_bits = 0, ai_bits_prev = 0;
lsampl_t ai_thresh_hi = 0, /* Threshold, above which we consider it 
                              a digital 1.  Note EmbC code might do its own custom thing
                              in which case this value is ignored.  It's just a default. */
         ai_thresh_low = 0; /* Below this we consider it a digital 0. */

sampl_t ai_samples[MAX_AI_CHANS]; /* comedi_command callback copies samples to here, or our grabAI synch function puts samples here */
double ai_samples_volts[MAX_AI_CHANS]; /* array of ai_samples above scaled to volts for embc functions, threshold detect, etc */
void *ai_asynch_buf = 0; /* pointer to driver's DMA circular buffer for AI. */
comedi_krange ai_krange, ao_krange; 
double ai_range_min_v, ai_range_max_v, ao_range_min_v, ao_range_max_v; /* scaled to volts */
unsigned long ai_asynch_buffer_size = 0; /* Size of driver's DMA circ. buf. */
unsigned ai_range = 0, ai_mode = UNKNOWN_MODE, ao_range = 0;
lsampl_t ao_0V_sample = 0; /* When we want to write 0V to AO, we write this. */
unsigned ai_chans_in_use_mask = 0, di_chans_in_use_mask = 0, do_chans_in_use_mask = 0, do_chans_locked_mask = 0, ai_chans_seen_this_scan_mask = 0; 
unsigned int lastTriggers; /* Remember the trigger lines -- these 
                              get shut to 0 after 1 cycle                */
uint64 cycle = 0; /* the current cycle */
uint64 trig_cycle[NUM_STATE_MACHINES] = {0}; /* The cycle at which a trigger occurred, useful for deciding when to clearing a trigger (since we want triggers to last trigger_ms) */
static char didInitRunStates = 0;
/* some latency and cycle time statistics */
int64 lat_min = 0x7fffffffffffffffLL, lat_max = ~0ULL, lat_avg = 0, lat_cur = 0, 
      cyc_min = 0x7fffffffffffffffLL, cyc_max = ~0ULL, cyc_avg = 0, cyc_cur = 0;
/* some AI/AO/DIO channel stats */
volatile unsigned long 
              ai_thresh_hi_ct[MAX_AI_CHANS],
              ai_thresh_lo_ct[MAX_AI_CHANS],
              ao_write_ct[MAX_AO_CHANS],
              di_thresh_hi_ct[MAX_DIO_CHANS],
              di_thresh_lo_ct[MAX_DIO_CHANS],
              do_write_hi_ct[MAX_DIO_CHANS],
              do_write_lo_ct[MAX_DIO_CHANS];
              
#define BILLION 1000000000
#define MILLION 1000000
uint64 task_period_ns = BILLION;
#define Sec2Nano(s) (s*BILLION)
static inline unsigned long long Nano2Sec(unsigned long long nano);
static inline unsigned long long Nano2USec(unsigned long long nano);
static inline long Micro2Sec(long long micro, unsigned long *remainder);
static inline long long timespec_to_nano(const struct timespec *);
static inline void UNSET_EXT_TRIG(unsigned which, unsigned trig);
static inline void SET_EXT_TRIG(unsigned which, unsigned trig);
static inline void doExtTrigOutput(unsigned which, unsigned trig);
/** Return nonzero on error */
static int initializeFSM(FSMID_t f, struct FSMSpec *fsmspec_ptr);
static void swapFSMs(FSMID_t);
/* calls rmmod, does cleanup of a particular fsm's embc ptr, etc*/
static void unloadDetachFSM(struct FSMSpec *fsm);

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
  unsigned num_transitions; /* Number of total transitions since RESET_ of state
                               machine.  
                               Index into array: num_transitions%MAX_HISTORY */
  /* The log item history is a circular history.  It is a series of 
     name/value pairs, that keeps growing until it loops around
     to the beginning.                                                       */
  struct VarLogItem log_items[MAX_HISTORY]; /* keeps track of variables
                                               logged by the embedded C code */
  unsigned num_log_items; /* keeps track of variables logged by embedded C */
};

struct RunState {
    /* TODO: verify this is smp-safe.                                        */
    
    /* FSM Specification */
    
    /* The actual state transition specifications -- 
       we have two of them since we swap them when a new FSM comes in from
       userspace to avoid dropping events during ITI. */
    struct FSMSpec    states1;
    struct FSMSpec    states2;
    struct FSMSpec   *states;

    /* End FSM Specification. */
    FSMID_t id; /* the ID of this RunState; links to embedded C's 'fsm' var */

    /* Bitset of the events that were detected for an fsm cycle.. cleared each task period */
    unsigned long events_bits[(0x1UL<<(sizeof(unsigned short)*8))/(8*sizeof(unsigned long))+1];

    /* inherited from ai_bits and/or dio_bits, but overwritten by
       optional embedded-C event detection code */
    unsigned long input_bits, input_bits_prev;
    
    unsigned current_state;
    int64 current_ts; /* Time elapsed, in ns, since init_ts */
    double current_ts_secs; /* current_ts above, normalized to seconds, used
                               by embedded C code */
    int64 ext_current_ts; /** Abs. time from external reference -- 
                              see FSMExtTimeShm and FSMExternalTime.h */
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

  /** Mimes of the scheduled waves that came in from the FSMSpec.  These 
      mimics contain times, relative to current_ts, at which each of the
      components of the wave are due to take place: from edge-up, to
      edge-down, to wave termination (after the refractory period). */     
  struct ActiveWave {
    int64 edge_up_ts;   /**< from: current_ts + SchedWave::preamble_us       */
    int64 edge_down_ts; /**< from: edge_up_ts + SchedWave::sustain_us        */
    int64 end_ts;       /**< from: edge_down_ts + SchedWave::refractory_us   */
  } active_wave[FSM_MAX_SCHED_WAVES];
  /** A quick mask of the currently active scheduled waves.  The set bits
      in this mask correspond to array indices of the active_wave
      array above. */
  unsigned active_wave_mask;
  /** A quick mask of the currently active AO waves.  The set bits
      in this mask correspond to array indices of the aowaves array outside 
      this struct. */
  unsigned active_ao_wave_mask; 

  /** Keep track, on a per-state-machine-basis the AI channels used for DAQ */
  unsigned daq_ai_nchans, daq_ai_chanmask;

  /** Keep track of trigger and cont chans per state machine */
  unsigned do_chans_trig_mask, do_chans_cont_mask;

  /** Keep track of the last IP out trig values to avoid sending dupe packets -- used by doOutput() */
  char last_ip_outs_is_valid[FSM_MAX_OUT_EVENTS];
  /** Keep track of the last IP out trig values to avoid sending dupe packets -- used by doOutput() */
  unsigned last_ip_outs[FSM_MAX_OUT_EVENTS];

  /** Keep track of the last state transition -- used by embedded C code, 
      which links to this variable. */
  struct EmbCTransition last_transition;
  
  unsigned pending_fsm_swap; /**< iff true, need to swap fsms on next state0
                                  crossing */

  /* This *always* should be at the end of this struct since we don't clear
     the whole thing in initRunState(), but rather clear bytes up until
     this point! */
    struct StateHistory history;             /* Our state history record.   */
};

volatile static struct RunState rs[NUM_STATE_MACHINES];

  /** fsmspec->aowaves points to this struct -- analog output wave data -- 
   *  Note: to avoid memory leaks make sure initRunState() is never called when
   *  we have active valid pointers here!  AOWaves should be cleaned
   *  up (freed) using cleanupAOWaves() whenever we unload an existing FSM.  cleanupAOWaves()
   *  needs to be called in Linux process context -- preferably in unloadDetachFSM().
   */
  struct AOWaveINTERNAL 
  {
    unsigned aoline, nsamples, loop, cur;
    unsigned short *samples;
    signed char *evt_cols;
  };

/*---------------------------------------------------------------------------
 Some helper functions
-----------------------------------------------------------------------------*/
static inline volatile struct StateTransition *historyAt(FSMID_t, unsigned);
static inline volatile struct StateTransition *historyTop(FSMID_t);
static inline void historyPush(FSMID_t, int event_id);
static inline volatile struct VarLogItem *logItemAt(FSMID_t, unsigned);
static inline volatile struct VarLogItem *logItemTop(FSMID_t);
static inline void logItemPush(FSMID_t f, const char *n, double v);

static int gotoState(FSMID_t, unsigned state_no, int event_id_for_history); /**< returns 1 if new state, 0 if was the same and no real transition ocurred, -1 on error */
static void detectInputEvents(FSMID_t); /**< sets the bitfield array rs[f].events_bits of all the events detected -- each bit corresponds to a state matrix "in event column" position, eg center in is bit 0, center out is bit 1, left-in is bit 2, etc */
static int doSanityChecksStartup(void); /**< Checks mod params are sane. */
static int doSanityChecksRuntime(FSMID_t); /**< Checks FSM input/output params */
static void doSetupThatNeededFloatingPoint(void);

static void do_dio_bitfield(unsigned, unsigned *);
static int  do_dio_config(unsigned chan, int mode);
static void doOutput(FSMID_t);
static void clearAllOutputLines(FSMID_t);
static inline void clearTriggerLines(FSMID_t);
static void dispatchEvent(FSMID_t, unsigned event_id);
static void handleFifos(FSMID_t);
static void dataWrite(unsigned chan, unsigned bit);
static void dataWrite_NoLocks(unsigned chan, unsigned bit);
static void commitDIOWrites(void);
static void grabAllDIO(void);
static void grabAI(void); /* AI version of above.. */
static int readAI(unsigned chan); /**< reads comedi, caches channel if need be, does converstion to volts, etc.  returns true on success or false on error */
static int writeAO(unsigned chan, lsampl_t samp); /**< writes samp to our AO subdevice.  This is a thin wrapper around comedi_data_write that also tallyies some stats.. returns true on success */
static int bypassDOut(unsigned f, unsigned mask);
static void doDAQ(void); /* does the data acquisition for remaining channels that grabAI didn't get.  See STARTDAQ fifo cmd. */
static void processSchedWaves(FSMID_t); /**< updates active wave state, does output, sets event id mask in rs[f].events_bits of any waves that generated input events */
static void processSchedWavesAO(FSMID_t); /**< updates active wave state, does output, sets event id mask  in rs[f].events_bits of any waves that generated input events */
static void scheduleWave(FSMID_t, unsigned wave_id, int op);
static void scheduleWaveDIO(FSMID_t, unsigned wave_id, int op);
static void scheduleWaveAO(FSMID_t, unsigned wave_id, int op);
static void stopActiveWaves(FSMID_t); /* called when FSM starts a new trial */
static void updateHasSchedWaves(FSMID_t);
static void updateDefaultExtObjNum(FSMID_t);

#ifndef RTAI
/* just like clock_gethrtime but instead it used timespecs */
static inline void clock_gettime(clockid_t clk, struct timespec *ts);
#endif
static inline void nano2timespec(hrtime_t time, struct timespec *t);
#ifndef RTAI
/* 64-bit division is not natively supported on 32-bit processors, so it must 
   be emulated by the compiler or in the kernel, by this function... */
static inline unsigned long long ulldiv(unsigned long long dividend, unsigned long divisor, unsigned long *remainder);
#endif
/* Just like above, but does modulus */
static inline unsigned long ullmod(unsigned long long dividend, unsigned long divisor);
static inline long long lldiv(long long ll, long ld, long *r);
/* this function is non-reentrant! */
static const char *uint64_to_cstr(uint64 in);
/* this function is reentrant! */
static int uint64_to_cstr_r(char *buf, unsigned bufsz, uint64 num);
/* this function is non-reentrant! */
static const char *int64_to_cstr(int64 in);
/* this function is reentrant! */
static int int64_to_cstr_r(char *buf, unsigned bufsz, int64 num);
static unsigned long transferCircBuffer(void *dest, const void *src, unsigned long offset, unsigned long bytes, unsigned long bufsize);
/* Place an unsigned int into the debug fifo.  This is currently used to
   debug AI 0 reads.  Only useful for development.. */
static inline void putDebugFifo(unsigned value);
static void printStats(void);
static void buddyTaskHandler(void *arg);
static void buddyTaskComediHandler(void *arg);
static void tallyJitterStats(hrtime_t real_wakeup, hrtime_t ideal_wakeup, hrtime_t task_period_ns);
static void tallyCbEosCycStats(hrtime_t cyc_time);
static void tallyCycleTime(hrtime_t t0, hrtime_t tf);
static char *cycleStr(void);
/*-----------------------------------------------------------------------------*/

/*-----------------------------------------------------------------------------
  EMBEDDED C Library functions 
-----------------------------------------------------------------------------*/
static double emblib_rand(void);
static double emblib_randNormalized(void);
static void emblib_logValue(unsigned, const char *, double);
static void emblib_logArray(unsigned, const char *, const double *, unsigned n);
static unsigned emblib_logGetSize(unsigned);
static const EmbCLogItem * emblib_logGetAt(unsigned, unsigned);
static const EmbCLogItem * emblib_logFind(unsigned, const char *, unsigned);
static double emblib_readAI(unsigned);
static int emblib_writeAO(unsigned,double);
static int emblib_readDIO(unsigned);
static int emblib_writeDIO(unsigned,unsigned);
static int emblib_bypassDOut(unsigned, unsigned);
static int emblib_gotoState(uint fsm, unsigned state, int eventid);
static int emblib_printf(const char *format, ...) __attribute__ ((format (printf, 1, 2)));
static int emblib_snprintf(char *buf, unsigned long sz, const char *format, ...) __attribute__ ((format (printf, 3, 4)));
static void emblib_sendPacket(unsigned fsm, int proto, const char *host, unsigned short port, const void *data, unsigned len);
static void emblib_trigSchedWave(unsigned fsm_id, unsigned sched_wave_bits);
static void emblib_trigExt(unsigned which, unsigned trig);
/*-----------------------------------------------------------------------------*/

int init (void)
{
  int retval = 0;
  
#ifdef RTAI
    rt_linux_use_fpu(1);
#endif

  if (    (retval = initTaskPeriod())
       || (retval = initShm())
       || (retval = initFifos())
       || (retval = initRunStates())
       || (retval = initBuddyTask())
       || (retval = initComedi())
       || (retval = initRT())
       || (retval = doSanityChecksStartup()) )
    {
      cleanup();  
      return retval > 0 ? -retval : retval;
    }  
  
  proc_ent = create_proc_entry(MODULE_NAME, S_IFREG|S_IRUGO, 0);
  if (proc_ent) {  /* if proc_ent is zero, we silently ignore... */
    proc_ent->owner = THIS_MODULE;
    proc_ent->uid = 0;
    proc_ent->proc_fops = &myproc_fops;
  }

  LOG_MSG("started successfully at %d Hz.\n", task_rate);
  
  return retval;
}

void cleanup (void)
{
  FSMID_t f;

  if (proc_ent)
    remove_proc_entry(MODULE_NAME, 0);

  if (atomic_read(&rt_task_running)) {
    atomic_set(&rt_task_stop, 1);
    pthread_join(rt_task_handle, 0);
  }

  for (f = 0; f < NUM_STATE_MACHINES; ++f) {
    if (buddyTask[f]) softTaskDestroy(buddyTask[f]);
    buddyTask[f] = 0;
  }
  if (buddyTaskComedi) softTaskDestroy(buddyTaskComedi);
  buddyTaskComedi = 0;

  if (dev) {
    unsigned sd;
    for (sd = 0; sd < NUM_DIO_SUBDEVS; ++sd)
        comedi_unlock(dev, subdev[sd]);
    comedi_close(dev);
    dev = 0;
  }

  if (dev_ai) {
    comedi_cancel(dev_ai, subdev_ai);
    comedi_unlock(dev_ai, subdev_ai);
    /* Cleanup any comedi_cmd */
    comedi_register_callback(dev_ai, subdev_ai, 0, 0, 0);
    comedi_close(dev_ai);
    dev_ai = 0;
  }

  if (dev_ao) {
    comedi_unlock(dev_ao, subdev_ao);
    comedi_close(dev_ao);
    dev_ao = 0;
  }

  if (shm)  { 
    for (f = 0; f < NUM_STATE_MACHINES; ++f) {
      if (shm->fifo_in[f] >= 0) rtf_destroy(shm->fifo_in[f]);
      if (shm->fifo_out[f] >= 0) rtf_destroy(shm->fifo_out[f]);
      if (shm->fifo_trans[f] >= 0) rtf_destroy(shm->fifo_trans[f]);
      if (shm->fifo_daq[f] >= 0) rtf_destroy(shm->fifo_daq[f]);
      if (shm->fifo_nrt_output[f] >= 0) rtf_destroy(shm->fifo_nrt_output[f]);
    }
    if (shm->fifo_debug >= 0) rtf_destroy(shm->fifo_debug);
    memset((void *)shm, 0, sizeof(*shm));
    mbuff_free(FSM_SHM_NAME, (void *)shm); 
    shm = 0; 
  }
  if (extTrigShm) {
    /* Kcount needs to be decremented so need to detach.. */
    mbuff_detach(FSM_EXT_TRIG_SHM_NAME, (void *)extTrigShm);
    extTrigShm = NULL;
  }
  if (extTimeShm) {
    /* Kcount needs to be decremented so need to detach.. */
    mbuff_detach(FSM_EXT_TIME_SHM_NAME, (void *)extTimeShm);
    extTimeShm = NULL;
  }

  /* clean up any kernel modules that contains FSMs that might be left.. */
  if (didInitRunStates) {
    for (f = 0; f < NUM_STATE_MACHINES; ++f) {
      unloadDetachFSM((struct FSMSpec *)&rs[f].states1);
      unloadDetachFSM((struct FSMSpec *)&rs[f].states2);
    }
    didInitRunStates = 0;
  }

#ifdef RTAI  
  /*stop_rt_timer(); NB: leave timer running just in case we have other threads in other modules.. */
#endif
  printStats();
}

static void printStats(void)
{
  if (debug && cb_eos_skips) 
    DEBUG("skipped %u times, %u scans\n", cb_eos_skips, cb_eos_skipped_scans);

  LOG_MSG("unloaded successfully after %s cycles.\n", cycleStr());
}

static int initBuddyTask(void)
{
  FSMID_t f;
  for (f = 0; f < NUM_STATE_MACHINES; ++f) {
    char namebuf[64];
    snprintf(namebuf, 64, MODULE_NAME" Buddy Task %d", (int)f);
    buddyTask[f] = softTaskCreate(buddyTaskHandler, namebuf);
    if (!buddyTask[f]) return -ENOMEM;
  }
  buddyTaskComedi = softTaskCreate(buddyTaskComediHandler, MODULE_NAME" Comedi Buddy Task");
  if (!buddyTaskComedi) return -ENOMEM;
  return 0;
}

static int initShm(void)
{
  FSMID_t f;

  shm = (volatile struct Shm *) mbuff_alloc(FSM_SHM_NAME, FSM_SHM_SIZE);
  if (! shm)  return -ENOMEM;
  
  memset((void *)shm, 0, sizeof(*shm));
  shm->magic = FSM_SHM_MAGIC;
  
  shm->fifo_debug = -1;
  
  for (f = 0; f < NUM_STATE_MACHINES; ++f)
    shm->fifo_nrt_output[f] = shm->fifo_daq[f] = shm->fifo_trans[f] = shm->fifo_out[f] = shm->fifo_in[f] = -1;

  extTrigShm = mbuff_attach(FSM_EXT_TRIG_SHM_NAME, FSM_EXT_TRIG_SHM_SIZE);
  if (!extTrigShm) {
    LOG_MSG("Could not attach to SHM %s.\n", FSM_EXT_TRIG_SHM_NAME);
    return -ENOMEM;
  } else if (!FSM_EXT_TRIG_SHM_IS_VALID(extTrigShm)) {
    WARNING("Attached to SHM %s, but it appears to be invalid currently (this could change later if a kernel module that uses this shm is loaded).\n", 
	    FSM_EXT_TRIG_SHM_NAME);
  }

  extTimeShm = mbuff_attach(FSM_EXT_TIME_SHM_NAME, FSM_EXT_TIME_SHM_SIZE);
  if (!extTimeShm) {
    LOG_MSG("Could not attach to SHM %s.\n", FSM_EXT_TIME_SHM_NAME);
    return -ENOMEM;    
  } else if (!FSM_EXT_TIME_SHM_IS_VALID(extTimeShm)) {
    WARNING("Attached to SHM %s, but it appears to be invalid currently (this could change later if a kernel module that uses this shm is loaded).\n", FSM_EXT_TIME_SHM_NAME);
  }
  return 0;
}

static int initFifos(void)
{
  int32 err;
  unsigned minor;
  FSMID_t f;

  /* Open up fifos here.. */
  
  for (f = 0; f < NUM_STATE_MACHINES; ++f) {
    err = rtf_find_free(&minor, FIFO_SZ);
    if (err < 0) return 1;
    shm->fifo_out[f] = minor;
    err = rtf_find_free(&minor, FIFO_SZ);
    if (err < 0) return 1;
    shm->fifo_in[f] = minor;
    err = rtf_find_free(&minor, FIFO_TRANS_SZ);
    if (err < 0) return 1;
    shm->fifo_trans[f] = minor;
    err = rtf_find_free(&minor, FIFO_DAQ_SZ);
    if (err < 0) return 1;
    shm->fifo_daq[f] = minor;
    err = rtf_find_free(&minor, FIFO_NRT_OUTPUT_SZ);
    if (err < 0) return 1;
    shm->fifo_nrt_output[f] = minor;
    
    DEBUG("FIFOS: For FSM %u in %d out %d trans %d daq %d nrt_out %d\n", f, shm->fifo_in[f], shm->fifo_out[f], shm->fifo_trans[f], shm->fifo_daq[f], shm->fifo_nrt_output[f]);
  }

  if (debug) {
    err = rtf_find_free(&minor, 3072); /* 3kb fifo enough? */  
    if (err < 0) return 1;
    shm->fifo_debug = minor;
    DEBUG("FIFOS: Debug %d\n", shm->fifo_debug);
  }

  return 0;  
}

static int initRunStates(void)
{
  FSMID_t f;
  int ret = 0;

  for (f = 0; !ret && f < NUM_STATE_MACHINES; ++f) {
    rs[f].states1.embc = rs[f].states2.embc = 0;
    ret |= initRunState(f);
  }
  didInitRunStates = 1;

  return ret;
}

static int initRunState(FSMID_t f)
{ 
  /* Save the embc pointer since memset clears it */
  struct EmbC *embc1 = rs[f].states1.embc, *embc2 = rs[f].states2.embc;
  /* Clear the runstate memory area.. note how we only clear the beginning
     and don't touch the state history since it is rather large! */
  memset((void *)&rs[f], 0, sizeof(rs[f]) - sizeof(struct StateHistory));

  rs[f].states = (struct FSMSpec *)&rs[f].states1;
  /* load back the saved embc ptr..*/
  rs[f].states1.embc = embc1;
  rs[f].states2.embc = embc2;
  /* Now, initialize the history.. */
  rs[f].history.num_transitions = 0; /* indicate no state history. */
  rs[f].history.num_log_items = 0; /* indicate no log items history. */

  /* clear first element of transitions array to be anal */
  memset((void *)&rs[f].history.transitions[0], 0, sizeof(rs[f].history.transitions[0]));
  /* clear first element of log_items array to be anal */
  memset((void *)&rs[f].history.log_items[0], 0, sizeof(rs[f].history.log_items[0]));
  
  /* Grab current time from gethrtime() which is really the pentium TSC-based 
     timer  on most systems. */
  rs[f].init_ts = gethrtime();

  RESET_TIMER(f);

  rs[f].forced_event = -1; /* Negative value here means not forced.. this needs
                              to always reset to negative.. */
  rs[f].paused = 1; /* By default the FSM is paused initially. */
  rs[f].valid = 0; /* Start out with an 'invalid' FSM since we expect it
                      to be populated later from userspace..               */
  rs[f].id = f;
  
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
      return -EINVAL/*comedi_errno()*/;
    }

    for (sd = -1, n_dio_subdevs = 0; n_dio_subdevs < MAX_DIO_SUBDEVS; ++n_dio_subdevs) {
        sd = comedi_find_subdevice_by_type(dev, COMEDI_SUBD_DIO, sd+1);    
        if (sd >= 0 
            && (n_chans = comedi_get_n_channels(dev, sd)) > 0
            && n_chans + n_chans_dio_total <= MAX_DIO_CHANS) {
            n_chans_dio[n_dio_subdevs] = n_chans;
            subdev[n_dio_subdevs] = sd;
            n_chans_dio_total += n_chans;
        } else
            break; /* so that n_dio_subdevs doesn't get incremented */
    }
    if (NUM_DIO_SUBDEVS == 0) {
      WARNING("No DIO subdevice found for %s.\n", COMEDI_DEVICE_FILE);
      comedi_close(dev);
      dev = 0;
      /*return -ENODEV;*/
    }

    if (dev) {
        for (sd = 0; sd < (int)NUM_DIO_SUBDEVS; ++sd)
            comedi_lock(dev, subdev[sd]);
    }
  }

  DEBUG("COMEDI: n_chans DIO /dev/comedi%d = %u in %u subdevs\n", minordev, NUM_DIO_CHANS, NUM_DIO_SUBDEVS);  

  reconfigureIO();

  /* Set up AI subdevice for synch/asynch acquisition in case we ever opt to 
     use AI for input. */
  ret = initAISubdev();
  if ( ret ) return ret;    

  /* Probe for or open the AO subdevice for analog output (sched waves to AO)*/
  ret = initAOSubdev();
  if ( ret ) return ret;

  return 0;
}

static void reconfigureIO(void)
{
  int i, reconf_ct = 0;
  static unsigned char old_modes[FSM_MAX_IN_CHANS];
  static char old_modes_init = 0;
  hrtime_t start;
  FSMID_t f;

  start = gethrtime();

  if (!old_modes_init) {
    for (i = 0; i < FSM_MAX_IN_CHANS; ++i)
      old_modes[i] = 0x6e;
    old_modes_init = 1;
  }

  ai_chans_in_use_mask = 0;
  di_chans_in_use_mask = 0;
  do_chans_in_use_mask = 0;

  for (f = 0; f < NUM_STATE_MACHINES; ++f) {
    rs[f].do_chans_cont_mask = 0;
    rs[f].do_chans_trig_mask = 0;
    /* indicate to doOutputs() that the last_ip_outs array is to be ignored
       until an output occurs on that ip_out column */
    memset((void *)&rs[f].last_ip_outs_is_valid, 0, sizeof(rs[f].last_ip_outs_is_valid));

    for (i = FIRST_IN_CHAN(f); i < AFTER_LAST_IN_CHAN(f); ++i)
      if (IN_CHAN_TYPE(f) == AI_TYPE)
        ai_chans_in_use_mask |= 0x1<<i;
      else
        di_chans_in_use_mask |= 0x1<<i;
    for (i = 0; i < NUM_OUT_COLS(f); ++i) {
      struct OutputSpec *spec = OUTPUT_ROUTING(f,i);
      switch (spec->type) {
      case OSPEC_DOUT:
        rs[f].do_chans_cont_mask |= ((0x1<<(spec->to+1 - spec->from))-1) << spec->from;        
        break;
      case OSPEC_TRIG:
        rs[f].do_chans_trig_mask |= ((0x1<<(spec->to+1 - spec->from))-1) << spec->from;
        break;
      }
    }
    /* now take into account sched waves channels since they also can use DOUT lines sometimes
       depending on their configuration.. */
    if (FSM_PTR(f)->has_sched_waves) {
        for (i = 0; i < FSM_MAX_SCHED_WAVES; ++i) {
            int ch = FSM_PTR(f)->routing.sched_wave_dout[i];
            if ( ch > -1 && FSM_PTR(f)->sched_waves[i].enabled ) /* does it have a  DOUT and is it enabled? */
                /* if so, earmark it as an active DOUT chan */
                do_chans_in_use_mask |= 0x1 << ch;
        }
    }
    do_chans_in_use_mask |= rs[f].do_chans_cont_mask|rs[f].do_chans_trig_mask;

  }

  DEBUG("ReconfigureIO masks: ai_chans_in_use_mask 0x%x di_chans_in_use_mask 0x%x do_chans_in_use_mask 0x%x\n", ai_chans_in_use_mask, di_chans_in_use_mask, do_chans_in_use_mask);

  /* Now, setup channel modes correctly */
  for (i = 0; i < NUM_DIO_CHANS; ++i) {
      unsigned char mode;
      unsigned char *old_mode;
      if ((0x1<<i) & di_chans_in_use_mask) mode =  COMEDI_INPUT;
      else if ((0x1<<i) & do_chans_in_use_mask) mode = COMEDI_OUTPUT;
      else continue;
      old_mode = (i < FSM_MAX_IN_CHANS) ? &old_modes[i] : 0;
      if (old_mode && *old_mode == mode) continue; /* don't redundantly configure.. */
      ++reconf_ct;
      if ( !dev || do_dio_config(i, mode) != 1 )
        WARNING("comedi_dio_config returned error for channel %u mode %d\n", i, (int)mode);
      
      DEBUG("COMEDI: comedi_dio_config %u %d\n", i, (int)mode);
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
      } else
        n_chans_ai_subdev = comedi_get_n_channels(dev_ai, s);
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

  /* set up the ranges we want.. 
     this loop tries to find which is the closest range to 0-5V */
  n = comedi_get_n_ranges(dev_ai, subdev_ai, 0);
  for (i = 0; i < n; ++i) {
    comedi_get_krange(dev_ai, subdev_ai, 0, i, &ai_krange);
    if (RF_UNIT(ai_krange.flags) == UNIT_volt /* If it's volts we're talking 
                                                 about */
        && minV < ai_krange.min && maxV > ai_krange.max /* And this one is 
                                                           narrower than the 
                                                           previous */
        && ai_krange.min <= 0 && ai_krange.max >= 5*MILLION) /* And it 
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
          long rem_dummy;
          tmpLL *= (long long)maxdata_ai;
          ai_thresh_hi =  lldiv(tmpLL, maxV - minV, &rem_dummy);
          tmpLL = AI_THRESHOLD_VOLTS_LOW * 1000000LL - (long long)minV;
          tmpLL *= (long long)maxdata_ai;
          ai_thresh_low = lldiv(tmpLL, maxV - minV, &rem_dummy);
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
  } else {
      /* range >= 0, so store ai_krange struct which we use later in SAMPLE_TO_VOLTS macro.. */
      comedi_get_krange(dev_ai, subdev_ai, 0, range, &ai_krange);
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
      } else
        n_chans_ao_subdev = comedi_get_n_channels(dev_ao, s);
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
    comedi_get_krange(dev_ao, subdev_ao, 0, i, &ao_krange);
    if (RF_UNIT(ao_krange.flags) == UNIT_volt /* If it's volts we're talking 
                                                 about */
        && minV < ao_krange.min && maxV > ao_krange.max /* And this one is 
                                                           narrower than the 
                                                           previous */
        && ao_krange.min <= 0 && ao_krange.max >= 5*MILLION) /* And it 
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
      DEBUG_VERB("transferCircBuffer: buffer wrapped around!\n");
      n = bufsize - offset;
    }
    if (!n) {
      offset = 0;
      continue;
    }
    memcpy(d + nread, s + offset, n);
    DEBUG_CRAZY("transferCircBuffer: copied %lu bytes!\n", n);
    bytes -= n;
    offset += n;
    nread += n;
  }  
  return offset;
}

static void restartAIAcquisition(long flags)
{
    comedi_cancel(dev_ai, subdev_ai);
    if (AI_MODE == ASYNCH_MODE)
        /* slow operation to restart the comedi command, so just pend it
           to non-rt buddy task which runs in process context and can take its 
           sweet-assed time.. */
        softTaskPend(buddyTaskComedi, (void *)flags);
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
    restartAIAcquisition(1);
    return 0;
  }
  if (mask & COMEDI_CB_BLOCK) {
        DEBUG_CRAZY("comediCallback: got COMEDI_CB_BLOCK at abs. time %s.\n", timeBuf);
  }
  if (mask & COMEDI_CB_EOS) {
    static hrtime_t timeLast = 0;
    hrtime_t timeNow = gethrtime();
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

    DEBUG_CRAZY("comediCallback: got COMEDI_CB_EOS at abs. time %s, with %d bytes (offset: %d) of data.\n", timeBuf, numBytes, offset);
    
    if (numScans > 1) {
      DEBUG_CRAZY("COMEDI_CB_EOS with more than one scan's data:  Expected %d, got %d.\n", oneScanBytes, numBytes);
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
    
    cb_eos_num_scans += numScans;
    if (timeLast) tallyCbEosCycStats(timeNow-timeLast);
    timeLast = timeNow;
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
      /*unsigned long flags;
      
      rt_critical(flags); / * Lock machine, disable interrupts.. to avoid
                              race conditions with RT since we write to 
                              ai_samples. */

      /* Read only the latest full scan available.. */
      lastScanOffset = 
        transferCircBuffer(ai_samples, /* dest */
                           buf,  /* src */
                           lastScanOffset, /* offset into src */
                           oneScanBytes, /* num */
                           ai_asynch_buffer_size); /* circ buf size */

      /* consume all full scans up to present... */
      comedi_mark_buffer_read(dev_ai, subdev_ai, numScans * oneScanBytes);
      /*rt_end_critical(flags); / * Unlock machine, reenable interrupts... */

      /* puts chan0 to the debug fifo iff in debug mode */
      putDebugFifo(ai_samples[0]); 
    }

  } /* end if COMEDI_CB_EOS */
  return 0;
}

/** This function sets up asynchronous streaming acquisition for AI
    input.  It is a pain to get working and doesn't work on all boards. */
static int setupComediCmd(void)
{
  comedi_cmd cmd;
  int err, i;
  unsigned int chanlist[MAX_AI_CHANS];

  /* Clear sample buffers.. */
  memset(ai_samples, 0, sizeof(ai_samples));

  /* First, clear old callback just in case */
  comedi_register_callback(dev_ai, subdev_ai, 0, 0, 0);
  
  /* Next, setup our callback func. */
  err = comedi_register_callback(dev_ai, subdev_ai,  COMEDI_CB_EOA|COMEDI_CB_ERROR|COMEDI_CB_OVERFLOW|COMEDI_CB_EOS/*|COMEDI_CB_BLOCK*/, comediCallback, 0);

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

  if (ai_sampling_rate < task_rate) {
    ai_sampling_rate = task_rate;
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
  /*cmd.data = ai_samples;
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
  DEBUG("Comedi Asynch buffer at 0x%p of size %d\n", ai_asynch_buf, ai_asynch_buffer_size);

  err = comedi_command(dev_ai, &cmd);
  if (err) {
    ERROR("Comedi command could not be started, comedi_command returned: %d!\n", err);
    return err;
  }

  if (cmd.scan_begin_arg != BILLION / ai_sampling_rate) {
    WARNING("Comedi Asynch IO rate requested was %d, got %lu!\n", BILLION / ai_sampling_rate, (unsigned long)cmd.scan_begin_arg);
  }

  LOG_MSG("Comedi Asynch IO started with period %u ns.\n", cmd.scan_begin_arg);
  
  return 0;
}

static int initTaskPeriod(void)
{
  unsigned long rem;

  /* setup task period.. */
  if (task_rate <= 0 || task_rate >= 20000) {
    LOG_MSG("FSM task rate of %d seems crazy, going to default of %d\n", task_rate, DEFAULT_TASK_RATE);
    task_rate = DEFAULT_TASK_RATE;
  }
  task_period_ns = ulldiv(BILLION, task_rate, &rem);

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

#ifdef RTAI /* gah.. tell RTAI to start the timer in oneshot mode.. */
  if (!rt_is_hard_timer_running()) {
    rt_set_oneshot_mode();
    start_rt_timer(0); /* 0 period means what? its ok for oneshot? */
  }
  /* Must mark linux as using FPU because of embedded C code startup */
  rt_linux_use_fpu(1);
#endif
  /* Setup pthread data, etc.. */
  pthread_attr_init(&attr);
#ifdef RTLINUX
  pthread_attr_setfp_np(&attr, 1); 
#else /* RTAI */
  /* NB:  on RTAI implicitly all pthreads use the FPU */
#endif
#ifdef USE_OWN_STACK
  error = pthread_attr_setstackaddr(&attr, TASK_STACK);
  if (error) {
    ERROR("pthread_attr_setstackaddr(): %d\n", error);
    return -error;
  }
  error = pthread_attr_setstacksize(&attr, TASK_STACKSIZE);  
  if (error) {
    ERROR("pthread_attr_setstacksize(): %d\n", error);
    return -error;
  }
#endif
  sched_param.sched_priority = sched_get_priority_max(SCHED_FIFO);
  error = pthread_attr_setschedparam(&attr, &sched_param);  
  if (error) {
    ERROR("pthread_attr_setschedparam(): %d\n", error);
    return -error;
  }
  error = pthread_create(&rt_task_handle, &attr, doFSM, 0);
  if (error) {
    ERROR("pthread_create(): %d\n", error);
    return -error;
  }
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
  /* Ergh.. this should be a compile-time thing.. */
  if (MAX_DIO_CHANS < sizeof(unsigned)*8) {
      ERROR("MAX_DIO_CHANS=%u is too small!  Please recompile and set it to at least the number of bits in an unsigned (%u)!\n", (unsigned)MAX_DIO_CHANS, (unsigned)sizeof(unsigned)*8);
  }
  return ret;
}

static int doSanityChecksRuntime(FSMID_t f)
{
  int ret = 0;
/*   int n_chans = n_chans_dio_subdev + NUM_AI_CHANS, ret = 0; */
/*   char buf[256]; */
/*   static int not_first_time[NUM_STATE_MACHINES] = {0}; */
  switch (FSM_PTR(f)->type) {
  case FSM_TYPE_EMBC:
    if (!FSM_PTR(f)->embc) return -EINVAL;
    break;
  case FSM_TYPE_PLAIN:
      /* no specific stuff for plain.. just jump to code outside the switch.. */
      break;
  case FSM_TYPE_NONE: /* reached on error from buddyTaskHandler()  */
      return -EINVAL;
  default: /* this is only set on error.. */
      ERROR_INT("Inconsistent/invalid FSMSpec::type field! ARGH!  FIXME!\n");
      return -EINVAL;
  }
  if (READY_FOR_TRIAL_JUMPSTATE(f) >= NUM_ROWS(f) || READY_FOR_TRIAL_JUMPSTATE(f) <= 0)  
    WARNING("ready_for_trial_jumpstate of %d need to be between 0 and %d!\n", 
            (int)READY_FOR_TRIAL_JUMPSTATE(f), (int)NUM_ROWS(f)); 
  if (rs[f].current_state >= NUM_ROWS(f) && !FSM_PTR(f)->wait_for_jump_to_state_0_to_swap_fsm)
    WARNING("specified a new state machine with %d states, but current_state is %d -- will force a jump to state 0!\n", (int)NUM_ROWS(f), (int)rs[f].current_state);
  
/*   if (dev && n_chans != NUM_CHANS(f) && debug) */
/*     WARNING("COMEDI devices have %d input channels which differs from the number of input channels indicated by the sum of FSM parameters num_in_chans+num_cont_chans+num_trig_chans=%d!\n", n_chans, NUM_CHANS(f)); */

/*   if (NUM_IN_CHANS < NUM_IN_EVT_CHANS)  */
/*     ERROR("num_in_chans of %d is less than hard-coded NUM_IN_EVT_CHANS of %d!\n", (int)NUM_IN_CHANS, (int)NUM_IN_EVT_CHANS), ret = -EINVAL; */

/*   if ( dev && n_chans < NUM_CHANS(f))  */
/*     ERROR("COMEDI device only has %d input channels, but FSM parameters require at least %d channels!\n", n_chans, NUM_CHANS(f)), ret = -EINVAL; */
  
  if (IN_CHAN_TYPE(f) == AI_TYPE && AFTER_LAST_IN_CHAN(f) > MAX_AI_CHANS) 
    ERROR("The input channels specified (%d-%d) exceed MAX_AI_CHANS (%d).\n", (int)FIRST_IN_CHAN(f), ((int)AFTER_LAST_IN_CHAN(f))-1, (int)MAX_AI_CHANS), ret = -EINVAL;
  if (ret) return ret;

/*   if (!not_first_time[f] || debug) { */
/*     LOG_MSG("FSM %u: Number of physical input channels is %d; Number actually used is %d; Virtual triggers start at 2^%d with %d total vtrigs (bit 2^%d is sound stop bit!)\n", f, n_chans, NUM_CHANS(f), VTRIG_OFFSET(f), NUM_VTRIGS(f), NUM_VTRIGS(f) ? VTRIG_OFFSET(f)+NUM_VTRIGS(f)-1 : 0); */

/*     snprintf(buf, sizeof(buf), "in chans: %d-%d ", FIRST_IN_CHAN(f), ((int)AFTER_LAST_IN_CHAN(f))-1); */
/*     if (NUM_CONT_CHANS(f)) */
/*       snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "cont chans: %d-%d ", FIRST_CONT_CHAN(f), FIRST_CONT_CHAN(f)+NUM_CONT_CHANS(f)-1); */
/*     if (NUM_TRIG_CHANS(f)) */
/*       snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "trig chans: %d-%d",FIRST_CONT_CHAN(f)+NUM_CONT_CHANS(f),FIRST_CONT_CHAN(f)+NUM_CONT_CHANS(f)+NUM_TRIG_CHANS(f)-1); */
/*     LOG_MSG("%s\n", buf); */
/*     not_first_time[f] = 1; */
/*   } */

  return 0;
}

static int myseq_open(struct inode *i, struct file *f)
{
  return single_open(f, myseq_show, 0);
}

static int myseq_show (struct seq_file *m, void *dummy)
{ 
  FSMID_t f;
  unsigned i, ct;
  char buf[13][22];
  int bufidx = 12;
#define BUFFMTS(s) ({ ++bufidx; bufidx %= 12; int64_to_cstr_r(buf[bufidx], sizeof(buf[bufidx]), s); buf[bufidx]; })
#define BUFFMTU(u) ({ ++bufidx; bufidx %= 12; uint64_to_cstr_r(buf[bufidx], sizeof(buf[bufidx]), u); buf[bufidx]; })
  
  (void)dummy;
  seq_printf(m, "%s Module\n\nmagic: %08x version: %s\n\n", MODULE_NAME, shm->magic, VersionSTR);
  seq_printf(m, 
             "Misc. Stats\n"
             "-----------\n"
             "Cycle count: %s  Task period (in ns): %s\n\n"
             "Real-time Correctness Stats\n"
             "---------------------------\n"
             "Cycles Overran: %lu  Cycles Skipped: %lu  Wokeup Late/Early: %lu\n"
             "Latency (in ns):  Cur %s  Avg %s  Min %s  Max %s\n"
             "Cycle cost (in ns):  Cur %s  Avg %s  Min %s  Max %s\n\n",
             BUFFMTU(cycle), BUFFMTU(task_period_ns), 
             fsm_cycle_long_ct, fsm_cycles_skipped, fsm_wakeup_jittered_ct, 
             BUFFMTS(lat_cur), BUFFMTS(lat_avg), BUFFMTS(lat_min), BUFFMTS(lat_max),
             BUFFMTS(cyc_cur), BUFFMTS(cyc_avg), BUFFMTS(cyc_min), BUFFMTS(cyc_max));

  seq_printf(m,
             "Data Read/Write Stats\n"
             "---------------------\n");
  seq_printf(m,
             "AI Threshold Crossing (+) Counts: ");   
  for (i = 0, ct = 0; i < NUM_AI_CHANS; ++i) {
      if (ai_thresh_hi_ct[i]) {
        if (!(ct%5)) seq_printf(m, "\n\t"); /* newline every 5th chan */
        seq_printf(m, "AI%02u: %04lu ", i, ai_thresh_hi_ct[i]);
        ++ct;
      }
  }
  if(ct) seq_printf(m, "\n");
  else seq_printf(m, "(None Yet..)\n");
  
  seq_printf(m,
             "AI Threshold Crossing (-) Counts: ");   
  for (i = 0, ct = 0; i < NUM_AI_CHANS; ++i) {
      if (ai_thresh_lo_ct[i]) {
        if (!(ct%5)) seq_printf(m, "\n\t"); /* newline every 5th chan */
        seq_printf(m, "AI%02u: %04lu ", i, ai_thresh_lo_ct[i]);
        ++ct;
      }
  }
  if(ct) seq_printf(m, "\n");
  else seq_printf(m, "(None Yet..)\n");
  
  seq_printf(m,
             "DI Threshold Crossing (+) Counts: ");   
  for (i = 0, ct = 0; i < NUM_DIO_CHANS; ++i) {
      if (di_thresh_hi_ct[i]) {
        if (!(ct%5)) seq_printf(m, "\n\t"); /* newline every 5th chan */
        seq_printf(m, "DI%02u: %04lu ", i, di_thresh_hi_ct[i]);
        ++ct;
      }
  }
  if(ct) seq_printf(m, "\n");
  else seq_printf(m, "(None Yet..)\n");
  
  seq_printf(m,
             "DI Threshold Crossing (-) Counts: ");   
  for (i = 0, ct = 0; i < NUM_DIO_CHANS; ++i) {
      if (di_thresh_lo_ct[i]) {
        if (!(ct%5)) seq_printf(m, "\n\t"); /* newline every 5th chan */
        seq_printf(m, "DI%02u: %04lu ", i, di_thresh_lo_ct[i]);
        ++ct;
      }
  }
  if(ct) seq_printf(m, "\n");
  else seq_printf(m, "(None Yet..)\n");
  
  seq_printf(m,
             "AO Write Counts: ");   
  for (i = 0, ct = 0; i < NUM_AO_CHANS; ++i) {
      if (ao_write_ct[i]) {
        if (!(ct%5)) seq_printf(m, "\n\t"); /* newline every 5th chan */
        seq_printf(m, "AO%02u: %04lu ", i, ao_write_ct[i]);
        ++ct;
      }
  }
  if(ct) seq_printf(m, "\n");
  else seq_printf(m, "(None Yet..)\n");
             
  seq_printf(m,
             "DO Write Logical 1 Counts: ");   
  for (i = 0, ct = 0; i < NUM_DIO_CHANS; ++i) {
      if (do_write_hi_ct[i]) {
        if (!(ct%5)) seq_printf(m, "\n\t"); /* newline every 5th chan */
        seq_printf(m, "DO%02u: %04lu ", i, do_write_hi_ct[i]);
        ++ct;
      }
  }
  if(ct) seq_printf(m, "\n");
  else seq_printf(m, "(None Yet..)\n");
  
  seq_printf(m,
             "DO Write Logical 0 Counts: ");   
  for (i = 0, ct = 0; i < NUM_DIO_CHANS; ++i) {
      if (do_write_lo_ct[i]) {
        if (!(ct%5)) seq_printf(m, "\n\t"); /* newline every 5th chan */
        seq_printf(m, "DO%02u: %04lu ", i, do_write_lo_ct[i]);
        ++ct;
      }
  }
  if(ct) seq_printf(m, "\n");
  else seq_printf(m, "(None Yet..)\n");
  
  seq_printf(m, "\n");

  if (AI_MODE == ASYNCH_MODE) {
    seq_printf(m, 
               "AI Asynch Info\n"
               "--------------\n"
               "CycTimeMax: %lu  CycTimeMin: %lu  CycTimeAvg: %lu\n"
               "NunScans: %lu  " "NumSkips: %lu  "  "NumScansSkipped: %lu  "  "NumAIOverflows: %lu\n\n",
               (unsigned long)cb_eos_cyc_max, (unsigned long)cb_eos_cyc_min, (unsigned long)cb_eos_cyc_avg,
               cb_eos_num_scans, cb_eos_skips, cb_eos_skipped_scans, ai_n_overflows);    
  }

  for (f = 0; f < NUM_STATE_MACHINES; ++f) {
    if (f > 0) seq_printf(m, "\n"); /* additional newline between FSMs */

    seq_printf(m, 
               "FSM %02u Info\n"
               "-----------\n", f);

    if (!rs[f].valid) {
      seq_printf(m, "FSM is not specified or is invalid.\n");
    } else {
      int structlen = sizeof(struct RunState) - sizeof(struct StateHistory);
      struct RunState *ss = (struct RunState *)vmalloc(structlen);
      /* too slow and too prone to allocation failures.. we'll live with the race condition.. */
      /*struct FSMSpec *fsm = (struct FSMSpec *)vmalloc(sizeof(struct FSMSpec));*/
      if (ss) {       
        memcpy(ss, (struct RunState *)&rs[f], structlen);
        /*memcpy(fsm, FSM_PTR(f), sizeof(*fsm)); \/\*we got rid of this because it was too big to alloc and copy each time.. we will live with the race condition..\*\/ */
        
        /* setup the pointer to the real fsm correctly */
        /*ss->states = fsm;*/
        ss->states = FSM_PTR(f); /* TODO XXX FIXME HACK 
                                    NB: THIS HAS THE POTENTIAL FOR RACE CONDITIONS/CRASHES!  
                                    ideally we should lock the fsm, copy it, unlock it! */
        
        seq_printf(m, "Current State: %d\t"    "Transition Count:%d\t"   "C-Var. Log Items: %d\n", 
                   (int)ss->current_state,     (int)NUM_TRANSITIONS(f),  (int)NUM_LOG_ITEMS(f));      

        seq_printf(m, "\n"); /* extra nl */
        
        if (ss->states->type == FSM_TYPE_EMBC) {
            char *embCProgram;
            if (ss->paused) { 
                seq_printf(m,
                            "FSM %02u State Program - Input Processing PAUSED\n"
                            "----------------------------------------------\n", f);
            } else {
                seq_printf(m,
                            "FSM %02u State Program\n"
                            "--------------------\n", f);
            }
            embCProgram = inflateCpy(ss->states->program.program_z, ss->states->program.program_z_len, ss->states->program.program_len, 0);
            if (!embCProgram) {
                seq_printf(m, " ** ERROR DECOMPRESSING PROGRAM TEXT IN MEMORY ** \n");
            } else {
                seq_printf(m, "%s\n", embCProgram);
            }
            freeDHBuf(embCProgram);
        } else if (ss->states->type == FSM_TYPE_PLAIN) {
            unsigned r, c;
            if (ss->paused) { 
                seq_printf(m,
                            "FSM %02u State Matrix - Input Processing PAUSED\n"
                            "---------------------------------------------\n", f);
            } else {
                seq_printf(m,
                            "FSM %02u State Matrix\n"
                            "-------------------\n", f);
            }
            for (r = 0; r < ss->states->n_rows; ++r) {
                for (c = 0; c < ss->states->n_cols; ++c) {
                    seq_printf(m, "%4u ", (unsigned)FSM_MATRIX_AT(ss->states, r, c));
                }
                seq_printf(m, "\n");
            }
        }

        { /* print input routing */
          unsigned i, j;
          seq_printf(m, "\nFSM %02u Input Routing\n"
                          "--------------------\n", f);
          seq_printf(m, "%s Channel IDs: ", ss->states->routing.in_chan_type == AI_TYPE ? "AI" : "DIO");
          /* argh this is an O(n^2) algorithm.. sorry :( */
          for (i = 0; i < ss->states->routing.num_evt_cols; ++i)
            for (j = 0; j < FSM_MAX_IN_EVENTS; ++j)
              if (ss->states->routing.input_routing[j] == i) {
                int chan_id = j/2;
                seq_printf(m, "%c%d ", j%2 ? '-' : '+', chan_id);
                break;
              }
          seq_printf(m, "\n");
        }

        { /* print static timer info */
          
          unsigned tstate_col = ss->states->routing.num_evt_cols;
          seq_printf(m, "\nFSM %02u Timeout Timer\n"
                          "--------------------\n", f);
          seq_printf(m, "Columns:\n");
          seq_printf(m, "\tColumn %u: timeout jump-state\n\tColumn %u: timeout time (microseconds)\n", tstate_col, tstate_col+1);
        }

        { /* print output routing */
          unsigned i;
          seq_printf(m, "\nFSM %02u Output Routing\n"
                          "---------------------\n", f);
          seq_printf(m, "Columns:\n");
          for (i = 0; i < ss->states->routing.num_out_cols; ++i) {
            struct OutputSpec *spec = 0;
            seq_printf(m, "\tColumn %u: ", i+ss->states->routing.num_evt_cols+2);
            spec = &ss->states->routing.output_routing[i];
            switch(spec->type) {
            case OSPEC_DOUT: 
            case OSPEC_TRIG: 
              seq_printf(m, "%s output on chans %u-%u\n", spec->type == OSPEC_DOUT ? "digital" : "trigger", spec->from, spec->to); 
              break;
            case OSPEC_EXT: seq_printf(m, "external triggering on object %u\n", spec->object_num); break;
            case OSPEC_SCHED_WAVE: seq_printf(m, "scheduled wave trigger\n"); break;
            case OSPEC_TCP: 
            case OSPEC_UDP: {
              unsigned len = strlen(spec->fmt_text);
              char *text = kmalloc(len+1, GFP_KERNEL);
              if (text) {
                unsigned j;
                strncpy(text, spec->fmt_text, len);  text[len] = 0;
                /* strip newlines.. */
                for (j = 0; j < len; ++j) if (text[j] == '\n') text[j] = ' ';
              }
              seq_printf(m, "%s packet for host %s port %u text: \"%s\"\n", spec->type == OSPEC_TCP ? "TCP" : "UDP", spec->host, spec->port, text ? text : "(Could not allocate buffer to print text)"); 
              kfree(text);
            }
              break;
            case OSPEC_NOOP: 
              seq_printf(m, "no operation (column ignored)\n"); 
              break;              
            default: 
              seq_printf(m, "*UNKNOWN OUTPUT COLUMN TYPE!*\n");
              break;              
            }
          }
        }

        if (ss->states->has_sched_waves) { /* Print Sched. Wave statistics */
          int i;
          seq_printf(m, "\nFSM %02u Scheduled Wave Info\n"
                          "--------------------------\n", f);
          /* Print stats on AO Waves */
          for (i = 0; i < FSM_MAX_SCHED_WAVES; ++i) {
            volatile struct AOWaveINTERNAL *w = ss->states->aowaves[i];
            if ( w && w->nsamples && w->samples) {
              seq_printf(m, "AO  Sched. Wave %d  %u bytes (%s)\n", i, (unsigned)(w->nsamples*(sizeof(*w->samples)+sizeof(*w->evt_cols))+sizeof(*w)), ss->active_ao_wave_mask & 0x1<<i ? "playing" : "idle");
            }
          }
          /* Print stats on DIO Sched Waves */
          for (i = 0; i < FSM_MAX_SCHED_WAVES; ++i) {
            if (ss->states->sched_waves[i].enabled) {
              seq_printf(m, "DIO Sched. Wave %d  (%s)\n", i, ss->active_wave_mask & 0x1<<i ? "playing" : "idle");
              seq_printf(m, "\tPreamble: %uus  Sustain: %uus  Refraction: %uus\n", ss->states->sched_waves[i].preamble_us, ss->states->sched_waves[i].sustain_us, ss->states->sched_waves[i].refraction_us);
              seq_printf(m, "\tRouting:  InpEvtCols +/-: ");
              if (ss->states->routing.sched_wave_input[i*2] > -1)
                seq_printf(m, "%d", ss->states->routing.sched_wave_input[i*2]);
              else
                seq_printf(m, "x");
              seq_printf(m, "/");
              if (ss->states->routing.sched_wave_input[i*2+1] > -1)
                seq_printf(m, "%d", ss->states->routing.sched_wave_input[i*2+1]);
              else
                seq_printf(m, "x");
              seq_printf(m, "  DO Line: ");
              if (ss->states->routing.sched_wave_dout[i] > -1)
                  seq_printf(m, "%d", ss->states->routing.sched_wave_dout[i]);
              else
                  seq_printf(m, "(None)");
              seq_printf(m, "  Sound: ");
              if (ss->states->routing.sched_wave_extout[i])
                  seq_printf(m, "%u", ss->states->routing.sched_wave_extout[i]);
              else
                  seq_printf(m, "(None)");
              seq_printf(m, "\n");
            }
          }
        }
        vfree(ss);
        /*vfree(fsm);*/
      } else {
        seq_printf(m, "Cannot retrieve FSM data:  Temporary failure in memory allocation.\n");
      }    
    }
  }
  return 0;
}

static inline int triggersExpired(FSMID_t f)
{
  return cycle - trig_cycle[f] >= (task_rate/1000)*trigger_ms;
}

static inline void resetTriggerTimer(FSMID_t f)
{
  trig_cycle[f] = cycle;
}

/**
 * This function does the following:
 *  Detects edges, does state transitions, updates state transition history,
 *  and also reads commands off the real-time fifo.
 *  This function is called by rtos scheduler every task period... 
 *  @see init()
 */
static void *doFSM (void *arg)
{
  struct timespec next_task_wakeup;
  hrtime_t cycleT0, cycleTf;
  FSMID_t f;

  (void)arg;

  atomic_set(&rt_task_running, 1);

  doSetupThatNeededFloatingPoint();

  clock_gettime(CLOCK_REALTIME, &next_task_wakeup);
    
  while (! atomic_read(&rt_task_stop)) {

    cycleT0 = gethrtime();
    ++cycle;
    
    /* see if we woke up jittery/late.. */
    tallyJitterStats(cycleT0, timespec_to_nano(&next_task_wakeup), task_period_ns);
    
    for (f = 0; f < NUM_STATE_MACHINES; ++f) {
      if (lastTriggers && triggersExpired(f)) 
        /* Clears the results of the last 'trigger' output done, but only
           when the trigger 'expires' which means it has been 'sustained' in 
           the on position for trigger_ms milliseconds. */
        clearTriggerLines(f);
    }
    
      /* Grab both DI and AI chans depending on the chans in use mask which
         was setup by reconfigureIO. */
    if (di_chans_in_use_mask) grabAllDIO(); 
    if (ai_chans_in_use_mask) grabAI();  
    
    for (f = 0; f < NUM_STATE_MACHINES; ++f) {
      /* Grab time */
      rs[f].current_ts = cycleT0 - rs[f].init_ts;
      rs[f].current_ts_secs = ((double)rs[f].current_ts) / 1e9;
      rs[f].ext_current_ts = FSM_EXT_TIME_GET(extTimeShm);

      handleFifos(f);
      
      if ( rs[f].valid ) {

        int got_timeout = 0, n_evt_loops, evt_id;
        FSMSpec *fsm = FSM_PTR(f); 
        unsigned state = rs[f].current_state;
        unsigned long state_timeout_us;
        
        /* clear events bitset */
        memset((void *)rs[f].events_bits, 0, MIN(NUM_INPUT_EVENTS(f)/8+8, sizeof(rs[f].events_bits)));
        
        state_timeout_us = STATE_TIMEOUT_US(fsm, state);
        
        if ( rs[f].forced_times_up ) {
          
          /* Ok, it was a forced timeout.. */
          if (state_timeout_us != 0) got_timeout = 1;
          rs[f].forced_times_up = 0;
          
        } 
        
        if (rs[f].forced_event > -1) {
          
          /* Ok, it was a forced input transition.. indicate this event in our bitfield array */
          set_bit(rs[f].forced_event, rs[f].events_bits);
          rs[f].forced_event = -1;
          
        } 
        
        /* If we aren't paused.. detect timeout events and input events */
        if ( !rs[f].paused  ) {
          
          CALL_EMBC(f, tick); /* call the embedded C tick function, if defined */

          /* Check for state timeout -- 
             IF Curent state *has* a timeout (!=0) AND the timeout expired */
          if ( !got_timeout && state_timeout_us != 0 && TIMER_EXPIRED(f, state_timeout_us) ) {
            
            got_timeout = 1;
            
            if (debug > 1) {
              char buf[22];
              strncpy(buf, uint64_to_cstr(rs[f].current_ts), 21);
              buf[21] = 0;
              DEBUG_VERB("timer expired in state %u t_us: %u timer: %s ts: %s\n", state, state_timeout_us, uint64_to_cstr(rs[f].current_timer_start), buf);
            }
            
          }
          
          /* Normal event transition code -- detectInputEvents() modifies the rs[f].events_bits
             bitfield array for all the input events we have right now.
             Note how we can have multiple ones, and they all get
             acknowledged by the loop later in this function!
             
             Note that we may have some events bits already 
             as part of the forced_event stuff above.  */
          detectInputEvents(f);   
          
          DEBUG_VERB("FSM %u Got input events mask %08x\n", f, *(unsigned *)rs[f].events_bits);
          
          /* Process the scheduled waves by checking if any of their components
             expired.  For scheduled waves that result in an input event
             on edge-up/edge-down, they will set those event ids
             in the rs[f].events_bits bitfield array.  */
          processSchedWaves(f);
          
          DEBUG_VERB("FSM %u After processSchedWaves(), got input events mask %08x\n", f, *(unsigned *)rs[f].events_bits);
          
          /* Process the scheduled AO waves -- do analog output for
             samples this cycle.  If there's an event id for the samples
             outputted, will get a mask of event ids set in rs[f].events_bits.  */
          processSchedWavesAO(f);
          
          DEBUG_VERB("FSM %u After processSchedWavesAO(), got input events mask %08x\n", f, *(unsigned *)rs[f].events_bits);
          
        }
        
        if (got_timeout) 
          /* Timeout expired, transistion to timeout_state.. */
          gotoState(f, STATE_TIMEOUT_STATE(fsm, state), -1);
        
        /* Normal event transition code, keep popping ones off our 
           bitfield array, events_bits. */
        for (n_evt_loops = 0, evt_id = 0; (evt_id = find_next_bit(rs[f].events_bits, NUM_INPUT_EVENTS(f), evt_id)) < NUM_INPUT_EVENTS(f) && n_evt_loops < NUM_INPUT_EVENTS(f); ++n_evt_loops, ++evt_id) {
            dispatchEvent(f, evt_id);
        }
        /*
        if (NUM_INPUT_EVENTS(f) && n_evt_loops >= NUM_INPUT_EVENTS(f)) {
          ERROR_INT("Event detection code in doFSM() for FSM %u tried to loop more than %d times!  DEBUG ME!\n", f, n_evt_loops, NUM_INPUT_EVENTS(f));
        }*/
        
      }
    } /* end loop through each state machine */

    commitDIOWrites();   
        
    /* do any necessary data acquisition and writing to shm->fifo_daq */
    doDAQ();

    
    /* keep some stats on cycle times.. */
    tallyCycleTime(cycleT0, cycleTf = gethrtime());
    
    { 
      long ct = -1;
      do {
        timespec_add_ns(&next_task_wakeup, (long)task_period_ns);
        ++ct;
      }  while (cycleTf > timespec_to_nano(&next_task_wakeup) && ct < 4); /* ensure that if we overran cycle period, we keep incrementing the next task wakeup by cycle period amounts.. */
      fsm_cycles_skipped += ct;
      if (ct)
        WARNING("Skipping %ld cycles for fsm cycle #%s! (Task overran its period?)\n", ct, cycleStr());
      if (ct >= 4) {
        WARNING("Resynching task wakeup -- too many cycles skipped!\n");
        clock_gettime(CLOCK_REALTIME, &next_task_wakeup);
        timespec_add_ns(&next_task_wakeup, (long)task_period_ns);
      }
    }
    
    /* Sleep until next period */    
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next_task_wakeup, 0);
  }

  atomic_set(&rt_task_running, 0);
  pthread_exit(0);
  return 0;
}

static inline volatile struct StateTransition *historyAt(FSMID_t f, unsigned idx) 
{
  return &rs[f].history.transitions[idx % MAX_HISTORY];
}

static inline volatile struct StateTransition *historyTop(FSMID_t f)
{
  return historyAt(f, NUM_TRANSITIONS(f)-1);
}

static inline volatile struct VarLogItem *logItemAt(FSMID_t f, unsigned idx) 
{
  return &rs[f].history.log_items[idx % MAX_HISTORY];
}

static inline volatile struct VarLogItem *logItemTop(FSMID_t f)
{
  return logItemAt(f, NUM_LOG_ITEMS(f)-1);
}

static inline void transitionNotifyUserspace(FSMID_t f, volatile struct StateTransition *transition)
{
  const unsigned long sz = sizeof(*transition);
  int err = rtf_put(shm->fifo_trans[f], (void *)transition, sz);  
  if (debug && err != (int)sz) {
    DEBUG("FSM %u error writing to state transition fifo, got %d, expected %lu -- free space is %d\n", f, err, sz, RTF_FREE(shm->fifo_trans[f]));
  }
}

static inline void historyPush(FSMID_t f, int event_id)
{
  volatile struct StateTransition * transition;

  /* increment current index.. it is ok to increment indefinitely since 
     indexing into array uses % MAX_HISTORY */
  ++rs[f].history.num_transitions;
  
  transition = historyTop(f);
  transition->previous_state = rs[f].previous_state;
  transition->state = rs[f].current_state;
  transition->ts = rs[f].current_ts;  
  transition->ext_ts = rs[f].ext_current_ts;
  transition->event_id = event_id;

  /* Store this information in a different place so that embedded C can see 
     it */
  rs[f].last_transition.time = rs[f].current_ts_secs;
  rs[f].last_transition.from = transition->previous_state;
  rs[f].last_transition.to = transition->state;
  rs[f].last_transition.event = transition->event_id;
  
  transitionNotifyUserspace(f, transition);
}

static inline void logItemPush(FSMID_t f, const char *n, double v)
{
  volatile struct VarLogItem * item;

  /* increment current index.. it is ok to increment indefinitely since 
     indexing into array uses % MAX_HISTORY */
  ++rs[f].history.num_log_items;
  
  item = logItemTop(f);
  item->ts = rs[f].current_ts_secs;
  strncpy((char *)item->name, n, FSM_MAX_SYMBOL_SIZE);
  item->name[FSM_MAX_SYMBOL_SIZE] = 0;
  item->value = v;
}

static int gotoState(FSMID_t f, unsigned state, int event_id)
{
  DEBUG_VERB("gotoState %u %u %d s: %u\n", f, state, event_id, rs[f].current_state);

  if (state >= NUM_ROWS(f)) {
    ERROR_INT("FSM %u state id %d is >= NUM_ROWS %d!\n", f, (int)state, (int)NUM_ROWS(f));
    return -1;
  }

  if (state == READY_FOR_TRIAL_JUMPSTATE(f) && rs[f].ready_for_trial_flg) {
    /* Special case of "ready for trial" flag is set so we don't go to 
       state 35, but instead to 0 */    
    state = 0;
    stopActiveWaves(f); /* since we are starting a new trial, force any
                           active timer waves to abort! */
  }

  if (state == 0) {
    /* Hack -- Rule is to only swap FSMs at state 0 crossing.*/
    rs[f].ready_for_trial_flg = 0;
    if (rs[f].pending_fsm_swap) swapFSMs(f);
  }

  if (rs[f].current_state != state)
    /* tell fsm embedded C that we are about to leave this state */
    CALL_EMBC1(f, exit, rs[f].current_state);
  
  rs[f].previous_state = rs[f].current_state;
  rs[f].current_state = state;
  historyPush(f, event_id); /* now add this transition to the history  */

  if (rs[f].current_state == rs[f].previous_state) 
  {
      /* Ok, the old state == the new state, that means we:

         1. *DO NOT* do any trigger outputs
         2. *DO NOT* do any continuous lines (keep the same as before)
         3. *DO NOT* reset the timer (unless it was a timeout event)!
         4. *DO* record this state transition. (already happened above..) */

      /* Ok, on timeout event reset the timer anyway.. */
      if (event_id < 0)  RESET_TIMER(f); 

      /* tell fsm embedded C about this transition */
      CALL_EMBC(f, statetransition);
      
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
      RESET_TIMER(f);

      /* tell fsm embedded C about this transition */
      CALL_EMBC(f, statetransition);
      CALL_EMBC1(f, entry, state);  /*rs[f].states->embc->entry(state);*/

      /* In new state, do output(s) (trig and cont).. */
      doOutput(f); 
    
      return 1; /* Yes, was a new state. */
  }

  return 0; /* Not reached.. */
}

static int bypassDOut(unsigned f, unsigned mask)
{
    unsigned prev_forced_mask = rs[f].forced_outputs_mask;
    int ret = 0;
    
    /* Clear previous forced outputs  */
    while (prev_forced_mask) {
        unsigned chan = __ffs(prev_forced_mask);
        prev_forced_mask &= ~(0x1<<chan);
        dataWrite_NoLocks(chan, 0);
        ret = 1;
    }
    rs[f].forced_outputs_mask = 0;
    if (rs[f].do_chans_cont_mask) {
        rs[f].forced_outputs_mask = (mask << __ffs(rs[f].do_chans_cont_mask)) & rs[f].do_chans_cont_mask;
        ret = 1;
    }
    return ret;
}

static void doNRTOutput(FSMID_t f, unsigned type, unsigned col, int trig, const char *host, unsigned short port, const void *data, unsigned datalen)
{
    static struct NRTOutput nrt_out;

    nrt_out.magic = NRTOUTPUT_MAGIC;
    nrt_out.state = rs[f].current_state;
    nrt_out.trig = trig;
    nrt_out.ts_nanos = rs[f].current_ts;
    nrt_out.type = type;
    nrt_out.col = col;
    snprintf(nrt_out.ip_host, IP_HOST_LEN, "%s", host);
    nrt_out.ip_port = port;
    if (type & NRT_BINDATA) {
        nrt_out.datalen = datalen;
        memcpy(nrt_out.data, data, MIN(datalen, sizeof(nrt_out.data)));
    } else {
        snprintf(nrt_out.ip_packet_fmt, FMT_TEXT_LEN, "%s", (const char *)data);
    }
    rtf_put(shm->fifo_nrt_output[f], &nrt_out, sizeof(nrt_out));
}

static void doSchedWaveOutput(FSMID_t f, int swBits)
{
        /* HACK!! Untriggering is funny since you need to do:
           -(2^wave1_id+2^wave2_id) in your FSM to untrigger! */
        int op = 1;
        /* if it's negative, invert the bitpattern so we can identify
           the wave id in question, then use 'op' to supply a negative waveid to
           scheduleWave() */
        if (swBits < 0)  { op = -1; swBits = -swBits; }
        /* Do scheduled wave outputs, if any */
        while (swBits) {
          int wave = __ffs(swBits);
          swBits &= ~(0x1<<wave);
          scheduleWave(f, wave, op);
        }
}

static void doOutput(FSMID_t f)
{
  unsigned i;
  unsigned trigs = 0, conts = 0, contmask = rs[f].do_chans_cont_mask; 
  unsigned state = rs[f].current_state;
  FSMSpec *fsm = FSM_PTR(f);
  
  for (i = 0; i < FSM_NUM_OUTPUT_COLS(fsm); ++i) {
    struct OutputSpec *spec = OUTPUT_ROUTING(f,i);
    switch (spec->type) {
    case OSPEC_DOUT:
      conts |= STATE_OUTPUT(fsm, state, i) << spec->from;
      break;
    case OSPEC_TRIG:
      trigs |= STATE_OUTPUT(fsm, state, i) << spec->from;
      break;
    case OSPEC_EXT:
      /* Do Ext 'virtual' triggers... */
      doExtTrigOutput(spec->object_num, STATE_OUTPUT(fsm, state, i));
      break;
    case OSPEC_SCHED_WAVE: 
      doSchedWaveOutput(f, STATE_OUTPUT(fsm, state, i));
      break;
    case OSPEC_TCP:
    case OSPEC_UDP:  /* write non-realtime TCP/UDP packet to fifo */
      /* suppress output of a dupe trigger -- IP packet triggers only get
         sent when state machine column has changed since the last time
         a packet was sent.  These two arrays get cleared on a new
         state matrix though (in reconfigureIO) */
      if (!rs[f].last_ip_outs_is_valid[i] 
          || rs[f].last_ip_outs[i] != STATE_OUTPUT(fsm, state, i)) {
        rs[f].last_ip_outs[i] = STATE_OUTPUT(fsm, state, i);
        rs[f].last_ip_outs_is_valid[i] = 1;
        doNRTOutput(f, /* fsm id */
                    (spec->type == OSPEC_TCP) ? NRT_TCP : NRT_UDP,  /* type */
                    FIRST_OUT_COL(fsm)+i,  /* col */
                    STATE_OUTPUT(fsm, state, i), /* trig */
                    spec->host,
                    spec->port,
                    spec->fmt_text, /* data */
                    0 /*datalen, ignored because type flag NRT_BINDATA is not set */
                   );
      }
      break;
    }
  }

  /* Do trigger outputs */
  if ( trigs ) {
    /* is calling clearTriggerLines here really necessary?? It can interfere
       with really fast state transitioning but 'oh well'.. */
    clearTriggerLines(f); /* FIXME See what happens if you get two rapid triggers. */
    while (trigs) {
      i = __ffs(trigs);
      trigs &= ~(0x1<<i);
      dataWrite(i, 1);  
      lastTriggers |= 0x1<<i; 
    }
    resetTriggerTimer(f);
  }

  /* Do continuous outputs */
  while ( contmask ) {
    i = __ffs(contmask);
    contmask &= ~(0x1<<i);
    if ( (0x1<<i) & conts )  dataWrite(i, 1);
    else                     dataWrite(i, 0);
  }

}

static inline void clearTriggerLines(FSMID_t f)
{
  unsigned i;
  unsigned mask = rs[f].do_chans_trig_mask;
  while (mask) {
    i = __ffs(mask);
    mask &= ~(0x1<<i);
    if ( (0x1 << i) & lastTriggers ) {
      dataWrite(i, 0);
      lastTriggers &= ~(0x1<<i);
    }
  }
}

static void detectInputEvents(FSMID_t f)
{
  unsigned i, try_use_embc_thresh = 0;
  unsigned global_bits = 0;
  volatile unsigned long *thresh_hi_ct = 0, *thresh_lo_ct = 0;

  rs[f].input_bits_prev = rs[f].input_bits;
  
  switch(IN_CHAN_TYPE(f)) { 
  case DIO_TYPE: /* DIO input */
    global_bits = dio_bits;    
    thresh_hi_ct = di_thresh_hi_ct;
    thresh_lo_ct = di_thresh_lo_ct;
    try_use_embc_thresh = 0;
    break;
  case AI_TYPE: /* AI input */
    global_bits = ai_bits;
    thresh_hi_ct = ai_thresh_hi_ct;
    thresh_lo_ct = ai_thresh_lo_ct;
    try_use_embc_thresh = 1;
    break;
  default:  
    /* Should not be reached, here to suppress warnings and avoid possible NULL ptr? */
    ERROR_INT("Illegal IN_CHAN_TYPE for fsm %u -- THIS SHOULD NEVER HAPPEN!!\n", (unsigned)f);
    return;
    break;
  }
  
  /* Loop through all our event channel id's comparing them to our DIO bits */
  for (i = FIRST_IN_CHAN(f); i < AFTER_LAST_IN_CHAN(f); ++i) {
      int bit = 0,  /* default bit to 0, either it comes from EmbC or from global thresh detector */
        last_bit = ((0x1 << i) & rs[f].input_bits_prev) != 0,
        event_id_edge_up = INPUT_ROUTING(f, i*2), /* Even numbered input event 
                                                  id's are edge-up events.. */
        event_id_edge_down = INPUT_ROUTING(f, i*2+1); /* Odd numbered ones are 
                                                      edge-down */

    /* If they specified a threshold detection function for this FSM, call it, 
        and use the bit from that. */
    if (try_use_embc_thresh && EMBC_HAS_FUNC(f, threshold_detect)) {
        TRISTATE ret = CALL_EMBC_RET2(f, threshold_detect, i, ai_samples_volts[i]);
        if (ISPOSITIVE(ret)) { bit = 1;  }
        else if (ISNEGATIVE(ret)) { bit = 0; }
        else bit = last_bit; /* NEUTRAL condition */
    } else {
        /* No EmbC or using DIO, so just use the global bits to get our bit */
        bit = ((0x1 << i) & global_bits) != 0;        
    }

    /* now we save the bit state per FSM -- at this point this bit (specifying
       whether there's an high or low thesh)
       *may* have come from either 
         - Embedded C threshold detection 
         - or from the global threshold detector
         
         but we need to remember the bitstate per-fsm. */

    if (bit) {
        set_bit(i, &rs[f].input_bits);
    } else {
        clear_bit(i, &rs[f].input_bits);
    }

    /* Edge-up transitions */ 
    if (event_id_edge_up > -1 && bit && !last_bit) {
      /* before we were below, now we are above,  therefore yes, it is an blah-IN */
      set_bit(event_id_edge_up, rs[f].events_bits); 
      DEBUG_VERB("detectInputEvents set bit %d\n", event_id_edge_up);
       ++thresh_hi_ct[i]; /* tally this thresh crossing 
                         -- writes to either globals: ai_thresh_hi_ct or di_thresh_lo_ct */
    }

    
    /* Edge-down transitions */ 
    if (event_id_edge_down > -1 /* input event is actually routed somewhere */
        && last_bit /* Last time we were above */
        && !bit ) {/* Now we are below, therefore yes, it is event*/
      set_bit(event_id_edge_down, rs[f].events_bits); 
      DEBUG_VERB("detectInputEvents set bit %d\n", event_id_edge_down);
      ++thresh_lo_ct[i]; /* tally this thresh crossing, either writes to ai_thresh_lo_ct or di_thresh_lo_ct*/
    }
  }
}


static void dispatchEvent(FSMID_t f, unsigned event_id)
{
  unsigned next_state = 0;
  FSMSpec *fsm = FSM_PTR(f);
  unsigned state = rs[f].current_state;

  if (event_id > NUM_INPUT_EVENTS(f)) {
    ERROR_INT("FSM %u event id %d is > NUM_INPUT_EVENTS %d!\n", f, (int)event_id, (int)NUM_INPUT_EVENTS(f));
    return;
  }
  next_state = (event_id == NUM_INPUT_EVENTS(f)) ? STATE_TIMEOUT_STATE(fsm, state) : STATE_COL(fsm, state, event_id); 
  gotoState(f, next_state, event_id);
}

/* Set everything to zero to start fresh */
static void clearAllOutputLines(FSMID_t f)
{
  uint i, mask;
  mask = rs[f].do_chans_trig_mask|rs[f].do_chans_cont_mask;
  while (mask) {
    i = __ffs(mask);
    mask &= ~(0x1<<i);
    dataWrite(i, 0);
  }
}

static atomic_t buddyTaskCmds[NUM_STATE_MACHINES];

static void handleFifos(FSMID_t f)
{
# define buddyTaskCmd (atomic_read(&buddyTaskCmds[f]))
# define BUDDY_TASK_BUSY (buddyTaskCmd > 0 ? buddyTaskCmd : 0)
# define BUDDY_TASK_DONE (buddyTaskCmd < 0 ? -buddyTaskCmd : 0)
# define BUDDY_TASK_RESULT (-buddyTaskCmd)
# define BUDDY_TASK_CLEAR (atomic_set(&buddyTaskCmds[f], 0))
# define BUDDY_TASK_PEND(arg) \
  do { \
    atomic_set(&buddyTaskCmds[f], arg); \
    softTaskPend(buddyTask[f], (void *)(long)f); \
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

    switch(BUDDY_TASK_RESULT) {
    case FSM:
      rs[f].pending_fsm_swap = 0;
      rs[f].states = OTHER_FSM_PTR(f); /* temp. swap in the new fsm.. */
      errcode = doSanityChecksRuntime(f); /* check new FSM  sanity -- also checks if buddy task indicated error */
      rs[f].states = OTHER_FSM_PTR(f); /* temp. swap back the old fsm.. */ 
      if (errcode) {  /* sanity check failed.. */
        
          /* uh-oh.. it's a bad FSM?  Reject it.. */
          /*initRunState(f);
          reconfigureIO();*/

      } else {  /* FSM passed sanity check.. */

          /*LOG_MSG("Cycle: %lu  Got new FSM\n", (unsigned long)cycle);*/
        if (!rs[f].valid || !OTHER_FSM_PTR(f)->wait_for_jump_to_state_0_to_swap_fsm)
          /* fsm did not request to cleanly wait for last FSM, or there is no
             last fsm.. */
          swapFSMs(f);
        else
          /* fsm requested to cleanly wait for last FSM to exit.. 
             so we pend the swap until a state 0 crossing of the old fsm */
          rs[f].pending_fsm_swap = 1;
      }
      do_reply = 1;
      break;

    case GETFSM:
    case GETERROR:
      do_reply = 1;
      break;
      
    case RESET_:
      /* these may have had race conditions with non-rt, so set them again.. */
      rs[f].current_ts = 0;

      RESET_TIMER(f);
      reconfigureIO(); /* to reset DIO config since our routing spec 
                           changed */
      do_reply = 1;
      break;

    case AOWAVE:
      /* need up upadte rs.states->has_sched_waves flag as that affects 
       * other logic on whether we check certain data structures for data. */
      updateHasSchedWaves(f);
      /* we just finished processing/allocating an AO wave, reply to 
         userspace now */
      do_reply = 1;
      break;

    case LOGITEMS:
    case TRANSITIONS:
        do_reply = 1; /* just reply that it's done */
        break;
        
    default:
      ERROR_INT(" Got bogus reply %d from non-RT buddy task!\n", BUDDY_TASK_RESULT);
      break;
    }

    BUDDY_TASK_CLEAR; /* clear pending result */

  } else {     /* !BUDDY_TASK_BUSY */

    /* See if a message is ready, and if so, take it from the SHM */
    struct ShmMsg *msg = (struct ShmMsg *)&shm->msg[f];

    errcode = rtf_get_if(shm->fifo_in[f], &dummy, sizeof(dummy));

    /* errcode == 0 on no data available, that's ok */
    if (!errcode) return; 

    if (errcode != sizeof(dummy)) {
      static int once_only = 0;
      if (!once_only)
        ERROR_INT("(FSM No %u) got return value of (%d) when reading from fifo_in %d\n", f, errcode, shm->fifo_in[f]), once_only++;
      return;
    }
      
    switch (msg->id) {
        
    case RESET_:
      /* Just to make sure we start off fresh ... */
      stopActiveWaves(f);
      clearAllOutputLines(f);
      rs[f].valid = 0; /* lock fsm so buddy task can safely manipulate it */
      BUDDY_TASK_PEND(RESET_); /* slow operation because it clears FSM blob,
                                 pend it to non-RT buddy task. */
      reconfigureIO(); /* to reset DIO config since our routing spec 
                           changed */
      break;
        
    case TRANSITIONCOUNT:
      msg->u.transition_count = NUM_TRANSITIONS(f);
      do_reply = 1;
      break;

    case LOGITEMCOUNT:
      msg->u.log_item_count = NUM_LOG_ITEMS(f);
      do_reply = 1;
      break;
      
    case TRANSITIONS:
      {
        unsigned *from = &msg->u.transitions.from; /* Shorthand alias.. */
        unsigned *num = &msg->u.transitions.num; /* alias.. */
        /*unsigned i;*/

        if ( *from >= NUM_TRANSITIONS(f)) 
          *from = NUM_TRANSITIONS(f) ? NUM_TRANSITIONS(f)-1 : 0;
        if (*num + *from > NUM_TRANSITIONS(f))
          *num = NUM_TRANSITIONS(f) - *from + 1;
        
        if (*num > MSG_MAX_TRANSITIONS)
          *num = MSG_MAX_TRANSITIONS;

        /*for (i = 0; i < *num; ++i)
          memcpy((void *)&msg->u.transitions.transitions[i],
                 (const void *)historyAt(f, *from + i),
                 sizeof(struct StateTransition));
        do_reply = 1;*/
        BUDDY_TASK_PEND(TRANSITIONS);
      }
      break;

    case LOGITEMS:
      {
        unsigned *from = &msg->u.log_items.from; /* Shorthand alias.. */
        unsigned *num = &msg->u.log_items.num; /* alias.. */
        /*unsigned i;*/

        if ( *from >= NUM_LOG_ITEMS(f)) 
          *from = NUM_LOG_ITEMS(f) ? NUM_LOG_ITEMS(f)-1 : 0;
        if (*num + *from > NUM_LOG_ITEMS(f))
          *num = NUM_LOG_ITEMS(f) - *from + 1;
        
        if (*num > MSG_MAX_LOG_ITEMS)
          *num = MSG_MAX_LOG_ITEMS;

        /*for (i = 0; i < *num; ++i)
          memcpy((void *)&msg->u.log_items.items[i],
                 (const void *)logItemAt(f, *from + i),
                 sizeof(struct VarLogItem));
        do_reply = 1;*/
        BUDDY_TASK_PEND(LOGITEMS);
      }
      break;
      
      
    case PAUSEUNPAUSE:
      rs[f].paused = !rs[f].paused;
      /* We reset the timer, just in case the new FSM's rs.current_state 
         had a timeout defined. */
      RESET_TIMER(f);
      /* Notice fall through to next case.. */
    case GETPAUSE:
      msg->u.is_paused = rs[f].paused;
      do_reply = 1;
      break;
      
    case INVALIDATE:
      rs[f].valid = 0;
      /* Notice fall through.. */
    case GETVALID:
      msg->u.is_valid = rs[f].valid;
      do_reply = 1;
      break;
      
    case FSM:
      /* rs.valid = 0; / * don't lock fsm since it will cause dropped input/output events! instead we have the buddy task write to the alternate fsm which we swap in as current when the buddy task completes (see beginning of this function) */
      
      DEBUG("handleFifos(%u) new FSM at cycle %s currentstate: %u\n", f,
            uint64_to_cstr(cycle), 
            rs[f].current_state);

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

    case GETERROR:
      BUDDY_TASK_PEND(GETERROR); /* Copies rather large string, pend to nrt */
      break;
      
    case SETAIMODE: /* reset AI mode to/from asynch/synch -- may (re)start the comedi cmd */
      ai_mode = msg->u.ai_mode_is_asynch ? ASYNCH_MODE : SYNCH_MODE;
      restartAIAcquisition(0);
      do_reply = 1;
      break;
      
    case GETAIMODE: /* query the current acquisition mode */
      msg->u.ai_mode_is_asynch = AI_MODE == ASYNCH_MODE;
      do_reply = 1;
      break;
            
    case GETFSMSIZE:
      if (rs[f].valid) {
        
        rs[f].valid = 0; /* lock fsm? */       
        
        msg->u.fsm_size[0] = FSM_PTR(f)->n_rows;
        msg->u.fsm_size[1] = FSM_PTR(f)->n_cols;
        
        rs[f].valid = 1; /* Unlock FSM.. */
        
      } else {
        
        memset((void *)msg->u.fsm_size, 0, sizeof(msg->u.fsm_size));
        
      }
      
      do_reply = 1;
      
      break;        
    case GETNUMINPUTEVENTS:
      if (rs[f].valid) {
        rs[f].valid = 0; /* lock fsm? */
        msg->u.num_input_events = NUM_INPUT_EVENTS(f);
        rs[f].valid = 1; /* unlock fsm */
      } else {
        msg->u.num_input_events = 0;        
      }
      do_reply = 1;
      break;
    case FORCEEVENT:
        if (msg->u.forced_event < NUM_INPUT_EVENTS(f))
          rs[f].forced_event = msg->u.forced_event;
        do_reply = 1;
        break;
        
    case FORCETIMESUP:
        rs[f].forced_times_up = 1;
        do_reply = 1;
        break;

    case FORCEEXT:
      if (rs[f].valid) { /* umm.. have to figure out which ext trig object they want from output 
           spec */
        unsigned which = f, i;
        for (i = 0; i < NUM_OUT_COLS(f); ++i)
          if (OUTPUT_ROUTING(f, i)->type == OSPEC_EXT) {
            which = OUTPUT_ROUTING(f, i)->object_num;
            break;
          }
        if (i < NUM_OUT_COLS(f)) /* otherwise we didn't find one */
            doExtTrigOutput(which, msg->u.forced_triggers);
        do_reply = 1;
      }
      break;
        
    case FORCEOUTPUT:
      bypassDOut(f, msg->u.forced_outputs);
      do_reply = 1;
      break;

      case GETRUNTIME:
        msg->u.runtime_us = Nano2USec(rs[f].current_ts);
        do_reply = 1;
        break;

      case READYFORTRIAL:
        if (rs[f].current_state == READY_FOR_TRIAL_JUMPSTATE(f)) {
            rs[f].ready_for_trial_flg = 0;
            if(rs[f].valid) gotoState(f, 0, -1);
        } else
            rs[f].ready_for_trial_flg = 1;
        do_reply = 1;
        break;

      case GETCURRENTSTATE:
        msg->u.current_state = rs[f].current_state;
        do_reply = 1;
        break;

      case FORCESTATE:
        if ( !rs[f].valid || gotoState(f, msg->u.forced_state, -1) < 0 )
          msg->u.forced_state = -1; /* Indicate error.. */
        do_reply = 1;
        break;
	
      case STARTDAQ:
        msg->u.start_daq.range_min = ai_krange.min;        
        msg->u.start_daq.range_max = ai_krange.min;
        msg->u.start_daq.maxdata = maxdata_ai;
        {
          unsigned ch;
          rs[f].daq_ai_chanmask = 0;
          rs[f].daq_ai_nchans = 0;
          for (ch = 0; ch < NUM_AI_CHANS; ++ch)
            if (msg->u.start_daq.chan_mask & (0x1<<ch)) {
              rs[f].daq_ai_chanmask |= 0x1<<ch;
              ++rs[f].daq_ai_nchans;
            }
        }
        msg->u.start_daq.started_ok = rs[f].daq_ai_chanmask;
        msg->u.start_daq.chan_mask = rs[f].daq_ai_chanmask;
        do_reply = 1;
        break;

      case STOPDAQ:
        rs[f].daq_ai_nchans = rs[f].daq_ai_chanmask = 0;
        do_reply = 1;
        break;
        
      case GETAOMAXDATA:
        msg->u.ao_maxdata = maxdata_ao;
        do_reply = 1;
        break;

    case AOWAVE:
        
        if (rs[f].valid)
            BUDDY_TASK_PEND(AOWAVE); /* a slow operation -- it has to allocate
                                        memory, etc, so pend to a linux process
                                        context buddy task 
                                        (see buddyTaskHandler()) */
        break;

      default:
        rt_printk(MODULE_NAME": Got unknown msg id '%d' in handleFifos(%u)!\n", 
                   msg->id, f);
        do_reply = 0;
        break;
    }
  }

  if (do_reply) {
    errcode = rtf_put(shm->fifo_out[f], &dummy, sizeof(dummy));
    if (errcode != sizeof(dummy)) {
      static int once_only = 0;
      if (!once_only) 
        ERROR_INT("rtos_fifo_put to fifo_out %d returned %d!\n", 
                  shm->fifo_out[f], errcode), once_only++;
      return;
    }
  }

# undef buddyTaskCmd
# undef BUDDY_TASK_BUSY
# undef BUDDY_TASK_DONE
# undef BUDDY_TASK_CLEAR
# undef BUDDY_TASK_PEND

}

static unsigned pending_output_bits = 0, pending_output_mask = 0;

static void dataWrite_NoLocks(unsigned chan, unsigned bit)
{
  unsigned bitpos = 0x1 << chan;
  if (!(bitpos & do_chans_in_use_mask)) {
    ERROR_INT("Got write request for a channel (%u) that is not in the do_chans_in_use_mask (%x)!  FIXME!\n", chan, do_chans_in_use_mask);
    return;
  }
  
  pending_output_mask |= bitpos;
  if (bit) { /* set bit.. */
    pending_output_bits |= bitpos;
  } else {/* clear bit.. */
    pending_output_bits &= ~bitpos;
  }
}

static void dataWrite(unsigned chan, unsigned bit)
{
    if (!IS_DO_CHAN_LOCKED(chan)) /* locked chans are used by the scheduled waves to prevent SM from overwriting DIO lines that scheduled waves are currently using*/
        dataWrite_NoLocks(chan, bit);
}

static void commitDIOWrites(void)
{
  hrtime_t dio_ts = 0, dio_te = 0;
  FSMID_t f;
  static unsigned output_bits = 0, prev_output_bits = 0;
  
  for (f = 0; f < NUM_STATE_MACHINES; ++f) {
    /* Override with the 'forced' bits. */
    pending_output_mask |= rs[f].forced_outputs_mask;
    pending_output_bits |= rs[f].forced_outputs_mask;
  }
  output_bits = pending_output_bits&pending_output_mask;
   
  if (debug > 2)  dio_ts = gethrtime();
  if (dev && pending_output_mask) do_dio_bitfield(pending_output_mask, &pending_output_bits);
  if (debug > 2)  dio_te = gethrtime();
    
  DEBUG_CRAZY("WRITES: dio_out mask: %x bits: %x for cycle %s took %u ns\n", pending_output_mask, output_bits, uint64_to_cstr(cycle), (unsigned)(dio_te - dio_ts));

  /** Tally stats do_write_*_ct stats.. only tallied if bit changed */
  while (pending_output_mask) {
      unsigned bit = __ffs(pending_output_mask), mask = 0x1<<bit, val = output_bits&mask, prev_val = prev_output_bits&mask;
      if ( val != prev_val ) { /* test if bit changed.. */
          if (val)  { ++do_write_hi_ct[bit]; prev_output_bits |= mask; }
          else      { ++do_write_lo_ct[bit]; prev_output_bits &= ~mask; }
      }
      pending_output_mask &= ~mask; /* clear bit */
  }
  pending_output_bits = 0;
  pending_output_mask = 0;
}

static void grabAllDIO(void)
{
  /* Remember previous bits */
  dio_bits_prev = dio_bits;
  /* Grab all the input channels at once */
  if (dev) do_dio_bitfield(0, &dio_bits);
  dio_bits = dio_bits & di_chans_in_use_mask;
  /* Debugging comedi reads.. */
  if (dio_bits && ullmod(cycle, task_rate) == 0 && debug > 1)
    DEBUG_VERB("READS 0x%x\n", dio_bits);
}

static void grabAI(void)
{
  unsigned mask = ai_chans_in_use_mask;

  /* Remember previous bits */
  ai_bits_prev = ai_bits;

  /* Grab all the AI input channels that are masked as 'in-use' by an FSM */
  while (mask) {
    int chan = __ffs(mask);    
    mask &= ~(0x1<<chan); /**< clear bit */
    if (!readAI(chan)) return; /**< error in readAI -- it already printed a message to log for us so just return */
  } 
}

static int readAI(unsigned chan)
{
    lsampl_t sample;
    static uint64 last_cycle_ai_was_read = 0xffffffffffffffffULL;
    
    if (chan >= MAX_AI_CHANS) {
        WARNING("readAI(): passed-in channel id %u is an illegal (out-of-range) value!", chan);
        return 0;
    }
    
    if (last_cycle_ai_was_read != cycle) {
        /* clear bitmask of our cached samples -- this forces readAI() below to actually do a real read */
        ai_chans_seen_this_scan_mask = 0; 
        last_cycle_ai_was_read = cycle;
    }

    /* If we already have it for this scan, no sense in reading it again.. */
    if ( (1<<chan) & ai_chans_seen_this_scan_mask ) return 1;
    
    /* Otherwise do a conversion and/or copy it from ASYNCH buffer if in ASYNCH mode.. */
     if (AI_MODE == SYNCH_MODE) {
      /* Synchronous AI, so do the slow comedi_data_read() */
      int err = comedi_data_read(dev_ai, subdev_ai, chan, ai_range, AREF_GROUND, &sample);
      if (err != 1) {
        WARNING("readAI(): comedi_data_read returned %d on AI chan %d!\n", err, chan);
        return 0;
      }
      
      ai_samples[chan] = sample; /* save the sample for doDAQ() function.. */
      
      if (chan == 0 && debug) putDebugFifo(sample); /* write AI 0 to debug fifo*/
      
    } else { /* AI_MODE == ASYNCH_MODE */
      /* Asynch IO, read samples from our ai_aynch_samples which was populated
         by our comediCallback() function. */
      sample = ai_samples[chan];
    }
    /* mark it as seen this scan */
    ai_chans_seen_this_scan_mask |= 1<<chan; 
    
    /* At this point, we don't care anymore about synch/asynch.. we just
       have a sample.  */
    
    /* Now, scale it to a volts value for embC stuff to use */
    ai_samples_volts[chan] = SAMPLE_TO_VOLTS(sample);
    

    /* Next, we translate the sample into either a digital 1 or a digital 0.
       That is, we threshold detect on it.

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
      ai_bits |= 0x1<<chan; /* set the bit, it's above our arbitrary thresh. */
    } else if (sample <= ai_thresh_low) {
      /* We just dropped below ai_thesh_low, so definitely clear the bit. */
      ai_bits &= ~(0x1<<chan); /* clear the bit, it's below ai_thesh_low */
    } else {
      /* No change from last scan.
         This happens sometimes when we are between ai_thesh_low 
         and ai_thresh_high. */
    }

    return 1;
}
/** writes samp to our AO subdevice.  This is a thin wrapper around comedi_data_write that also tallyies some stats.. */
static int writeAO(unsigned chan, lsampl_t samp)
{ 
    int ret;
    if (chan >= NUM_AO_CHANS) return 0;
    ret = comedi_data_write(dev_ao, subdev_ao, chan, ao_range, 0, samp);
    if (ret > 0 && chan < MAX_AO_CHANS) ++ao_write_ct[chan];
    return ret > 0;
}

static void doDAQ(void)
{
  FSMID_t f;

  for (f = 0; f < NUM_STATE_MACHINES; ++f) {
    static unsigned short samps[MAX_AI_CHANS];
    unsigned mask = rs[f].daq_ai_chanmask;
    unsigned ct = 0;
    while (mask) {
      unsigned ch = __ffs(mask);
      mask &= ~(0x1<<ch); /* clear bit */
      readAI(ch); /**< may be a noop if it's cached, otherwise goes to hardware to do a real read */
      samps[ct] = ai_samples[ch];
      ++ct;
    }
    if (ct) {
      struct DAQScan scan;
      scan.magic = DAQSCAN_MAGIC;
      scan.ts_nanos = rs[f].current_ts;
      scan.nsamps = ct;
      rtf_put(shm->fifo_daq[f], &scan, sizeof(scan));
      rtf_put(shm->fifo_daq[f], samps, sizeof(samps[0])*ct);
    }
  }
}

#ifndef RTAI
/* just like clock_gethrtime but instead it used timespecs */
static inline void clock_gettime(clockid_t clk, struct timespec *ts)
{
  hrtime_t now = clock_gethrtime(clk);
  nano2timespec(now, ts);  
}
#endif

static inline void nano2timespec(hrtime_t time, struct timespec *t)
{
  unsigned long rem = 0;
  t->tv_sec = ulldiv(time, 1000000000, &rem);
  t->tv_nsec = rem;
}
#ifndef RTAI /* RTAI already has a definition of this */
static inline unsigned long long ulldiv(unsigned long long ull, unsigned long uld, unsigned long *r)
{
        *r = do_div(ull, uld);
        return ull;
}
#endif
static inline unsigned long ullmod(unsigned long long ull, unsigned long uld)
{         
        unsigned long ret;
        ulldiv(ull, uld, &ret);
        return ret;
}
static inline long long lldiv(long long ll, long ld, long *r)
{
  int neg1 = 0, neg2 = 0;
  
  if (ll < 0) (ll = -ll), neg1=1;
  if (ld < 0) (ld = -ld), neg2=1;
  *r = do_div(ll, ld);
  if ( (neg1 || neg2) && !(neg1&&neg2) ) ll = -ll;
  return ll;
}
static inline unsigned long long Nano2Sec(unsigned long long nano)
{
  unsigned long rem = 0;
  return ulldiv(nano, BILLION, &rem);
}

static inline unsigned long long Nano2USec(unsigned long long nano)
{
  unsigned long rem = 0;
  return ulldiv(nano, 1000, &rem);
}

static int int64_to_cstr_r(char *buf, unsigned bufsz, int64 num)
{
  static const int64 ZEROLL = 0LL;
  static const int32 dividend = 10;
  int ct = 0, i;
  int64 quotient = num;
  long remainder;
  char reversebuf[22];
  unsigned sz = 22;

  if (bufsz < sz) sz = bufsz;
  if (sz) buf[0] = 0;
  if (sz <= 1) return 0;

  /* convert to base 10... results will be reversed */
  do {
    quotient = lldiv(quotient, dividend, &remainder);
    reversebuf[ct++] = remainder + '0';
  } while (quotient != ZEROLL && ct < sz-1);

  /* append negative to negative values.. */
  if (num < 0) reversebuf[ct++] = '-';
  
  /* now reverse the reversed string... */
  for (i = 0; i < ct && i < sz-1; i++) 
    buf[i] = reversebuf[(ct-i)-1];
  
  /* add nul... */
  buf[ct] = 0;
  
  return ct;
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
    remainder = do_div(quotient, dividend);
    reversebuf[ct++] = remainder + '0';
  } while (quotient != ZEROULL && ct < sz-1);

  /* now reverse the reversed string... */
  for (i = 0; i < ct && i < sz-1; i++) 
    buf[i] = reversebuf[(ct-i)-1];
  
  /* add nul... */
  buf[ct] = 0;
  
  return ct;
}

static const char *uint64_to_cstr(uint64 num)
{
  static char buf[10][21];
  static atomic_t index = ATOMIC_INIT(0);
  long flags;
  unsigned i;
  rt_critical(flags);
  i = ((unsigned)atomic_read(&index)) % 10;
  atomic_inc(&index);
  rt_end_critical(flags);
  uint64_to_cstr_r(buf[i], 21, num);
  return buf[i];
}

static const char *int64_to_cstr(int64 num)
{
  static char buf[10][22];
  static atomic_t index = ATOMIC_INIT(0);
  long flags;
  unsigned i;
  rt_critical(flags);
  i = ((unsigned)atomic_read(&index)) % 10;
  atomic_inc(&index);
  rt_end_critical(flags);
  int64_to_cstr_r(buf[i], 22, num);
  return buf[i];
}

static inline long Micro2Sec(long long micro, unsigned long *remainder)
{
  int neg = micro < 0;
  unsigned long rem;
  long ans;
  if (neg) micro = -micro;
  ans = ulldiv(micro, MILLION, &rem);
  if (neg) ans = -ans;
  if (remainder) *remainder = rem;
  return ans;
}

static inline void UNSET_EXT_TRIG(unsigned which, unsigned trig)
{
  DEBUG_VERB("Virtual Trigger %d unset for target %u\n", trig, which); 
  FSM_EXT_UNTRIG(extTrigShm, which, trig); 
}

static inline void SET_EXT_TRIG(unsigned which, unsigned trig)
{
  DEBUG_VERB("Virtual Trigger %d set for target %u\n", trig, which); 
  FSM_EXT_TRIG(extTrigShm, which, trig); 
}

static inline void doExtTrigOutput(unsigned which, unsigned t) 
{
  int trig = *(int *)&t;
  if (!trig) return;
  if (trig < 0) /* Is 'last' bit set? */ 
      UNSET_EXT_TRIG( which, -trig );
    else 
      SET_EXT_TRIG( which, trig ); 
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

static void processSchedWaves(FSMID_t f)
{  
  unsigned wave_mask = rs[f].active_wave_mask;

  while (wave_mask) {
    unsigned wave = __ffs(wave_mask);
    struct ActiveWave *w = &((struct RunState *)&rs[f])->active_wave[wave];
    wave_mask &= ~(0x1<<wave);

    if (w->edge_up_ts && w->edge_up_ts <= rs[f].current_ts) {
      /* Edge-up timer expired, set the wave high either virtually or
         physically or both. */
          int id = SW_INPUT_ROUTING(f, wave*2);
          if (id > -1 && id <= NUM_IN_EVT_COLS(f)) {
           /* mark the event as having occurred
              for the in-event of the matrix, if
              it's routed as an input event wave
              for edge-up transitions. */
              set_bit(id, rs[f].events_bits); 
          }
          
          id = SW_DOUT_ROUTING(f, wave);
          if ( id > -1 ) 
            dataWrite_NoLocks(id, 1); /* if it's routed to do digital output, set line high. */
          

          id = SW_EXTOUT_ROUTING(f, wave);
          if ( id > 0 ) /* if it's routed to do ext trigs */
            doExtTrigOutput(DEFAULT_EXT_TRIG_OBJ_NUM(f), id); /* trigger sound, etc */
              
          w->edge_up_ts = 0; /* mark this component done */
    }
    if (w->edge_down_ts && w->edge_down_ts <= rs[f].current_ts) {
      /* Edge-down timer expired, set the wave high either virtually or
         physically or both. */
          int id = SW_INPUT_ROUTING(f, wave*2+1);
          if (id > -1 && id <= NUM_IN_EVT_COLS(f)) {
             /* mark the event as having occurred
                for the in-event of the matrix, if
                it's routed as an input event wave
                for edge-up transitions. */
            set_bit(id, rs[f].events_bits);
          }
          
          id = SW_DOUT_ROUTING(f, wave);
          if ( id > -1 ) 
            dataWrite_NoLocks(id, 0); /* if it's routed to do digital output, set line low. */

          id = SW_EXTOUT_ROUTING(f, wave);
          if ( id > 0 ) /* if it's routed to do ext trigs */
            doExtTrigOutput(DEFAULT_EXT_TRIG_OBJ_NUM(f), -id); /* untrigger sound, etc */

          w->edge_down_ts = 0; /* mark this wave component done */
    } 
    if (w->end_ts && w->end_ts <= rs[f].current_ts) {
          int chan = SW_DOUT_ROUTING(f, wave);
          
          /* Refractory period ended and/or wave is deactivated */
          rs[f].active_wave_mask &= ~(0x1<<wave); /* deactivate the wave */
          w->end_ts = 0; /* mark this wave component done */
          if (chan > -1) /* if it's routed to do digital output,
                            unlock the DOUT channel so the FSM can use it again */
              UNLOCK_DO_CHAN(chan);
    }
  }
}

static void processSchedWavesAO(FSMID_t f)
{  
  unsigned wave_mask = rs[f].valid ? rs[f].active_ao_wave_mask : 0;

  while (wave_mask) {
    unsigned wave = __ffs(wave_mask);
    volatile struct AOWaveINTERNAL *w = FSM_PTR(f)->aowaves[wave];
    wave_mask &= ~(0x1<<wave);
    if (!w) {
        ERROR_INT("FSM %u has AOWave %u marked as active but it has a NULL AOWaveINTERNAL pointer!\n", (unsigned)f, wave);
        continue;
    }
    if (w->cur >= w->nsamples && w->loop) w->cur = 0;
    if (w->cur < w->nsamples) {
      int evt_col = w->evt_cols[w->cur];
      lsampl_t samp = w->samples[w->cur];
      writeAO(w->aoline, samp); /* NB this function does checking to make sure data write is sane */
      if (evt_col > -1 && evt_col <= NUM_IN_EVT_COLS(f))
        set_bit(evt_col, rs[f].events_bits);
      w->cur++;
    } else { /* w->cur >= w->nsamples, so wave ended.. unschedule it. */
      scheduleWaveAO(f, wave, -1);
      rs[f].active_ao_wave_mask &= ~(1<<wave);
    }
  }
}

static void scheduleWave(FSMID_t f, unsigned wave_id, int op)
{
  if (wave_id >= FSM_MAX_SCHED_WAVES)
  {
      DEBUG("Got wave request %d for an otherwise invalid scheduled wave %u!\n", op, wave_id);
      return;
  }
  scheduleWaveDIO(f, wave_id, op);
  scheduleWaveAO(f, wave_id, op);
}

static void scheduleWaveDIO(FSMID_t f, unsigned wave_id, int op)
{
  struct ActiveWave *w;
  const struct SchedWave *s;
  int do_chan;

  if (wave_id >= FSM_MAX_SCHED_WAVES  /* it's an invalid id */
      || !FSM_PTR(f)->sched_waves[wave_id].enabled /* wave def is not valid */ )
    return; /* silently ignore invalid wave.. */
  
  do_chan = SW_DOUT_ROUTING(f, wave_id);
  
  if (op > 0) {
    /* start/trigger a scheduled wave */
    
    if ( rs[f].active_wave_mask & 0x1<<wave_id ) /*  wave is already running! */
    {
        DEBUG("FSM %u Got wave start request %u for an already-active scheduled DIO wave!\n", f, wave_id);
        return;  
    }
    /* ugly casts below are to de-volatile-ify */
    w = &((struct RunState *)&rs[f])->active_wave[wave_id];
    s = &((struct RunState *)&rs[f])->states->sched_waves[wave_id];
    
    w->edge_up_ts = rs[f].current_ts + (int64)s->preamble_us*1000LL;
    w->edge_down_ts = w->edge_up_ts + (int64)s->sustain_us*1000LL;
    w->end_ts = w->edge_down_ts + (int64)s->refraction_us*1000LL;
    rs[f].active_wave_mask |= 0x1<<wave_id; /* set the bit, enable */
    if (do_chan > -1)
        LOCK_DO_CHAN(do_chan); /* lock the digital out chan so the FSM's own 
                                  output column doesn't conflict with it */
    
  } else {
    /* Prematurely stop/cancel an already-running scheduled wave.. */

    if (!(rs[f].active_wave_mask & 0x1<<wave_id) ) /* wave is not running! */
    {
        DEBUG("Got wave stop request %u for an already-inactive scheduled DIO wave!\n", wave_id);
        return;  
    }
    rs[f].active_wave_mask &= ~(0x1<<wave_id); /* clear the bit, disable */
    if (do_chan > -1)
        UNLOCK_DO_CHAN(do_chan);
  }
}

static void scheduleWaveAO(FSMID_t f, unsigned wave_id, int op)
{
  volatile struct AOWaveINTERNAL *w = 0;
  if (wave_id >= FSM_MAX_SCHED_WAVES  /* it's an invalid id */
      || !rs[f].valid
      || !(w = FSM_PTR(f)->aowaves[wave_id])
      || !w->nsamples /* wave def is not valid */ )
    return; /* silently ignore invalid wave.. */
  if (op > 0) {
    /* start/trigger a scheduled AO wave */    
    if ( rs[f].active_ao_wave_mask & 0x1<<wave_id ) /* wave is already running! */
    {
        DEBUG("FSM %u Got wave start request %u for an already-active scheduled AO wave!\n", f, wave_id);
        return;  
    }
    w->cur = 0;
    rs[f].active_ao_wave_mask |= 0x1<<wave_id; /* set the bit, enable */

  } else {
    /* Prematurely stop/cancel an already-running scheduled wave.. */
    if (!(rs[f].active_ao_wave_mask & 0x1<<wave_id) ) /* wave is not running! */
    {
        DEBUG("FSM %u Got wave stop request %u for an already-inactive scheduled AO wave!\n", f, wave_id);
        return;  
    }
    rs[f].active_ao_wave_mask &= ~(0x1<<wave_id); /* clear the bit, disable */
    if (w->nsamples)
      /* write 0 on wave stop */
        writeAO(w->aoline, ao_0V_sample);
    w->cur = 0;
  }
}

static void stopActiveWaves(FSMID_t f) /* called when FSM starts a new trial */
{  
  while (rs[f].active_wave_mask) { 
    unsigned wave = __ffs(rs[f].active_wave_mask);
    int do_chan = -1;
    struct ActiveWave *w = &((struct RunState *)&rs[f])->active_wave[wave];
    rs[f].active_wave_mask &= ~(1<<wave); /* clear bit */
    if (w->edge_down_ts && w->edge_down_ts <= rs[f].current_ts) {
          /* The wave was in the middle of a high, so set the line back low
             (if there actually is a line). */
          int id = do_chan = SW_DOUT_ROUTING(f, wave);

          if (id > -1) 
            dataWrite_NoLocks(id, 0); /* if it's routed to do output, set line back to low. */
          
          id = SW_EXTOUT_ROUTING(f, wave);
          
          if (id > 0) /* if it's routed to play sounds, untrigger sound */
              doExtTrigOutput(DEFAULT_EXT_TRIG_OBJ_NUM(f), -id);
              
    }
    if (do_chan > -1 && w->end_ts && w->end_ts <= rs[f].current_ts)
    /* now, we must unlock the channel if the sched wave was using it*/
        UNLOCK_DO_CHAN(do_chan); /* 'unlock' or release the DO channel */

    memset(w, 0, sizeof(*w));  
  }
  while (rs[f].active_ao_wave_mask) {
    unsigned wave = __ffs(rs[f].active_ao_wave_mask);
    scheduleWaveAO(f, wave, -1); /* should clear bit in rs.active_ao_wave_mask */
    rs[f].active_ao_wave_mask &= ~(0x1<<wave); /* just in case */
  }
}

static void buddyTaskHandler(void *arg)
{
  FSMID_t f = (FSMID_t)(long)arg;
  int req;
  struct ShmMsg *msg = 0;

  if (f >= NUM_STATE_MACHINES) {
    ERROR_INT("buddyTask got invalid fsm id handle %u!\n", f);
    return;
  }
  req = atomic_read(&buddyTaskCmds[f]);
  msg = (struct ShmMsg *)&shm->msg[f];
  switch (req) {
  case FSM: {
    struct FSMSpec *fsm = OTHER_FSM_PTR(f);
    struct EmbC **embc = &fsm->embc;
    const char *name = fsm->name;
    int isok = 1;
    
    /* use alternate FSM as temporary space during this interruptible copy 
       realtime task will swap the pointers when it realizes the copy
       is done and/or when it's time to swap fsms (see pending_fsm_swap stuff)  */
    
    /* first, free alternate FSM */
    unloadDetachFSM(fsm); /* NB: may be a noop if no old fsm exists
                             or if old fsm is of type FSM_TYPE_PLAIN.. */
    /* now copy in the fsm from usersopace */
    memcpy(fsm, (void *)&msg->u.fsm, sizeof(*fsm));  
    *embc = 0;/* make SURE the ptr starts off NULL, this prevents userspace from crashing us.. */
    
    if (fsm->type == FSM_TYPE_EMBC) {
        *embc = (struct EmbC *) mbuff_attach(name, sizeof(**embc));
        
        if (!*embc) {
            ERROR("Failed to attach to FSM Shm %s!\n", name);
            isok = 0;
        } 
    }
    /* set up the error handler */
    if (isok && !initializeFSM(f, fsm)) {
        ERROR("Failed to initialize the FSM %s!\n", name);
        unloadDetachFSM(fsm);
        isok = 0;
    }
    
    if (!isok)
    /* IMPORTANT!! setting the type to NONE indicates to realtime task there was an error.. */
        fsm->type = FSM_TYPE_NONE; 
  }

    break;
  case GETFSM:
    if (!rs[f].valid) {  
      memset((void *)&msg->u.fsm, 0, sizeof(msg->u.fsm));
    } else {
      memcpy((void *)&msg->u.fsm, FSM_PTR(f), sizeof(msg->u.fsm));
    }
    break;
  case GETERROR:
    /*memcpy((void *)msg->u.error, (void *)rs[f].errorbuf, FSM_ERROR_BUF_SIZE);*/
    memset(msg->u.error, 0, FSM_ERROR_BUF_SIZE);
    break;
  case RESET_:
    /* make sure to clean up all fsms */
    unloadDetachFSM(FSM_PTR(f)); 
    unloadDetachFSM(OTHER_FSM_PTR(f));
    initRunState(f);
    break;
  case AOWAVE: {
      struct AOWave *w = &msg->u.aowave;
      if (w->id < FSM_MAX_SCHED_WAVES && w->aoline < NUM_AO_CHANS) {
        volatile struct AOWaveINTERNAL *wint = 0;
        volatile struct FSMSpec *spec;
        if (rs[f].pending_fsm_swap && w->pend_flg)
            spec = OTHER_FSM_PTR(f);
        else 
            spec = FSM_PTR(f);
        if (w->nsamples > AOWAVE_MAX_SAMPLES) w->nsamples = AOWAVE_MAX_SAMPLES;
        wint = spec->aowaves[w->id];
        cleanupAOWave(wint, (char *)spec->name, w->id);
        wint = kmalloc(sizeof(*wint), GFP_KERNEL);
        if (wint && w->nsamples) {
          int ssize = w->nsamples * sizeof(*wint->samples),
              esize = w->nsamples * sizeof(*wint->evt_cols);
          wint->samples = vmalloc(ssize);
          wint->evt_cols = vmalloc(esize);
          if (wint->samples && wint->evt_cols) {
            wint->aoline = w->aoline;
            wint->nsamples = w->nsamples;
            wint->loop = w->loop;
            wint->cur = 0;
            memcpy((void *)(wint->samples), w->samples, ssize);
            memcpy((void *)(wint->evt_cols), w->evt_cols, esize);
            DEBUG("FSM %u AOWave: allocated %d bytes for AOWave %s:%u\n", f, ssize+esize+sizeof(*wint), spec->name, w->id);
          } else {
            ERROR_INT("FSM %u In AOWAVE Buddy Task Handler: failed to allocate memory for an AO wave! Argh!!\n", f);
            cleanupAOWave(wint, (char *)spec->name, w->id);
          }
        } else if (!wint) {
            ERROR_INT("FSM %u In AOWAVE Buddy Task Handler: failed to allocate memory for an AO wave struct! Argh!!\n", f);
        }
        spec->aowaves[w->id] = (struct AOWaveINTERNAL *)wint;
      }
    }
    break;
  case TRANSITIONS: {
        unsigned const from = msg->u.transitions.from; /* Shorthand alias.. */
        unsigned const num = msg->u.transitions.num; /* alias.. */
        unsigned i;

        for (i = 0; i < num; ++i)
            memcpy((void *)&msg->u.transitions.transitions[i],
                    (const void *)historyAt(f, from + i),
                    sizeof(struct StateTransition));
    }
    break;

  case LOGITEMS: {
        unsigned const from = msg->u.log_items.from; /* Shorthand alias.. */
        unsigned const num = msg->u.log_items.num; /* alias.. */
        unsigned i;

        for (i = 0; i < num; ++i)
            memcpy((void *)&msg->u.log_items.items[i],
                    (const void *)logItemAt(f, from + i),
                    sizeof(struct VarLogItem));
    }
    break;

  } /* end switch */

  /* indicate to RT task that request is done.. */
  atomic_set(&buddyTaskCmds[f], -req);
}

static void buddyTaskComediHandler(void *arg)
{
  int err;
  long flags = (long)arg;
  
  err = setupComediCmd();    
  if (err)
    ERROR("comediCallback: failed to restart acquisition after COMEDI_CB_OVERFLOW %d: error: %d!\n", ai_n_overflows, err);
  else if (flags)
    LOG_MSG("comediCallback: restarted acquisition after %d overflows.\n", ai_n_overflows);
}

static inline long long timespec_to_nano(const struct timespec *ts)
{
  return ((long long)ts->tv_sec) * 1000000000LL + (long long)ts->tv_nsec;
}

static void cleanupAOWaves(volatile struct FSMSpec *spec)
{
  unsigned i;
  for (i = 0; i < FSM_MAX_SCHED_WAVES; ++i) {
    cleanupAOWave(spec->aowaves[i], (char *)spec->name, i); /* ok if null, cleanup func just returns then */
    spec->aowaves[i] = 0;
  }
}

static void cleanupAOWave(volatile struct AOWaveINTERNAL *wint, const char *name, int bufnum)
{
  int freed = 0, nsamps = 0;
  if (!wint) return;
  nsamps = wint->nsamples;
  wint->loop = wint->cur = wint->nsamples = 0; /* make fsm not use this? */
  mb();
  if (wint->samples) vfree(wint->samples), wint->samples = 0, freed += nsamps*sizeof(*wint->samples);
  if (wint->evt_cols) vfree(wint->evt_cols), wint->evt_cols = 0, freed += nsamps*sizeof(*wint->evt_cols);
  if (wint) kfree((void *)wint), freed += sizeof(*wint);
  if (freed) 
    DEBUG("AOWave: freed %d bytes for AOWave wave buffer %s:%d\n", freed, name, bufnum);
}

static void updateHasSchedWaves(FSMID_t f)
{
	unsigned i;
	int yesno = 0;
	for (i = 0; i < NUM_OUT_COLS(f); ++i)
      yesno = yesno || (OUTPUT_ROUTING(f,i)->type == OSPEC_SCHED_WAVE);  	
	FSM_PTR(f)->has_sched_waves = yesno;
}

static void updateDefaultExtObjNum(FSMID_t f)
{
    unsigned i;
    
    FSM_PTR(f)->default_ext_trig_obj_num = -1;
    
    for (i = 0; i < NUM_OUT_COLS(f); ++i) {
        struct OutputSpec *spec = OUTPUT_ROUTING(f,i);
        if ( spec->type == OSPEC_EXT ) {
            FSM_PTR(f)->default_ext_trig_obj_num = spec->object_num;
            break;
        }
    }
}

static void fsmLinkProgram(FSMID_t f, struct EmbC *embc)
{
  volatile const struct RunState *r = &rs[f];

 /*  'link' symbols.. see Embedded_C_FSM_Notes.txt or EmbC.h 
     to see which pointers need to be added.. */
  
  embc->time = &r->current_ts_secs;
  embc->state = &r->current_state;
  embc->ready_for_trial = &r->ready_for_trial_flg;
  embc->transitions = &r->history.num_transitions;
  embc->cycle = &cycle;
  embc->rate = (unsigned *)&task_rate;
  embc->fsm = &r->id;
  embc->transition = (struct EmbCTransition *)&r->last_transition;
  embc->rand = &emblib_rand;
  embc->randNormalized = &emblib_randNormalized;
  embc->logValue = &emblib_logValue;
  embc->logArray = &emblib_logArray;
  embc->logGetAt = &emblib_logGetAt;
  embc->logFind = &emblib_logFind;
  embc->logGetSize = &emblib_logGetSize;
  embc->readAI = &emblib_readAI;
  embc->writeAO = &emblib_writeAO;
  embc->readDIO = &emblib_readDIO;
  embc->writeDIO = &emblib_writeDIO;
  embc->bypassDOut = &emblib_bypassDOut;
  embc->forceJumpToState = &emblib_gotoState;
  embc->printf = &emblib_printf;
  embc->snprintf = &emblib_snprintf;
  embc->sendPacket = &emblib_sendPacket;
  embc->trigExt = &emblib_trigExt;
  embc->trigSchedWave = &emblib_trigSchedWave;
  embc->sqrt = &sqrt;
  embc->exp = &exp;
  embc->exp2 = &exp2;
  embc->exp10 = &exp10;
  embc->pow = &pow;
  embc->log = &log;
  embc->log2 = &log2;
  embc->log10 = &log10;
  embc->sin = &sin;
  embc->cos = &cos;
  embc->tan = &tan;
  embc->atan = &atan;
  embc->round = &round;
  embc->ceil = &ceil;
  embc->floor = &floor;
  embc->fabs = &fabs;
  embc->acosh = &acosh;
  embc->asin = &asin;
  embc->asinh = &asinh;
  embc->atanh = &atanh;
  embc->cosh = &cosh;
  embc->expn = &expn;
  embc->fac = &fac;
  embc->gamma = &gamma;
  embc->isnan = &isnan;
  embc->powi = &powi;
  embc->sinh = &sinh;
  embc->tanh = &tanh;

}

/** Return nonzero on error */
static int initializeFSM(FSMID_t f, struct FSMSpec *spec)
{
  struct EmbC *embc = spec->embc;

  if (embc) {
    embc->lock(); /* NB: unlock called by unloadDetachFSM */

    fsmLinkProgram(f, embc);
  }
  
  /* clear all AO wave pointers */
  memset(spec->aowaves, 0, sizeof(spec->aowaves));
 
  /* call init here in non-realtime context.  Note that cleanup is called in non-realtime context too in unloadDetachFSM() */
  if (embc && embc->init) embc->init(); 
  
  return 1;
}

static double emblib_rand(void) 
{
  u32 rand_num = random32();
  return ((double)rand_num) / ((double)(0xffffffffU)) * 1.0;
}

static double emblib_randNormalized(void)
{
  static double last;
  static int use_last = 0;
  double ret;
  
  if (use_last) {
      ret = last;
      use_last = 0;
  } else {
    double fac, rsq, v1, v2;
    //Now make the Box-Muller transformation to get two normal deviates. 
    do {
            v1=2.0*emblib_rand()-1.0; 
            v2=2.0*emblib_rand()-1.0; 
            rsq=v1*v1+v2*v2;
    } while (rsq >= 1.0 || rsq == 0.0); 
    fac = sqrt( (-2.0*log(rsq)) / rsq );
    ret = v2*fac;
    // cache a copy of the variable for next call
    last = v1*fac;
    use_last = 1;
  }
  return ret; 
}

static void emblib_logValue(unsigned f, const char * nam, double v)
{
  if (f < NUM_STATE_MACHINES) logItemPush(f, nam, v);
}

static void emblib_logArray(unsigned f, const char *nam, const double *v, unsigned n)
{
  if (f < NUM_STATE_MACHINES) {
    unsigned i;
    for (i = 0; i < n; ++i) {
      char nbuf[FSM_MAX_SYMBOL_SIZE+11];
      snprintf(nbuf, sizeof(nbuf), "%s%u", nam, i);
      emblib_logValue(f, nbuf, v[i]);
      ++i;
    }
  }
}
static unsigned emblib_logGetSize(unsigned f)
{
    if (f < NUM_STATE_MACHINES)
        return NUM_LOG_ITEMS(f);
    return 0;
}
static const EmbCLogItem * emblib_logGetAt(unsigned f, unsigned pos)
{
    static EmbCLogItem dummy = { .ts = 0., .name = {0}, .value = 0. };
    if (f < NUM_STATE_MACHINES)
        return (EmbCLogItem *)logItemAt(f, pos);
    return &dummy;
}
static const EmbCLogItem * emblib_logFind(unsigned f, const char *name, unsigned N)
{
    int i;
    const EmbCLogItem *item;
    static EmbCLogItem dummy = { .ts = 0., .name = {0}, .value = 0. };
    if (N > emblib_logGetSize(f)) N = emblib_logGetSize(f);
    if (N > MAX_HISTORY) N = MAX_HISTORY;
    for (i = emblib_logGetSize(f)-1; N && i > -1; --i, --N)
        if ( (item = emblib_logGetAt(f, i)) && !strcasecmp(name, item->name) )
            return item;
    return &dummy;
}

static double emblib_readAI(unsigned chan)
{
    if (readAI(chan)) /* note readAI() is caching and may not be slow at all.. */
        return ai_samples_volts[chan];
    /* else.. */
    WARNING("The embedded-C code got an error in emblib_getAI() for channel %u\n", chan);
    return 0.0; /* what to do on error? */
}
static int emblib_writeAO(unsigned chan, double volts)
{
    return writeAO(chan, VOLTS_TO_SAMPLE_AO(volts));
}
static int emblib_readDIO(unsigned chan)
{
    unsigned bitpos = 0x1<<chan;
    if ( bitpos & di_chans_in_use_mask ) 
        return  (dio_bits & bitpos) ? 1 : 0;
    /* otherwise channel is invalid or not configued for input, return error */
    return -1;
}
static int emblib_writeDIO(unsigned chan, unsigned val)
{
    unsigned bitpos = 0x1<<chan;
    if ( bitpos & do_chans_in_use_mask ) {
        dataWrite_NoLocks(chan, val);
        return 1;
    }
    return 0;
}
static int emblib_bypassDOut(unsigned f, unsigned mask)
{
    return bypassDOut(f, mask);
}

static int emblib_gotoState(uint fsm, unsigned state, int eventid)
{
    if (fsm < NUM_STATE_MACHINES) {
        int ret = gotoState(fsm, state, eventid); /* NB: gotoState() returns 1 on new state, 0 on same state, -1 on error */
        return ret > -1; 
    }
    return 0;
}

static int emblib_printf(const char *format, ...) 
{
    char buf[256]; /* we can't make this too big since it may break stack.. */
    int ret;
    va_list ap;

    buf[0] = 0;
    va_start(ap, format);
    ret = float_vsnprintf(buf, sizeof(buf), format, ap);/* first, format floats into buf, %d %x etc are copied verbatim from format! */
    va_end(ap);
    if (ret > -1)
    rt_printk("%s", buf); /* finally, really print it using rt-safe printf */
    return ret;
}

static int emblib_snprintf(char *buf, unsigned long sz, const char *format, ...)
{
    int ret;
    va_list ap;
    
    va_start(ap, format);
    ret = float_vsnprintf(buf, sz, format, ap);
    va_end(ap);
    return ret;
}

static void emblib_sendPacket(unsigned fsm, int proto, const char *host, unsigned short port, const void *data, unsigned len)
{
    if (fsm < NUM_STATE_MACHINES && (proto == EMBC_TCP || proto == EMBC_UDP) && data && host) {
        doNRTOutput(
            fsm, /* fsm id */
            (proto == EMBC_TCP ? NRT_TCP : NRT_UDP) | NRT_BINDATA,  /* type -- make sure it's marked as binary */
            0xff,  /* col */
            0xff, /* trig */
            host,
            port,
            data, /* data */
            len /*datalen */
        );
    }
}
static void emblib_trigSchedWave(unsigned fsm_id, unsigned sched_wave_bits)
{
    if (fsm_id < NUM_STATE_MACHINES)
        doSchedWaveOutput(fsm_id, sched_wave_bits);
}
static void emblib_trigExt(unsigned which, unsigned trig)
{
    doExtTrigOutput(which, trig);
}

static void swapFSMs(FSMID_t f)
{
  stopActiveWaves(f); /* since we are starting a new trial, force any
                         active timer waves to abort! */ 
  rs[f].states = OTHER_FSM_PTR(f); /* swap in the new fsm.. */
  /*LOG_MSG("Cycle: %lu  Got new FSM\n", (unsigned long)cycle);*/
  updateHasSchedWaves(f); /* just updates rs.states->has_sched_waves flag*/
  updateDefaultExtObjNum(f); /* just updates rs.states->default_ext_obj_num */
  reconfigureIO(); /* to have new routing take effect.. */
  rs[f].valid = 1; /* Unlock FSM.. */
  rs[f].pending_fsm_swap = 0; 
  if (rs[f].current_state >= NUM_ROWS(f))
    gotoState(f, 0, -1); /* force a state 0 if the new fsm doesn't contain current_state!! */
}

static void unloadDetachFSM(struct FSMSpec *fsm)
{
    if (fsm->type == FSM_TYPE_EMBC) {
        struct EmbC **embc = &fsm->embc;
        char *argv[] = {
          "/sbin/rmmod",
          (char *)fsm->name,
          0
        };
        char *envp[] = {
          "HOME=/",
          "TERM=linux",
          "PATH=/sbin:/usr/sbin:/bin:/usr/bin",
          NULL,
        };        
        if (*embc) {
      
            if ((*embc)->cleanup) 
                (*embc)->cleanup();
      
            /* decrease use count .. was incremented by fsmLinkProgram */
            (*embc)->unlock(); 
      
            mbuff_detach(fsm->name, *embc);
            *embc = 0; /* clean up old embc ptr, if any */
            
        }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
        call_usermodehelper(argv[0], argv, envp, 1);
#else
        call_usermodehelper(argv[0], argv, envp);
#endif
    } 
    
    /* NB: don't make this an else of the above if.. we call this 
       unconditionally as long as fsm->type != FSM_TYPE_NONE */
    
    if (fsm->type != FSM_TYPE_NONE) {
      /* now, free any allocated ao wav data */
      cleanupAOWaves(fsm);
    }
    
    memset(fsm, 0, sizeof(*fsm)); /* clear the fsmspec just to be paranoid.. */
}

static void  doSetupThatNeededFloatingPoint(void)
{
  int i, nchans = comedi_get_n_channels(dev_ao, subdev_ao);
  
  ao_range_min_v = ((double)ao_krange.min) * 1e-6;
  ao_range_max_v = ((double)ao_krange.max) * 1e-6;
  ao_0V_sample = VOLTS_TO_SAMPLE_AO(0);
  
  for (i = 0; i < nchans; ++i) 
    /* initialize all AO chans to 0V */
    writeAO(i, ao_0V_sample);
  
  ai_range_min_v = ((double)ai_krange.min) * 1e-6;
  ai_range_max_v = ((double)ai_krange.max) * 1e-6;
  
  for (i = 0; i < MAX_AI_CHANS; ++i)
      ai_samples_volts[i] = 0.0;
}

static void tallyJitterStats(hrtime_t real_wakeup, hrtime_t ideal_wakeup, hrtime_t task_period_ns)
{
   hrtime_t quot;
   const int n = cycle > 100 ? 100 : ( cycle ? cycle : 1 );
   
   lat_cur = real_wakeup - ideal_wakeup; 
   
   /* see if we woke up jittery/late.. if latency is > JITTER_TOLERANCE_NS *and* it's > 10% our task cycle.
      The second requirement is to avoid stupid reporting when the task cycle is like really large, 
      like on the order of ms*/
    if ( ABS(lat_cur) > JITTER_TOLERANCE_NS && ABS(lat_cur)*10 >= task_period_ns) {
      ++fsm_wakeup_jittered_ct;
      WARNING("Jittery wakeup! Magnitude: %s ns (cycle #%s)\n", int64_to_cstr(lat_cur), cycleStr());
    }
    if (lat_cur > lat_max) lat_max = lat_cur;
    if (lat_cur < lat_min) lat_min = lat_cur;
    quot = lat_avg * (n-1);
    quot += lat_cur;
    do_div(quot, n);
    lat_avg = quot;
}

static void tallyCbEosCycStats(hrtime_t cyc_time)
{
   hrtime_t quot;
   const unsigned long n = ++cb_eos_cyc_ct > 100 ? 100 : ( cb_eos_cyc_ct ? cb_eos_cyc_ct : 1 );
   
    if (cyc_time > cb_eos_cyc_max) cb_eos_cyc_max = cyc_time;
    if (cyc_time < cb_eos_cyc_min) cb_eos_cyc_min = cyc_time;
    quot = cb_eos_cyc_avg * (n-1);
    quot += cyc_time;
    do_div(quot, n);
    cb_eos_cyc_avg = quot;
}

static char *cycleStr(void)
{
  static uint64 last = ~0ULL;
  static char buf[21];
  
  if (last != cycle) {
      last = cycle;
      uint64_to_cstr_r(buf, sizeof(buf), last);
  }
  return buf;
}

static void tallyCycleTime(hrtime_t cycleT0, hrtime_t cycleTf)
{
    int64 quot;
    const int n = cycle > 100 ? 100 : ( cycle ? cycle : 1 );
    
    cyc_cur = cycleTf-cycleT0;
    
    if ( cyc_cur + 1000LL > task_period_ns) {
        WARNING("Cycle %s took %s ns (task period is %lu ns)!\n", cycleStr(), int64_to_cstr(cyc_cur), (unsigned long)task_period_ns);
        ++fsm_cycle_long_ct;
    }
    if (cyc_cur > cyc_max) cyc_max = cyc_cur;
    if (cyc_cur < cyc_min) cyc_min = cyc_cur;
    quot = cyc_avg * (n-1);
    quot += cyc_cur;
    do_div(quot, n);
    cyc_avg = quot;
}



static void do_dio_bitfield(unsigned chan_mask_in, unsigned *bits_in)
{
    unsigned chan_mask, bits, i, chans_done = 0, n_chans;
    int ret;
    
    for (i = 0; i < NUM_DIO_SUBDEVS && chans_done < NUM_DIO_CHANS; ++i) {
        n_chans =  n_chans_dio[i];
        chan_mask = (chan_mask_in>>chans_done) & ((0x1<<n_chans)-1);
        bits = (*bits_in >> chans_done) & ((0x1<<n_chans)-1);
        DEBUG_CRAZY("comedi_dio_bitfield(dev, %u, %x, %x) = ", subdev[i], chan_mask, bits);
        ret = comedi_dio_bitfield(dev, subdev[i], chan_mask, &bits);
        if (debug > 2) rt_printk("%d\n", ret);
        *bits_in &= ~((0x1<<n_chans)-1) << chans_done; /* clear output bits */
        *bits_in |= (bits & ((0x1<<n_chans)-1)) << chans_done; /* set them from comedi_dio_bitfield cmd */
        chans_done += n_chans;
    }
}

static int do_dio_config(unsigned chan, int mode)
{
    unsigned i;
    
    for (i = 0; i < NUM_DIO_SUBDEVS; ++i) {
        if (chan < n_chans_dio[i]) {
            int ret;
            DEBUG_CRAZY("comedi_dio_config(dev, %u, %x, %x) = ", subdev[i], chan, mode);
            ret = comedi_dio_config(dev, subdev[i], chan, mode);
            if (debug > 2) rt_printk("%d\n", ret);
            return ret;
        }
        chan -= n_chans_dio[i];
    }
    return 0;
}
