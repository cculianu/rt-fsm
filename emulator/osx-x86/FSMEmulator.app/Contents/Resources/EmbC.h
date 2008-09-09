#ifndef EmbC_H
#define EmbC_H

#if defined(OS_OSX) || defined(OS_WINDOWS)
#define NEED_UINT
#endif

#if !defined(NO_EMBC_TYPEDEFS) || defined(NEED_UINT)
#if defined(EMBC_GENERATED_CODE) || defined(NEED_UINT) /* These typedefs already exist in most contexts */
typedef unsigned long ulong;
typedef unsigned int uint;
typedef unsigned short ushort;
#endif
#if !defined(OS_WINDOWS) || defined(NEED_INT64)
#ifdef UNDEF_INT64
#  undef uint64
#  undef int64
#endif
#ifndef NO_EMBC_TYPEDEFS
typedef unsigned long long uint64;
typedef long long int64;
#endif
#endif
#endif

typedef int TRISTATE; /* A tristate value takes one of 3 states: POSITIVE, NEGATIVE, or NEUTRAL */
#define POSITIVE (1)
#define NEGATIVE (-1)
#define NEUTRAL (0)
#define ISPOSITIVE(t) (t > 0)
#define ISNEGATIVE(t) (t < 0)
#define ISNEUTRAL(t)  (t == 0)
#define EMBC_TCP 0
#define EMBC_UDP 1

struct EmbCTransition 
{
  double time; // the time in seconds that this transition occurred
  uint from; // the state we came from
  uint to; // the state we went to
  uint event; // the event id that led to this transition
};

typedef struct EmbCTransition EmbCTransition;

#define EMBC_LOG_NAME_SZ 33
struct EmbCLogItem
{
    double ts; /**< timestamp in seconds */
    char name[EMBC_LOG_NAME_SZ];
    double value;
};

typedef struct EmbCLogItem EmbCLogItem;

struct EmbC
{
  /* Filled-in by rt-process */
  const volatile double *time; // the current time, in seconds
  const volatile uint *state; // the current state we are in
  const volatile int *ready_for_trial; // flag telling FSM we are ready for a new trial
  const volatile uint *transitions; // a count of the number of state transitions thus far for this FSM
  const volatile uint64 *cycle; // the count of the number of cycles since the last time the FSM was reset.  The duration of a cycle is 1.0/rate seconds (see rate variable below).
  const volatile uint *rate; // the sampling rate, in Hz, that the FSM is running at.
  const volatile uint *fsm; // the id number of the fsm.  usually 0 but if 1 machine is running multiple FSMs, maybe larger than 0
  // a struct that encapsulates the last state transition.  Note that
  // transition.to normally is the same value as 'state'
  const struct EmbCTransition *transition;  

  // returns a random number in the range [0, 1.0]
  double (*rand)(void);
  // returns a random number in the range [-1,0, 1.0] normalized over a distribution with mean 0 and unit variance.
  double (*randNormalized)(void);

  // logs a single value
  void (*logValue)(uint fsm, const char *varname, double val);
  // logs an entire array of num_elems size.  Each element of the array creates a separate timestamp, name, value record in the log.
  void (*logArray)(uint fsm, const char *varname, const double *array, uint num_elems);
  // get the number of items in the log
  uint (*logGetSize)(uint fsm);
  // get a specific item from the log, or a dummy item if not found
  const EmbCLogItem * (*logGetAt)(uint fsm, uint pos);
  const EmbCLogItem * (*logFind)(uint fsm, const char *name, uint N);

  /* Read an AI channel.  Note calls to this function are cheap because results get cached.. */
  double (*readAI)(unsigned channel_id);
  
  /* Write to an AO channel. */
  int (*writeAO)(unsigned chan, double volts);
  
  /* Immediately jump to a new state, returns 1 on success, 0 on failure. */
  int (*forceJumpToState)(uint fsm, unsigned state, int event_id_for_history);
  /* Read a DIO line, 0/1 on success, negative on failure. */
  int (*readDIO)(unsigned);
  /* Write to a DIO line, 1 on success, 0 on failure. */
  int (*writeDIO)(unsigned, unsigned);
  /* Like matlab-side BypassDOout.m -- override state machine outputs relative to output routing spec */
  int (*bypassDOut)(unsigned,unsigned);
  
  void (*sendPacket)(uint fsm, int proto, const char *, unsigned short, const void *, unsigned);
  void (*trigExt)(unsigned, unsigned);
  void (*trigSchedWave)(unsigned, unsigned);
  
  // prints a message (most likely to the kernel log buffer)
  int (*printf)(const char *format, ...) __attribute__ ((format (printf, 1, 2)));
  // print formatted to a string 
  int (*snprintf)(char *buf, unsigned long len, const char *format, ...) __attribute__ ((format (printf, 3, 4)));

  // just like math library
  double (*sqrt)(double);
  double (*exp)(double); /* e raised to x */
  double (*exp2)(double); /* 2 raised to x */
  double (*exp10)(double); /* 10 raised to x */
  double (*pow)(double x, double y); /* x raised to y */
  double (*log)(double);
  double (*log2)(double);
  double (*log10)(double);
  double (*sin)(double);
  double (*cos)(double);
  double (*tan)(double);
  double (*atan)(double);
  double (*round)(double);
  double (*ceil)(double);
  double (*floor)(double);
  double (*fabs)(double);
  double (*acosh)(double);
  double (*asin)(double);
  double (*asinh)(double);
  double (*atanh)(double);
  double (*cosh)(double);
  double (*expn)(int,double); /* exponential inegral n */
  double (*fac)(int); /* factorial */
  double (*gamma)(double); /* see man gamma */
  int (*isnan)(double); /* true if x is NaN (not a number) */
  double (*powi)(double, int); /* y = x to the i */
  double (*sinh)(double); /* inverse hyperbolic sine */
  double (*tanh)(double);
  
  /* Filled-in by generated embedded C module */
  void (*init)(void);
  void (*cleanup)(void);
  void (*statetransition)(void);
  void (*tick)(void);
  void (*entry)(unsigned short);
  void (*exit)(unsigned short);
  unsigned long (*get_at)(unsigned short, unsigned short);
  TRISTATE (*threshold_detect)(int channel_id, double volts);
  /* called by rt-process to increase use count, etc.. */
  void (*lock)(void); /**< NOTE: Only callable in linux process context due to kernel 2.6 limitations */
  void (*unlock)(void); /**< NOTE: Only callable in linux process context due to kernel 2.6 limitations*/
};

#ifdef EMBC_MOD_INTERNAL

/* ------- Pointers and functions coming from generated .c file! ----------- */
/* Name of the shm to use for shared struct EmbC that is given to
   rt-process.  The value of this pointer comes from the generated .c
   file */
extern const char *__embc_ShmName;

extern void (*__embc_init)(void);
extern void (*__embc_cleanup)(void);
extern void (*__embc_transition)(void);
extern void (*__embc_tick)(void);
extern TRISTATE  (*__embc_threshold_detect)(int, double);
extern struct EmbC *__embc;

extern unsigned long __embc_fsm_get_at(ushort row, ushort col);
extern void __embc_fsm_do_state_entry(ushort state);
extern void __embc_fsm_do_state_exit(ushort state);

/* ------------------------------------------------------------------------- */
#ifdef EMBC_GENERATED_CODE /* only defined inside the generated FSM code */

/*--- Wrappers for embedded C ----------------------------------------------
  NOTE: User code should use the calls below and not use the function calls
  above.  struct EmbCTransition from above is ok, though.
----------------------------------------------------------------------------*/

/// returns a random number in the range [0, 1.0]
static inline double rand(void) {  return __embc->rand(); }

/// returns a random number normalized over a distribution with mean 0 and std dev 1.
static inline double randNormalized(void) {  return __embc->randNormalized(); }

static inline void logValue(const char *vn, double vv) {   __embc->logValue(*__embc->fsm, vn, vv); }

static inline void logArray(const char *vn, const double *vv, uint num) {   __embc->logArray(*__embc->fsm, vn, vv, num); }
/// get the number of items currently in the log, or 0 if empty
static inline unsigned logGetSize(void) { return __embc->logGetSize(*__embc->fsm); }
/// retrieve a specific item from the variable log.  May return a bogus item if 'pos' is >= logGetSize().
static inline const EmbCLogItem *logGetAt(unsigned pos) { return __embc->logGetAt(*__embc->fsm, pos); }
/// get the most recently logged item
static inline const EmbCLogItem *logTop(void) { return __embc->logGetAt(*__embc->fsm, logGetSize()-1); }
/// searches for an item named 'name', but only searches the last 'lastNToSearch' log items.  Returns a dummy  with an empty name and 0 value if not found!!
static inline const EmbCLogItem *logFindLastN(const char *name, unsigned lastNToSearch) { return __embc->logFind(*__embc->fsm, name, lastNToSearch); }
/// searches the entire log for a specific item named 'name'.  Returns a dummy if not found!!
static inline const EmbCLogItem *logFind(const char *name) { return logFindLastN(name, logGetSize()); }

/*------------------------------
  MISC C-Library-like-functions
  -----------------------------*/
#define printf(x...) (__embc->printf(x))
#define snprintf(x...) (__embc->snprintf(x))

extern void *memset(void *, int c, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern unsigned long strlen(const char *s);
extern int strcmp(const char *, const char *);
extern int strncmp(const char *, const char *, unsigned long);
extern int strcasecmp(const char *, const char *);
extern int strncasecmp(const char *, const char *, unsigned long);
extern char *strchr(const char *, int);
extern char *strnchr(const char *, unsigned long, int);
extern char *strstr(const char *, const char *);
extern char *strcat(char *, const char *);
extern char *strncat(char *, const char *, unsigned long);


/*------------------------
  MATH LIBRARY SUPPORT
  ------------------------*/
/* just like math library, do man on these to see their usage */
static inline double sqrt(double d) { return __embc->sqrt(d); }
static inline double exp(double d) { return __embc->exp(d); }
static inline double exp2(double d) { return __embc->exp2(d); }
static inline double pow(double x, double y) { return __embc->pow(x,y); }
static inline double log(double d) { return __embc->log(d); }
static inline double log2(double d) { return __embc->log2(d); }
static inline double log10(double d) { return __embc->log10(d); }
static inline double sin(double d) { return __embc->sin(d); }
static inline double cos(double d) { return __embc->cos(d); }
static inline double tan(double d) { return __embc->tan(d); }
static inline double atan(double d) { return __embc->atan(d); }
static inline double round(double d) { return __embc->round(d); }
static inline double ceil(double d) { return __embc->ceil(d); }
static inline double floor(double d) { return __embc->floor(d); }
static inline double fabs(double d) { return __embc->fabs(d); }
static inline double acosh(double d) { return __embc->acosh(d); }
static inline double asin(double d) { return __embc->asin(d); }
static inline double asinh(double d) { return __embc->asinh(d); }
static inline double atanh(double d) { return __embc->atanh(d); }
static inline double cosh(double d) { return __embc->cosh(d); }
static inline double expn(int i, double d) { return __embc->expn(i,d); }
static inline double fac(int i) { return __embc->fac(i); }
static inline double gamma(double d) { return __embc->gamma(d); }
static inline int isnan(double d) { return __embc->isnan(d); }
static inline double powi(double d, int i) { return __embc->powi(d, i); }
static inline double sinh (double d) { return __embc->sinh(d); }
static inline double tanh(double d) { return __embc->tanh(d); }

/// returns the current time in seconds
static inline double time(void) { return *__embc->time; }
/// current state
static inline uint state(void) { return *__embc->state; }
/// ready for trial flag
static inline int ready_for_trial(void) { return *__embc->ready_for_trial; }
/// the number of transitions thus far
static inline uint transitions(void) { return *__embc->transitions; }
/// the cycle number
static inline uint64 cycle(void) { return *__embc->cycle; }
/// the state machine task rate
static inline uint rate(void) { return *__embc->rate; }
/// the fsm id
static inline uint fsm(void) { return *__embc->fsm; }
/// see struct EmbCTransition above -- the most recent state transition
static inline struct EmbCTransition transition(void) { return *__embc->transition; }
/** Forces the state machine to immediately jump to a state -- bypassing normal
    event detection mechanism. Note that in the new state, pending events are 
    not cleared, so that they may be applied to the new state if and only if 
    they haven't yet been applied to the *old* state.  (If you don't like this 
    behavior let Calin know and he can change it or hack the code yourself.)

    This call is advanced and not recommended as it violates the simplicity and 
    clarity of the finite state machine paradigm, but it might be useful as a 
    hack to make some protocols easier to write.
    Returns 1 on success or 0 on error. */
static inline int forceJumpToState(unsigned state, int event_id_for_history) { return __embc->forceJumpToState(fsm(), state, event_id_for_history); }

/*------------------------
  LOW LEVEL I/O FUNCTONS 
  ------------------------*/

/** Read an AI channel -- note that if the AI channel was not enabled,
    that is, it was not part of the InputEvents spec for any running state 
    machine, the read will have to actually go to the DAQ hardware and could
    potentially be slowish because it requires an immediate hardware
    conversion. However the good news is subsequent reads on the same channel
    are always cached for that task cycle. Channel id's are 0-indexed 
    (correspond to the hardware's real channel-id-space). */
static inline double readAI(unsigned chan) { return __embc->readAI(chan); }
/** Write to a physical analog output channel.  Note that no caching is ever 
    done and a call to this function physically results in a new conversion and
    voltage being written to the hardware DAC.
    Returns true on success, false on failure. */
static inline int    writeAO(unsigned chan, double voltage) { return __embc->writeAO(chan, voltage); }
/** Read from a physical digital I/O line.  Channel id's are 0-indexed and 
    refer to the absolute channel number on the hardware.  The read is not done
    immediately, but is just the cached value that the state machine read at
    the beginning of the task cycle.
    Note that the line should already have been configured for digital input
    (normally done by telling the state machine to use input events of 
    type 'dio').
    [Note that input routing is not checked, so that chan is the global
    hardware channel ID and not relative to the first channel in the input 
    routing spec.]
    Returns 0/1 for the bitvalue or negative on failure.
    Failure reasons include an invalid channel id or trying to read from a 
    channel that is currently configured for digital output. */
static inline int readDIO(unsigned chan) { return __embc->readDIO(chan); }
/** Write to a physical digital I/O line (this overrides normal state 
    machine output). Channel id's are 0-indexed and refer to the absolute 
    channel number on the hardware.  The writes themselves don't take effect
    immediately, but rather at the end of the current task cycle (this is an
    optimization to make all DIO lines take effect at once).  Note that the 
    DIO channel 'chan' should have already been configured for digital output.
    This happens automatically if you are using input routing of type 'ai' 
    (thus freeing up all DIO channels to be digital outputs).
    [Note that output routing is not checked, so that chan is the global
    hardware channel ID and not relative to the first channel in the output 
    routing spec. (As compared to how BypassDOut works).]
    Returns true on success or 0 on failure. 
    Failure reasons may include: an invalid channel ID, or trying to write
    to a channel that is currently configured for digital input. */
static inline int writeDIO(unsigned chan, unsigned bitval) { return __embc->writeDIO(chan, bitval); }
/** Similar to the matlab client-side BypassDOout call -- simultaneously forces
    all of the channels in bitmask to be on until the next call to bypassDOut(0). Note 
    that the channels in this specification are actually relative to the output routing spec
    for this state machine! 
     Returns 1 on success or 0 on failure. */
static inline int bypassDOut(unsigned bitmask) {  return __embc->bypassDOut(fsm(), bitmask); }
    
/** Similar to the 'tcp' output routing type.  Basically, sometime in the 
    future,a userspace non-realtime thread will initiate a connection to 
    host:port and send data of length datalen, then it will close the connection. */
static inline void sendTCPPacket(const char *host, unsigned short port, const void *data, unsigned datalen) { __embc->sendPacket(fsm(),EMBC_TCP,host,port,data,datalen); }
/** Similar to above -- a shortcut for sending string data: expects a NUL-terminated string */
static inline void sendTCPPacketStr(const char *host, unsigned short port, const char *str) { sendTCPPacket(host, port, str, strlen(str)); }
/** Similar to the 'udp' output routing type.  Basically, sometime in the future,
    a userspace non-realtime thread will send dataString as a datagram to 
    host:port. */
static inline void sendUDPPacket(const char *host, unsigned short port, const void *data, unsigned datalen) { __embc->sendPacket(fsm(),EMBC_UDP,host,port,data,datalen); }
/** Similar to above -- a shortcut for sending string data: expects a NUL-terminated string */
static inline void sendUDPPacketStr(const char *host, unsigned short port, const char *str) { sendUDPPacket(host, port, str, strlen(str)); }
/** Call the external triggerable module passing it data 'which' and trigger 'trig'.  
    This is typically how you trigger sound! */
static inline void triggerExt(unsigned which, int trig) { __embc->trigExt(which, trig); }
/** Synonym for triggerExt() but named triggerSound() for convenience */
static inline void triggerSound(unsigned card, unsigned snd) { triggerExt(card, snd); }
static inline void untriggerSound(unsigned card, unsigned snd) { triggerExt(card, -(int)snd); }
/** Trigger a scheduled wave to play.  */
static inline void triggerSchedWave(unsigned wave_id) { __embc->trigSchedWave(fsm(), 0x1<<wave_id); }
/** Un-trigger a scheduled wave.  */
static inline void untriggerSchedWave(unsigned wave_id) { __embc->trigSchedWave(fsm(), -(int)(0x1<<wave_id)); }

/*--------------------------------------------------------------------------*/
#endif /* EMBC_GENERATED_CODE */

#endif

#endif
