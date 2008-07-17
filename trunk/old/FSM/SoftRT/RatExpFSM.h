#ifndef RAT_EXP_FSM_H
#  define RAT_EXP_FSM_H

#ifdef __cplusplus
extern "C" {
#endif

  typedef int int32;
  typedef unsigned uint32;
  typedef long long int64;
  typedef unsigned long long uint64;
  typedef signed char int8;
  typedef unsigned char uint8;
  typedef signed short int16;
  typedef unsigned short uint16;

  /** The amount of memory to preallocate in Kernel space for the FSM matrix. 

      It's set to 1MB, that's 256 thousand matrix cells! 

      Note: Don't set this larger than, say 6MB or so because struct ShmMsg
      (which contains FSMBlob) gets allocated on the stack in 
      RatExpFSMServer.cpp.  (The default stack limit on Linux is 8MB). */
#define FSM_MEMORY_BYTES (1024*1024) 
#define FSM_FLAT_SIZE (FSM_MEMORY_BYTES/sizeof(unsigned)) 
#define FSM_MAX_SCHED_WAVES (sizeof(unsigned)*8)
#define FSM_MAX_IN_CHANS 32
#define FSM_MAX_OUT_CHANS 32
#define FSM_MAX_IN_EVENTS (FSM_MAX_IN_CHANS*2)

struct SchedWave
{
  int enabled; /**< Iff true, actually use this sched_wave.  Otherwise it's
                  considered invalid and ignored. */

  unsigned preamble_ms; /**< the amount of time from when it is triggered
                           to when the scheduled waveform goes high, in ms. */
  unsigned sustain_ms;  /**< the amount of time from when the waveform fires
                           high to when it goes low again, in ms */
  unsigned refraction_ms; /**< the blanking time for the scheduled waveform.
                              The amount of time after it goes low again
                              until it can be triggered normally again.
                              It cannot fire or be triggered during the 
                              refractory period -- instead a triggered wave
                              during this time will *not* fire at all!!

                              refraction_ns_remaining + preamble_ns after the
                              trigger.  Not sure how useful this is.. but
                              here it is. */
};

enum { DIO_TYPE = 0, AI_TYPE, UNKNOWN_TYPE };

  /** A generic linear array that can be used for dynamically-sized 2D arrays.
      It is intended to be the real memory space where the runtime state matrix
      lives.  However, since it takes some pointer math to get the row,column
      elements of this 2D array, you proabbly want to do the following
      to access FSM elements living at row,col:

      FSM_AT(some_ptr_to_struct_FSMBlob, row, col); */
struct FSMBlob
{
  unsigned short n_rows; /* Corresponds to number of states... */
  unsigned short n_cols; /* Corresponds to variable number of input events plus
                            4 of the fixed columns at the end plus possibly 1
                            column for the SCHED_WAVE column at the end
                            if present. */
  unsigned flat[FSM_FLAT_SIZE];

  unsigned ready_for_trial_jumpstate; /**< normally always 35 */

  int has_sched_waves; /**< boolean value indicating if we are to 
                            use scheduled waves at all.  This influences 
                            whether we have the additional SCHED_WAVE column 
                            as the last column of the state matrix. */

  /** The scheduled wave specifications.  @see struct SchedWave */
  struct SchedWave sched_waves[FSM_MAX_SCHED_WAVES];

  /** Struct to describe routing mappings from input/output channel id's
      to physical channel id's.  */
  struct Routing {
    unsigned in_chan_type; /** either AI_TYPE, DIO_TYPE, or UNKNOWN_TYPE */

    unsigned num_in_chans;  /** <= FSM_MAX_IN_CHANS                      */
    unsigned first_in_chan; /** <= FSM_MAX_IN_CHANS                      */
    unsigned num_evt_cols;  /** <= FSM_MAX_IN_CHANS*2 and <= n_cols-(4+1)*/

    /** Note the following two fields should add up to <= 32! */
    unsigned num_cont_chans; 
    unsigned num_trig_chans;

    unsigned num_vtrigs;

    /** Associative array of phys_in_chan_id*2+(edge down?1:0) -> matrix 
        column */
    int input_routing[FSM_MAX_IN_EVENTS];
    /** Map of sched_wave_id*2+(edge down ? 1 : 0) -> matrix column  
        or -1 for none */
    int sched_wave_input[FSM_MAX_SCHED_WAVES*2];
    /** Map of sched_wave_id -> output channel id or -1 for none */
    int sched_wave_output[FSM_MAX_SCHED_WAVES];
  } routing;
};

/** Low-level interface to struct FSMBlob.  
    For a given struct FSMBlob *, get the data for row, col */
#define FSM_AT(_fsm_, _row_, _col_) \
    ((_fsm_)->flat[_row_*(_fsm_)->n_cols + _col_])

 /** Structure to encapsulate a single state in the state matrix.
     It basically is a convenience for accessing columns in the state matrix.

     Note that the real FSM matrix size is dynamic and only knowable 
     at runtime.  As such, the real state info is stored in FSMBlob,
     which is a generic linear array that takes on the right
     two-dimensional size at run-time.

     This structure is only used for convenience in low-level code that
     loads the matrix (see RatExpFSMServer.cpp).  It may be more convenient 
     to use this struct rather than doing FSM_AT(some_ptr_to_blob, row, col)
     to grab a matrix cell.  Then again, maybe not. */
struct State
{
  unsigned n_inputs;

  /* Pointers into FSMBlob.  Note that the input array below is always
     4 (+ 1 iff we have scheduled waves) smaller than FSMBlob::n_cols.  
     The remaining 4(+1) columns are for the values timeout_state, timeout_us, 
     continuous_outputs, trigger_outputs, and the optional sched_wave column.

     Strictly speaking, this struct is not necessary and is only used to make 
     code more readable.. */

  union {
    unsigned *input;   /* Array pointer to variable sized array.., 
                          points directly into struct FSBBlob!  */
    unsigned *column;  /* Alias for above.. use this when accessing past 
                          n_inputs for clarity. */
  };

  /* Note that whereas the above 'column/input' field is actually pointing
     into FSMBlob (and thus is a good way to modify the FSM for an
     FSMBlob), the remaining four fields are copies taken from
     column[n_inputs] to column[n_inputs+4+1]  */
  unsigned timeout_state; /* Where to go on timeout.. */
  unsigned timeout_us;  /* How long 'till state times out in microseconds 
                            -- 0 for never   */
  unsigned continuous_outputs; /* Bitarray of channel id's relative to
                                  FIRST_CONT_CHAN */
  unsigned trigger_outputs; /* bitarray of channel id's relative to 
                               FIRST_TRIG_CHAN */
  unsigned sched_wave; /**< Bitmask of which of the scheduled waves
                            to fire when entering this state.  Note there are 
                            31 possible scheduled waves, thus this bitfield 
                            array can schedule 31 simultaneous waves 
                            potentially! Use negative bitvalues (yes, this
                            will get cast to int) to un-schedule a wave). */
};

/** For a given struct FSMBlob *, and a given destination struct State *,
    populate it with info for state_num */
#define GET_STATE(fsm_, state_, state_num_) \
do { \
  unsigned no = state_num_; \
  struct FSMBlob * const _fsm = (struct FSMBlob *)(fsm_); \
  struct State * const _state = (struct State *)(state_); \
  if (no >= _fsm->n_rows) no = 0; \
  _state->n_inputs = _fsm->routing.num_evt_cols; \
  _state->input = &FSM_AT(_fsm, no, 0); \
  _state->timeout_state = FSM_AT(_fsm, no, _state->n_inputs); \
  _state->timeout_us = FSM_AT(_fsm, no, _state->n_inputs+1); \
  _state->continuous_outputs = FSM_AT(_fsm, no, _state->n_inputs+2); \
  _state->trigger_outputs = FSM_AT(_fsm, no, _state->n_inputs+3); \
  _state->sched_wave = _fsm->has_sched_waves ? FSM_AT(_fsm, no, _state->n_inputs+4) : 0; \
} while(0)

/** Declare a struct State and a pointer to it for use with GET_STATE above..*/
#define DECLARE_STATE_PTR(varname) \
struct State varname##_on_auto__, * const varname = & varname##_on_auto__ 
#define DECLARE_STATE_AND_PTR(varname) DECLARE_STATE_PTR(varname)

/** NB: all timestamps are in microseconds and are relative to each other   */
struct StateTransition 
{ 
  unsigned previous_state;
  unsigned state; /* The state that was entered via this transition.   */
  int event_id; /* The event_id that *led* to this state.  An event_id
                   is basically a column position.                     */
  long long ts; /* When we entered the state, in nanoseconds since FSM reset. */
};

#define AOWAVE_MEMORY_BYTES (FSM_MEMORY_BYTES/2)
#define AOWAVE_MAX_SAMPLES (AOWAVE_MEMORY_BYTES/sizeof(unsigned short))
struct AOWave
{
  unsigned id;
  unsigned aoline;
  unsigned nsamples;
  unsigned loop; /* if true, playing will loop until untriggered */
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

enum ShmMsgID {
    GETPAUSE = 1,  /* Query the FSM to find out if it is paused. */
    PAUSEUNPAUSE, /* Halt the state machine (temporarily).  No variables
                     are cleared but events cannot generate new
                     states or digital outputs. */
    RESET, /* Reset/Initialize the state machine.  All variables are cleared
              and the state matrix is cleared. Equivalent to matlab
              function Initialize(m). */
    GETVALID,  /* Query the FSM to find out if it has a valid state machine
                  specification. */
    INVALIDATE, /* Invalidate (clear) the state machine specification, 
                   but preserve other system variables. */
    FSM, /* Load an FSM. */    
    GETFSM,
    GETFSMSIZE, /* Query the rows,cols of the FSM Matrix. */
    FORCEEVENT, /* Force a particular event to have occurred. */
    FORCETIMESUP, /* Force a time's up event to have occurred. */
    FORCETRIGGER,  /* Force the FSM to do some triggers outputs for this tick*/
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
    LAST_SHM_MSG_ID
};

struct ShmMsg {
    int id; /* One of ShmMsgID above.. */
    
    /* Which element of union is used depends on id above. */
    union {

      /* For id == FSM */
      struct FSMBlob fsm;

      /* For id == TRANSITIONS */
      struct {
        unsigned num;
        unsigned from; 
#define MSG_MAX_TRANSITIONS 64
        struct StateTransition transitions[MSG_MAX_TRANSITIONS];
      } transitions;

      /* For id == TRANSITIONCOUNT */
      unsigned transition_count;

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
      long long runtime_us; /* Time since last reset, in seconds */

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


  /** 
      The shared memory -- not every plugin needs shared memory but 
      it's a convenient way for userspace UI to communicate with your
      control (real-time kernel) plugin
  */
  struct Shm 
  { 
    int fifo_out; /* The kernel-to-user FIFO                                 */
    int fifo_in;  /* The user-to-kernel FIFO                                 */
    int fifo_trans; /* Kernel-to-user FIFO to notify of state transitions    */
    int fifo_daq; /* Kernel-to-user FIFO that contains DAQ scans.. */
    int fifo_debug; /* The kernel-to-user FIFO setup for debugging           */
    
    struct ShmMsg msg; /* When fifo_in gets an int, this value is read
                          by kernel-process.  (The alternative would have been
                          to write this msg to a FIFO but that's a lot of
                          wasteful double-copying.  It's faster to use
                          the shm directly, and only use the FIFO for 
                          synchronization and notification.                 */

    int    magic;               /*< Should always equal SHM_MAGIC            */
  };


  typedef int FifoNotify_t;  /* Write one of these to the fifo to notify
                                that a new msg is available in the SHM       */
#define FIFO_SZ (sizeof(FifoNotify_t))
#define FIFO_TRANS_SZ (sizeof(struct StateTransition)*128)
#define FIFO_DAQ_SZ (1024*1024) /* 1MB for DAQ fifo */
#ifndef __cplusplus
  typedef struct Shm Shm;
#endif

#define SHM_NAME "RatExpFSM"
#define SHM_MAGIC ((int)(0xf001010e)) /*< Magic no. for shm... 'fool010e'  */
#define SHM_SIZE (sizeof(struct Shm))
#ifdef __cplusplus
}
#endif

#endif
