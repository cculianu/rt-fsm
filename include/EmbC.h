#ifndef EmbC_H
#define EmbC_H

#ifndef NO_EMBC_TYPEDEFS
#ifdef EMBC_GENERATED_CODE /* These typedefs already exist in most contexts */
typedef unsigned long ulong;
typedef unsigned int uint;
typedef unsigned short ushort;
#endif
typedef unsigned long long uint64;
typedef long long int64;
#endif

struct EmbCTransition 
{
  double time; // the time in seconds that this transition occurred
  uint from; // the state we came from
  uint to; // the state we went to
  uint event; // the event id that led to this transition
};

typedef struct EmbCTransition EmbCTransition;

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

  // prints a message (most likely to the kernel log buffer)
  //int (*printf)(const char *format, ...);

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

// returns a random number in the range [0, 1.0]
static inline double rand(void) {  return __embc->rand(); }

// returns a random number in the range [-1,0, 1.0] normalized over a distribution with mean 0 and unit variance.
static inline double randNormalized(void) {  return __embc->randNormalized(); }

static inline void logValue(const char *vn, double vv) {   __embc->logValue(*__embc->fsm, vn, vv); }

static inline void logArray(const char *vn, const double *vv, uint num) {   __embc->logArray(*__embc->fsm, vn, vv, num); }

// just like math library, do man on these to see their usage
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

// returns the current time in seconds
static inline double time(void) { return *__embc->time; }
// current state
static inline uint state(void) { return *__embc->state; }
// ready for trial flag
static inline int ready_for_trial(void) { return *__embc->ready_for_trial; }
// the number of transitions thus far
static inline uint transitions(void) { return *__embc->transitions; }
// the cycle number
static inline uint64 cycle(void) { return *__embc->cycle; }
// the state machine task rate
static inline uint rate(void) { return *__embc->rate; }
// the fsm id
static inline uint fsm(void) { return *__embc->fsm; }
// see struct EmbCTransition above -- the most recent state transition
static inline struct EmbCTransition transition(void) { return *__embc->transition; }
#if defined(RTLINUX) && !defined(RTAI)
extern int rtl_printf(const char *format, ...);
#  define printf rtl_printf
#elif defined(RTAI) && !defined(RTLINUX)
extern int rt_printk(const char *format, ...);
#  define printf rt_printk
#else
#  error Exactly one of RTLINUX or RTAI needs to be defined!
#endif

/*--------------------------------------------------------------------------*/
#endif /* EMBC_GENERATED_CODE */

#endif

#endif
