#ifndef FSM_H
#  define FSM_H

#include "IntTypes.h" // for int8 int16 int32, etc

#ifdef __cplusplus
extern "C" {
#endif

  /** The amount of memory to preallocate in Kernel space for the FSM program. 
      This is generated and user-specific C-code, essentially that fits in this
      large 512KB string..

      It's set to 512KB, but can be tweaked by changing this define.

      FIXME:
      Note that the FSM can't have arbitrarily many rows due to limitations
      in struct StateTransition which stores the previous and next states 
      in an unsigned short to conserve memory (we keep the last million state 
      transitions in RAM!) */
#define FSM_MEMORY_BYTES (1024*512)
#define FSM_PROGRAM_SIZE (FSM_MEMORY_BYTES*4/5)
#define FSM_MATRIX_SIZE (FSM_MEMORY_BYTES/5)
#define FSM_PLAIN_MATRIX_MAX (FSM_MEMORY_BYTES/sizeof(unsigned))
#define FSM_MAX_SYMBOL_SIZE 32 /**< the largest possible size of the name of
                                  a variable in an FSM program */
#define FSM_MAX_SCHED_WAVES (sizeof(unsigned)*8)
#define FSM_MAX_IN_CHANS 32
#define FSM_MAX_OUT_CHANS 32
#define FSM_MAX_IN_EVENTS (FSM_MAX_IN_CHANS*2)
#define FSM_MAX_OUT_EVENTS 16 /* this should be enough, right? */
#define FSM_ERROR_BUF_SIZE FSM_MEMORY_BYTES

struct SchedWave
{
  int enabled; /**< Iff true, actually use this sched_wave.  Otherwise it's
                  considered invalid and ignored. */

  unsigned preamble_us; /**< the amount of time from when it is triggered
                           to when the scheduled waveform goes high, in us. */
  unsigned sustain_us;  /**< the amount of time from when the waveform fires
                           high to when it goes low again, in us */
  unsigned refraction_us; /**< the blanking time for the scheduled waveform.
                              The amount of time after it goes low again
                              until it can be triggered normally again.
                              It cannot fire or be triggered during the 
                              refractory period -- instead a triggered wave
                              during this time will *not* fire at all!!

                              refraction_us_remaining + preamble_us after the
                              trigger.  Not sure how useful this is.. but
                              here it is. */
};

enum { OSPEC_DOUT = 1, OSPEC_TRIG, OSPEC_EXT, OSPEC_SCHED_WAVE, OSPEC_TCP, OSPEC_UDP, OSPEC_NOOP = 0x7f };

#define OUTPUT_SPEC_DATA_SIZE 1024
#define IP_HOST_LEN 80
#define FMT_TEXT_LEN (OUTPUT_SPEC_DATA_SIZE-sizeof(unsigned short)-IP_HOST_LEN)
struct OutputSpec
{
  int type; /* one of the OSPEC_ enum above */
  union {
    char data[OUTPUT_SPEC_DATA_SIZE]; /* for other misc types */
    struct { /* for OSPEC_DOUT and OSPEC_TRIG types */
      unsigned from; 
      unsigned to;
    };
    struct { /* for OSPEC_EXT */
      unsigned object_num;
    };
    struct {     /* for OSPEC_TCP or OSPEC_UDP */
      char host[IP_HOST_LEN];
      unsigned short port;
      char fmt_text[FMT_TEXT_LEN];
    };
  };
};

struct EmbC;
struct AOWaveINTERNAL; /**< opaque here */

/** For FSMSpec::Routing::in_chan_type field */
enum { DIO_TYPE = 0, AI_TYPE, UNKNOWN_TYPE };

/** For FSMSpec::type field */
enum { FSM_TYPE_NONE = 0, FSM_TYPE_PLAIN = 0x22, FSM_TYPE_EMBC = 0xf3 };

/** A structure encapsulating the entire fsm specification.  
    One of these gets sent to RT from userspace per 
    'SetStateMatrix' or 'SetStateProgram' call.  */
struct FSMSpec
{
  unsigned short n_rows; /* Corresponds to number of states... */
  unsigned short n_cols; /* Corresponds to variable number of input events plus
                            4 of the fixed columns at the end plus possibly 1
                            column for the SCHED_WAVE column at the end
                            if present. */
  int type; /**< one of FSM_TYPE_PLAIN, FSM_TYPE_EMBC, etc */
  
  char name[64]; /**< The name of the FSM kernel module's shm to attach if 
                      FSM_TYPE_EMBC, or just a descriptive name of the fsm
                      otherwise.. */
#ifdef EMULATOR
  void *handle; /**< Handle to the dlopen()/dlclose() calls */
#endif

  /** which struct in the union we use depends on the 'type' field above */
  union {
    /** Struct only used if type is FSM_TYPE_EMBC */
    struct {
      char program_z[FSM_PROGRAM_SIZE]; /**< A *zlib deflated*, 
                                            NUL-terminated huge string of the
                                            generated + user-specified fsm 
                                            program */
      unsigned program_len; /**< Number of bytes of uncompressed program 
                                 string, uncluding NUL. */
      unsigned program_z_len; /**< Number of bytes of compressed program data 
                                   -- basically number of valid bytes in program_z. */
      char matrix_z[FSM_MATRIX_SIZE]; /**< A *zlib deflated* string version
                                           of the matrix only.  This is for 
                                           reference and not used by the FSM at 
                                           all.  
                                           The format of this string is 
                                           URLEncoded lines, each line 
                                           representing  a matrix row, delimited 
                                           by whitespace. */
      unsigned matrix_len; /**< Number of bytes of uncompressed matrix string,
                                including NUL. */
      unsigned matrix_z_len; /**< Number of bytes of compressed matrix data,
                                  basically number of bytes in matrix_z. */
    } program;
    /** Struct only used if type is FSM_TYPE_PLAIN */
    struct {
      unsigned matrix[FSM_PLAIN_MATRIX_MAX]; /**< Raw matrix data as a linear 
                                                  array -- FSM code knows how 
                                                  to interpret this as a 2D 
                                                  array based on n_rows and 
                                                  n_cols above */
    } plain;
  }; 
  unsigned short ready_for_trial_jumpstate; /**< normally always 35 */
  /**< if true, uploading a new state matrix does not take effect immediately,
     but instead, the new FSM only takes effect when the old FSM
     jumps to state 0.  This is so that all FSMs have a well-defined
     entry point.  */
  unsigned short wait_for_jump_to_state_0_to_swap_fsm;


  /** The scheduled wave specifications.  @see struct SchedWave */
  struct SchedWave sched_waves[FSM_MAX_SCHED_WAVES];

  /** Cached flag for scheduled waves. */
  unsigned has_sched_waves;
  
  /** Cached 'default' ext trig object num.. used by sched waves to figure out which ext trig object num
      to trigger if sched_wave_extout > 0 for the sched wave.. */
  int default_ext_trig_obj_num;

  /** Struct to describe routing mappings from input/output channel id's
      to physical channel id's.  */
  struct Routing {
    unsigned in_chan_type; /** either AI_TYPE, DIO_TYPE, or UNKNOWN_TYPE */

    unsigned num_in_chans;  /** <= FSM_MAX_IN_CHANS                      */
    unsigned first_in_chan; /** <= FSM_MAX_IN_CHANS                      */
    unsigned num_evt_cols;  /** <= FSM_MAX_IN_CHANS*2                    */

    /** Associative array of phys_in_chan_id*2+(edge down?1:0) -> matrix 
        column */
    int input_routing[FSM_MAX_IN_EVENTS];
    /** Map of sched_wave_id*2+(edge down ? 1 : 0) -> matrix column  
        or -1 for none */
    int sched_wave_input[FSM_MAX_SCHED_WAVES*2];
    /** Map of sched_wave_id -> output channel id or -1 for none */
    int sched_wave_dout[FSM_MAX_SCHED_WAVES];
    /** Map of sched_wave_id -> ext (sound) to trigger/untrigger or 0 for none */
    unsigned sched_wave_extout[FSM_MAX_SCHED_WAVES];

    unsigned num_out_cols; /**< Always <= FSM_MAX_OUT_EVENTS -- defines valid
                                elements in below array                   */
    /** Defines the meaning of an output column */
    struct OutputSpec output_routing[FSM_MAX_OUT_EVENTS];
  } routing;

  /** This is only set inside kernel: RealtimeFSM kernel module uses it to 
      point to compiled code. However, if FSMSpec::type field is not 
      FSM_TYPE_EMBC, this should always be NULL */
  struct EmbC *embc;
  
  /** Array of pointers to allocated scheduled waves.  A NULL ptr indicates not allocated/used */
  struct AOWaveINTERNAL *aowaves[FSM_MAX_SCHED_WAVES];
};

#ifndef __cplusplus
typedef struct FSMSpec FSMSpec;
#endif

#define FSM_NUM_INPUT_COLS(_fsm) ((_fsm)->routing.num_evt_cols)
#define FSM_NUM_OUTPUT_COLS(_fsm) ((_fsm)->routing.num_out_cols)
#define TIMEOUT_STATE_COL(_fsm) (FSM_NUM_INPUT_COLS(_fsm))
#define TIMEOUT_TIME_COL(_fsm) (TIMEOUT_STATE_COL(_fsm)+1)
#define FIRST_OUT_COL(_fsm) (TIMEOUT_STATE_COL(_fsm)+2)

/** Access the matrix array for a row or col. f is a FSMSpec *, r is row, c is column
    Note this is RAW access and so calling code should make sure: 
    1. this is an FSMSpec with type field FSM_TYPE_PLAIN
    2. that r and c are within FSMSpec::n_rows and FSMSpec::n_cols! */
#define FSM_MATRIX_AT(_f,_r,_c) ((_f)->plain.matrix[(_r)*((_f)->n_cols)+(_c)])

/** NB: all timestamps are in microseconds and are relative to each other   */
struct StateTransition 
{   
  long long ts; /* When we entered the state, in nanoseconds since FSM reset. */
  long long ext_ts; /* Nanosecond timestamp from external reference clock */
  unsigned short previous_state;
  unsigned short state; /* The state that was entered via this transition.   */
  signed char event_id; /* The event_id that *led* to this state.  An event_id
                           is basically a column position.                   */
};

/** A log item that gets returned from LOGITEMS FSM command */
struct VarLogItem
{
  double ts; /* Timestamp, in seconds */
  char name[FSM_MAX_SYMBOL_SIZE+1]; /* The name of the C variable */
  double value; /* The value of the C variable */
};

#define AOWAVE_MEMORY_BYTES (FSM_MEMORY_BYTES/2)
#define AOWAVE_MAX_SAMPLES (AOWAVE_MEMORY_BYTES/sizeof(unsigned short))
struct AOWave
{
  unsigned id;
  unsigned aoline;
  unsigned nsamples;
  unsigned loop; /* if true, playing will loop until untriggered */
  unsigned pend_flg; /* if true, apply this AOWave to the pending fsm that has yet to be swapped-in */
  unsigned short samples[AOWAVE_MAX_SAMPLES]; /** samples already scaled to 
                                                  ai_maxdata */
  signed char evt_cols[AOWAVE_MAX_SAMPLES]; /**< the state matrix
                                               column for event
                                               generation when
                                               corresponding sample is
                                               played, -1 means don't
                                               generate event for
                                               corresponding sample */
};


/** Simulated Input Events -- used for fakerat, etc */
struct SimInpEvt
{
    int event_id; /* event column id, where -1 means a timeout event! */
    long long ts; /* ts in nanos */
};
#ifndef __cplusplus
typedef struct SimInpEvt SimInpEvt;
#endif

#define MAX_SIM_INP_EVTS 8192

#define FSM_EVT_TYPE_IN 0

/** FSMEvent -- used with the GETSTIMULI FSM message type */
struct FSMEvent
{
    short type; /* One of OSPEC_* for output events, or 0 for input */
    short id;   /* Corresponds to FSM column id for the event, 0 indexed */
    int val;    /*  For output events, corresponds to the value outputted
                    For input events, corresponds to the state we jumped to */
    long long ts; /* timestamp in nanos in FSM time */
};
#ifndef __cplusplus
typedef struct FSMEvent FSMEvent;
#endif

enum ShmMsgID 
{
    GETPAUSE = 1,  /* Query the FSM to find out if it is paused. */
    PAUSEUNPAUSE, /* Halt the state machine (temporarily).  No variables
                     are cleared but events cannot generate new
                     states or digital outputs. */
    RESET_, /* Reset/Initialize the state machine.  All variables are cleared
                and the state matrix is cleared. Equivalent to matlab
                function Initialize(m). NB: RESET is a #define in RTAI so we call it RESET_ */
    GETVALID,  /* Query the FSM to find out if it has a valid state machine
                  specification. */
    INVALIDATE, /* Invalidate (clear) the state machine specification, 
                   but preserve other system variables. */
    FSM, /* Load an FSM. */
    GETFSM,
    GETFSMSIZE, /* Query the rows,cols of the FSM Matrix. */
    FORCEEVENT, /* Force a particular event to have occurred. */
    FORCETIMESUP, /* Force a time's up event to have occurred. */
    FORCEEXT,     /* Force the FSM to do some ext outputs for this tick*/
    FORCEOUTPUT, /* Specify (or clear) the set of channels that are set to
                     'always on'. */
    TRANSITIONS, /* Query about past state transitions (EventIDs in matlab
                    parlance.. */
    TRANSITIONCOUNT, /* Get a count of the number of transitions that have
                        taken place since the last RESET. */
    GETRUNTIME, /* Returns the time, in microseconds, since the last reset. */
    READYFORTRIAL, /* Unimplemented for now.. */
    GETCURRENTSTATE, /* Query the state we are currently in.. */
    FORCESTATE, /* Force the FSM to jump to a specific state, usually 0 or 35 */
    GETNUMINPUTEVENTS, /* Ask the FSM to tell us how many input columns
                          there are. */
    STARTDAQ, /* Do data acquisition. */
    STOPDAQ, /* Stop data acquisition. */
    GETAOMAXDATA, /* query FSM to find out the maxdata value for its
                     AO channels, a precursor to uploading a correct AO wave
                     to kernel */
    AOWAVE, /* set/clear an existing AO wave */
    LOGITEMS, /* get the log items */
    LOGITEMCOUNT, /* get the log item count */
    GETERROR, /* get the alst fsm error -- use this to see if
                 an FSMSpec was accepted and if not, what was wrong with it */
    SETAIMODE, /* reset acquisition mode to either asynch or synch */
    GETAIMODE, /* query the current acquisition mode */
    GETSIMINPEVTS, /* query the simulated input events */
    ENQSIMINPEVTS, /* enqueue some simulated input events */
    CLEARSIMINPEVTS, /* clear the simulated input events */
    GETSTIMULI,  /** Returns a FSMEvents in the [from,to] range. */
    STIMULICOUNT, /** Returns the number of FSMEvents in the from,to range */
#ifdef EMULATOR
    GETCLOCKLATCHMS, /* query fsm clock latch amount */
    SETCLOCKLATCHMS, /* turn fsm clock latching on and set it to x MS */
    CLOCKLATCHPING, /* ping/reset the clock latch countdown */
    CLOCKISLATCHED, /* returns true iff the fsm is not advancing due to 
                       its clock being latched */
    FASTCLOCK,        /* Set/Clear the 'fast clock' flag for the emulator --
                       fast clock FSMs run the emulated time as fast as 
                       possible without sleeps in the 
                       clock_wait_next_period() function.
                       See kernel_emul.cpp */
    ISFASTCLOCK,     /* Query the status of the 'fast clock' flag */
#endif
    LAST_SHM_MSG_ID
};

struct ShmMsg {
    int id; /* One of ShmMsgID above.. */
  
    /* Which element of union is used depends on id above. */
    union {

      /* For id == FSM */
      struct FSMSpec fsm;

      /* For id == TRANSITIONS */
      struct {
        unsigned num;
        unsigned from; 
#define MSG_MAX_TRANSITIONS 64
        struct StateTransition transitions[MSG_MAX_TRANSITIONS];
      } transitions;

      /* For id == LOGITEMS */
      struct {
        unsigned num;
        unsigned from;
#define MSG_MAX_LOG_ITEMS 64
        struct VarLogItem items[MSG_MAX_TRANSITIONS];
      } log_items;

      /* For id == TRANSITIONCOUNT */
      unsigned transition_count;

      /* For id == LOGITEMCOUNT */
      unsigned log_item_count;

      /* For id == GETPAUSE */
      unsigned is_paused; 

      /* For id == GETVALID */
      unsigned is_valid;

      /* For id == FORCEEVENT */
      unsigned forced_event;

      /* For id == FORCETRIGGER */
      unsigned forced_triggers; /* Bitarray of ABSOLUTE channel id's.. */
      
      /* For id == FORCEOUTPUT */
      unsigned forced_outputs; /* Bitarray of ABSOLUTE channel id's */

      /* For id == GETRUNTIME */ 
      long long runtime_us; /* Time since last reset, in micro-seconds */

      /* For id == GETCURRENTSTATE */ 
      unsigned current_state; /* The current state */

      /* For id == FORCESTATE */
      unsigned forced_state;  /* The forced state */
    
      /* For id == GETFSMSIZE */
      unsigned short fsm_size[2]; /* [0] == rows, [1] == cols */

      /* For id == GETNUMINPUTEVENTS */
      unsigned num_input_events;
      
      /* For id == STARTDAQ */
      struct { 
        unsigned chan_mask; /**< Mask of channel id's from 0-31. */
        int range_min; /**< Range min in fixed point -- divide by 1e6 for V*/
        int range_max; /**< Range min in fixed point -- divide by 1e6 for V*/
        int started_ok; /**< Reply from RT to indicate STARTDAQ was accepted*/
        unsigned maxdata;
      } start_daq;

      /* For id == GETAOMAXDATA */
      unsigned short ao_maxdata;

      /* For id == AOWAVE */
      struct AOWave aowave;

      /* For id == GETERROR -- descriptive text of last FSM error, 
         or empty string if no errors  */
      char error[FSM_ERROR_BUF_SIZE];
      
      /* For id == SETAIMODE and GETAIMODE */
      unsigned ai_mode_is_asynch;

      struct {
          SimInpEvt evts[MAX_SIM_INP_EVTS];
          unsigned num;
#ifdef EMULATOR
          long long ts_for_clock_latch;
#endif
      } sim_inp;

        struct {
            unsigned from, num;
#define MSG_MAX_EVENTS 64
            FSMEvent e[MSG_MAX_EVENTS];/**< for id == GETSTIMULI */
        } fsm_events;

        unsigned fsm_events_count; /**< for id == STIMULICOUNT */

#ifdef EMULATOR
      /* For id == GETCLOCKLATCHMS or SETCLOCKLATCHMS */
      unsigned latch_time_ms;
      /* For id ==  CLOCKISLATCHED */
      unsigned latch_is_on;
     /* For id == FASTCLOCK or ISFASTCLOCK */ 
      int fast_clock_flg;
     /* For id == CLOCKLATCHPING */
      long long ts_for_clock_latch;
#endif
    } u;
  };

  /** Struct put into shm->fifo_daq, 1 per scan */
  struct DAQScan 
  {
#   define DAQSCAN_MAGIC (0x133710)
    unsigned magic : 24;
    unsigned nsamps : 8;
    long long ts_nanos;    
    unsigned short samps[0];
  };

  /** For use in NRTOutput struct below */
  enum NRTOutputType {
    NRT_TCP = 1,
    NRT_UDP = 2,
    NRT_BINDATA = (1<<7), /* set this bit in type to suppress formatting and just send raw data*/
  };

  /** This struct gets written to fifo_nrt_output for userspace processing */
  struct NRTOutput {
#   define NRTOUTPUT_MAGIC (0x12c9)
#   define NRTOUTPUT_DATA_SIZE OUTPUT_SPEC_DATA_SIZE
    unsigned short magic;
    unsigned short state; /* the state machine state that caused this */
    int trig; /*  this was the value of the state machine column
                  for this NRT trigger.. */
    unsigned long long ts_nanos;
    unsigned char type; /* one of NRTOutputType above */
    unsigned char col;  /* the state machine column */
    char ip_host[IP_HOST_LEN];
    unsigned short ip_port;
    union { 
        /* - This packet gets formatted based on the 'template' below 
             with params being interpreted based on specifal %-codes: 
                %v - the value of the state matrix column (trig above struct)
                %t - timestamp (floating point number, in seconds)
                %T - timestamp (fixed point number, in nanoseconds)
                %s - state
                %c - col
                %% - literal '%'
                %(anything else) - consumed (not printed)
                (it's ok for the same param to appear multiple times as well
                as for a param to not exist.
                For example: "SET ODOR Bank1 %v" would produce 
                "SET ODOR Bank1 13" if the state matrix column (trig) had
                value 13.
                Another example: "The packet value: %v timestamp: %t state: %s col: %c.  This is a literal percent: %%.  This is consumed: %u"
                Would produce the resulting text: "The packet value: 13 timestamp: 25.6 state: 47 col: 11.  This is a literal percent: %.  This is consumed: " (For trig=13 ts_nanos=2560000000 state=47 col=11).
           - The resulting text is sent to a host via a TCP
             connection or a UDP datagram.  
           - The connection is then immediately terminated and/or the socket is
             closed right away. */
        char ip_packet_fmt[FMT_TEXT_LEN]; /* for NRT_TCP or NRT_UDP */
        struct { /* iff bit NRT_BINDATA is set in 'type', use this struct and ignore above.. */
            unsigned datalen;
            unsigned char data[FMT_TEXT_LEN-sizeof(unsigned)];
        };
    };
  };
#ifdef EMULATOR
# define NUM_STATE_MACHINES 1
#else
# define NUM_STATE_MACHINES 4
#endif
  /** 
      The shared memory -- not every plugin needs shared memory but 
      it's a convenient way for userspace UI to communicate with your
      control (real-time kernel) plugin
  */
  struct Shm 
  { 
    int fifo_out[NUM_STATE_MACHINES]; /* The kernel-to-user FIFO             */
    int fifo_in[NUM_STATE_MACHINES];  /* The user-to-kernel FIFO             */
    int fifo_trans[NUM_STATE_MACHINES]; /* Kernel-to-user FIFO to notify of 
                                           state transitions                 */
    int fifo_daq[NUM_STATE_MACHINES]; /* Kernel-to-user FIFO that contains DAQ scans.. */
    int fifo_nrt_output[NUM_STATE_MACHINES]; /* Kernel-to-user FIFO that contains NRTOutput structs for non-realtime state machine outputs! */
    int fifo_debug; /* The kernel-to-user FIFO setup for debugging           */
    
    /* When fifo_in gets an int, this value is read by kernel-process.
       (The alternative would have been to write this msg to a FIFO
       but that's a lot of wasteful double-copying.  It's faster to
       use the shm directly, and only use the FIFO for synchronization
       and notification.  */
    struct ShmMsg msg[NUM_STATE_MACHINES]; 
    volatile int fsmCtr; /**< Incremented (by userspace) each time a new FSM is compiled.  Useful for generating unique FSM module and SHM names. */
    int    magic;               /*< Should always equal SHM_MAGIC            */
  };


  typedef int FifoNotify_t;  /* Write one of these to the fifo to notify
                                that a new msg is available in the SHM       */
#define FIFO_SZ (sizeof(FifoNotify_t))
#define FIFO_TRANS_SZ (sizeof(struct StateTransition)*128)
#define FIFO_DAQ_SZ (1024*1024) /* 1MB for DAQ fifo */
#define FIFO_NRT_OUTPUT_SZ (sizeof(struct NRTOutput)*10)

#ifndef __cplusplus
  typedef struct Shm Shm;
#endif

#define FSM_SHM_NAME "FSMShm"
#define FSM_SHM_MAGIC ((int)(0xf001011d)) /*< Magic no. for shm... 'fool011d'  */
#define FSM_SHM_SIZE (sizeof(struct Shm))
#ifdef __cplusplus
}
#endif

#endif
