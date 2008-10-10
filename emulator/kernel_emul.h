#ifndef KERNEL_EMUL_H
#define KERNEL_EMUL_H
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#ifdef WIN32
#include "ffs.h"
#else
#include <strings.h>
#endif
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* mbuff stuff */
    void *mbuff_alloc(const char *name, unsigned long size);
    void *mbuff_attach(const char *name, unsigned long size);
    void  mbuff_detach(const char *name, void *mem);
    void  mbuff_free(const char *name, void *mem);

    /* fifo stuff */
    int rtf_find_free(unsigned *minor_out, unsigned long size);
    int rtf_destroy(unsigned fifo);
    int rtf_put(unsigned fifo, const void *data, unsigned long size);
    int rtf_get_if(unsigned fifo, void *data_out, unsigned long size);
    int RTF_FREE(unsigned minor);

    /* comedi stuff */
    struct ComediEmul;
    typedef struct ComediEmul comedi_t;
    typedef short sampl_t;
    typedef int lsampl_t;
#   define RF_UNIT(f) ((f)&0xff)
#   define UNIT_volt               0
#   define UNIT_mA                 1
#   define UNIT_none               2
    typedef struct comedi_krange_struct { int min, max; unsigned flags; } comedi_krange;
    comedi_t *comedi_open(const char *devfile);
    int comedi_close(comedi_t *);
    int comedi_lock(comedi_t *, unsigned);
    int comedi_unlock(comedi_t *, unsigned);
    int comedi_cancel(comedi_t *, unsigned);
    /* subdevice types */
#   define COMEDI_SUBD_UNUSED              0       /* unused */
#   define COMEDI_SUBD_AI                  1       /* analog input */
#   define COMEDI_SUBD_AO                  2       /* analog output */
#   define COMEDI_SUBD_DI                  3       /* digital input */
#   define COMEDI_SUBD_DO                  4       /* digital output */
#   define COMEDI_SUBD_DIO                 5       /* digital input/output */
#   define COMEDI_SUBD_COUNTER             6       /* counter */
#   define COMEDI_SUBD_TIMER               7       /* timer */
#   define COMEDI_SUBD_MEMORY              8       /* memory, EEPROM, DPRAM */
#   define COMEDI_SUBD_CALIB               9       /* calibration DACs */
#   define COMEDI_SUBD_PROC                10      /* processor, DSP */
#   define COMEDI_SUBD_SERIAL              11      /* serial IO */
#   define COMEDI_NDEVICES 16
#   define AREF_GROUND 0
    int comedi_find_subdevice_by_type(comedi_t *, int type, unsigned first_sd);
    int comedi_get_n_channels(comedi_t *, unsigned sd);
    enum configuration_ids
    {
        INSN_CONFIG_DIO_INPUT = 0,
        INSN_CONFIG_DIO_OUTPUT = 1,
        INSN_CONFIG_DIO_OPENDRAIN = 2,
        COMEDI_INPUT = INSN_CONFIG_DIO_INPUT,
        COMEDI_OUTPUT = INSN_CONFIG_DIO_OUTPUT,
    };   
    int comedi_get_n_ranges(comedi_t *, unsigned sd, unsigned ch);
    int comedi_get_krange(comedi_t *, unsigned sd, unsigned ch, unsigned r, comedi_krange *out);
    lsampl_t comedi_get_maxdata(comedi_t *, unsigned sd, unsigned ch);    
    int comedi_dio_config(comedi_t *dev,unsigned int subdev,unsigned int chan,
                          unsigned int io);
    int comedi_dio_bitfield(comedi_t * dev, unsigned subdev, unsigned mask,
                            unsigned *bits);
    int comedi_data_write(comedi_t *dev,unsigned int subdev,unsigned int chan,
                          unsigned int range,unsigned int aref,lsampl_t data);
    int comedi_data_read(comedi_t *dev,unsigned int subdev,unsigned int chan,
                         unsigned int range,unsigned int aref,lsampl_t *data);
#   define comedi_register_callback(a,b,c,d,e) ((void)(-1))
    
    /* Misc, rt-associated, and misc kernel funcs. */
#   ifndef STR
#     define STR1(x) #x
#     define STR(x) STR1(x)
#    endif
#   define KERN_INFO    "<font color='#007700'> "
#   define KERN_WARNING "<font color='#00aaaa'> "
#   define KERN_DEBUG   "<font color='#000077'> "
#   define KERN_ERR     "<font color='#770000'> "
#   define KERN_CRIT    "<font color='#ff0000'> "
    typedef unsigned int u32;
    int rt_printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#   define float_vsnprintf vsnprintf

#   define MODULE_AUTHOR(x) /* nothing */
#   define MODULE_DESCRIPTION(x) /* nothing */
#   define module_init(func) int (*fsm_entry_func)(void) = (func)
#   define module_exit(func) void (*fsm_exit_func)(void) = (func)
    extern int (*fsm_entry_func)(void);
    extern void (*fsm_exit_func)(void);
    struct seq_file;
    extern int (*fsm_seq_show_func)(struct seq_file *m, void *dummy);
    int seq_printf(struct seq_file *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    int register_module_param(const char *name, void *param, const char *type);
    int register_module_param_desc(const char *name, void *param, const char *desc);
#   define MODULE_PARMS_BEGIN void fsm_init_module_parms(void) { static int didinit = 0; if (!didinit) {
#   define module_param(p,t,m) register_module_param(STR(p), &p, STR(t))
#   define MODULE_PARM_DESC(p,desc) register_module_param_desc(STR(p), &p, desc)
#   define MODULE_PARMS_END didinit = 1; } }
    extern void fsm_init_module_parms(void); /* call this from outside fsm before calling fsm_entry_func()*/
#   define kmalloc(s, f) malloc(s)
#   define kfree(p) free(p)
#   define vmalloc(s) malloc(s)
#   define vfree(p) free(p)
    typedef volatile int atomic_t;
#   define atomic_set(p,v) (*(p) = (v))
#   define atomic_read(p)  (*(const atomic_t *)(p))
#   define atomic_inc(p) (++*(p))
#   define ATOMIC_INIT(i) (i)
    static __inline__ int __ffs(int x) { return ffs(x)-1; }
    void set_bit(unsigned bit, volatile unsigned long *bits);
    void clear_bit(int nr, volatile unsigned long *addr);
    unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
#   define rt_critical(x) ((x) = rt_start_critical())
    long rt_start_critical(void);
    void rt_end_critical(long flags);
#   define mb()
    struct SoftTask;
    typedef void (*SoftTask_Handler)(void *);
    struct SoftTask *softTaskCreate(SoftTask_Handler handler_function,
                                    const char *taskName);
    void softTaskDestroy(struct SoftTask *);
    int softTaskPend(struct SoftTask *, void *arg);
#   define do_div(l,d) ({ unsigned long r = l%d; l/=d; r; })
    u32 random32(void);
    typedef long long hrtime_t;
    hrtime_t gethrtime(void);
#if defined(OS_OSX) || defined(OS_WINDOWS)
#   define CLOCK_REALTIME 0
#   define TIMER_ABSTIME  1
#endif
#ifdef NEED_RT_DEFINES
#   define clock_gettime   clock_gettime_emul
#   define lldiv lldiv_custom
#endif
    typedef void (*FifoHandlerFn_t)(unsigned);
    void clock_wait_next_period_with_latching(FifoHandlerFn_t, unsigned num_state_machines);
    int clock_gettime_emul(int clkid, struct timespec *ts);
    void timespec_add_ns(struct timespec *ts, long ns);

    /* Misc Query/Manipulate FSM */
    struct FSMStats 
    {        
        unsigned long long cycle;
        double ts; /**< in seconds */
        unsigned state;
        unsigned transitions;
        int isPaused, isValid, readyForTrial, isFastClk, isClockLatched;
        unsigned activeSchedWaves, activeAOWaves;
        double clockLatchTime; ///< in secs
        unsigned enqInpEvts; ///< the number of input events currently enqueued
    };

    /* implemented in fsm.c for emulator only */
    extern void fsm_get_stats(unsigned fsm_no, struct FSMStats *stats_out);

    extern const char *GetTmpPath(void);

    // clock latch stuff...
    /** 
        iff nonzero, make the fsm advances by at most 'latchTimeNanos' 
        nanoseconds (in emulator time) before pausing and waiting for 
        a latchCountdownReset() call, at which point the emulator is resumed.  
        So at most latchTimeNanos ns can elapse between latchCountdownReset() 
        calls.  */
    extern long getLatchTimeNanos(void);
    extern void setLatchTimeNanos(long nanos);
    /// if latchTimeNanos is nonzero, then this needs to be called periodically
    /// otherwise the emulator will pause after latchTimeNanos ns has elapsed.
    /// in other words, this resets the latch counter.
    /// @param: ts_when_latch_starts is a timestamp in emulator time (nanos)
    ///         when the clock latch is to 'start' counting down
    ///         the clock latch will actually assert at time:
    ///          ts_when_latch_starts + getLatchTimeNanos()
    ///         -1 means 'current time'    
    extern void latchCountdownReset(long long ts_when_latch_starts);
    /// returns true iff the clock is currently latched (paused) 
    /// if true, then latchCountdownReset() needs to be called to continue
    /// advancing FSM time
    extern int  isClockLatched(void);

    /* emulate linux's sort by calling C library's qsort() */
    static inline void sort(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *), void *dummy) { (void)dummy; qsort(base, nmemb, size, cmp); }

    /// returns true iff FSM is running in 'fast clock' mode
    extern int isFastClock(void);
    /// set/unset 'fast clock' mode for the FSM
    extern void setFastClock(int on_off);
    
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <map>
#include <string>
#include <functional>

namespace Emul {

    struct ModuleParm
    {
        enum Type { Int, CharP };
        void *p;
        Type type;
        std::string name, desc;
    };

    typedef std::map<std::string, ModuleParm> ModuleParmMap;

    const ModuleParmMap & getModuleParms();
    bool getModuleParm(const std::string & name, int & val_out);
    bool getModuleParm(const std::string & name, int * & ptr_out);
    bool getModuleParm(const std::string & name, std::string & val_out);

    void setModuleParm(const std::string & name, int val);
    void setModuleParm(const std::string & name, const char *val);

    /// returns the emulated FSM clock time in nanoseconds
    /// -- the emulated fsm clock only advances with calls to 
    /// heartBeat() below... and even then it only advances by
    /// quanta that are multiples of the task_rate
    hrtime_t getTime(); 

    /// returns the task rate as set in the FSM module params
    long long taskRate();

    /// iff true, make the fsm execute each task cycle as quickly as possible
    /// otherwise, the fsm sleeps for 1/taskRate() seconds in between 
    /// task cycles.  Defaults to false.
    extern bool fastFSM; 

    struct PrintFunctor 
    {
        virtual ~PrintFunctor();
        virtual void operator()(const char *) = 0;
    };

    void setPrintFunctor(PrintFunctor *functor);

    std::string readFSMProcFile();
}

#endif

#endif
