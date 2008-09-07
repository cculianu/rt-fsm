#include "kernel_emul.h"
#undef clock_nanosleep
#undef timespec_add_ns
#undef clock_gettime
#include "rtos_utility.h"
#include <stdarg.h>
#include <stdio.h> /* for now, all rt_printks go to stdout.. */
#include <string.h>
#include "ComediEmul.h"
#include <sstream>
#include <semaphore.h>
#ifdef OS_WINDOWS
#include <windows.h>
#endif
#include <sys/time.h>

namespace Emul {

    static ModuleParmMap *moduleParms = 0;

    static std::ostringstream procFileEmulStream;

    std::string readFSMProcFile()
    {
        procFileEmulStream.str("");
        if (fsm_seq_show_func) {
            fsm_seq_show_func(0, 0);            
        }
        return procFileEmulStream.str();
    }
    
    static unsigned long long tickCount = 0;
    static int *taskRate_ptr = 0;

    long long taskRate()
    {
        if (!taskRate_ptr) {
            if (!getModuleParm("task_rate", taskRate_ptr)) {
                fprintf(stderr, "MAJOR ERROR: No fsm task_rate module parm found in Emul::taskRate()\n");
            }
        }
        if (taskRate_ptr) return *taskRate_ptr;
        return 6000; // default if error above
    }
    
    hrtime_t getTime() 
    {
        return tickCount * (1000000000LL/taskRate());
    }


    /*
    static unsigned long long time2TickCount(hrtime_t t)
    {
        return static_cast<unsigned long long>(t/(1000000000LL/taskRate()));
    }

    struct Sleeper 
    {
        sem_t sem;
        unsigned long long wakeup; ///< tick at which to wakeup
        Sleeper() : wakeup(0)  {  sem_init(&sem, 0, 0);     }
        ~Sleeper() {  sem_destroy(&sem);   }
    };

    typedef std::multimap<unsigned long long, Sleeper *> SleeperMap;

    static SleeperMap sleepers;
    static pthread_mutex_t sleeperLock = PTHREAD_MUTEX_INITIALIZER;
  
    void heartBeat() 
    {
        ++tickCount; // advance the tick count
        SleeperMap::iterator it;
        pthread_mutex_lock(&sleeperLock);
        // now wake all sleepers whose wakeuptime <= current tick count
        while ( (it = sleepers.begin()) != sleepers.end() && it->first <= tickCount ) {
            Sleeper *s = it->second;
            sleepers.erase(it);
            sem_post(&s->sem);
        }
        pthread_mutex_unlock(&sleeperLock);
    }
    */
    static void heartBeat() { ++tickCount; }

    bool fastFSM = false;

#ifdef OS_OSX
    int clock_gettime(int clkid, struct timespec *ts)
    {
        struct timeval tv;
        int ret = gettimeofday(&tv, 0);
        (void)clkid;
        TIMEVAL_TO_TIMESPEC(&tv, ts);
        return ret;
    }
    static pthread_cond_t nscond = PTHREAD_COND_INITIALIZER;
    static pthread_mutex_t nsmut = PTHREAD_MUTEX_INITIALIZER;
    void nanosleep(unsigned long nanos)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += nanos;
        while (ts.tv_nsec >= 1000000000) ++ts.tv_sec, ts.tv_nsec -= 1000000000;
        pthread_mutex_lock(&nsmut);
        pthread_cond_timedwait(&nscond, &nsmut, &ts);
        pthread_mutex_unlock(&nsmut);
    }
#elif defined(WIN32) && !defined(CYGWIN)
    void nanosleep(unsigned long nanos)
    {
        HANDLE hTimer = NULL;
        LARGE_INTEGER liDueTime;

        liDueTime.QuadPart = -static_cast<long long>(nanos/100UL);

        // Create a waitable timer.
        static volatile int i = 0;
        std::ostringstream os;
        os << "WaitableTimer" << ++i;
        hTimer = CreateWaitableTimerA(NULL, TRUE, os.str().c_str());
        if (hTimer && SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, 0))
        {
            // Wait for the timer.
            WaitForSingleObject(hTimer, INFINITE);
        }

        CloseHandle(hTimer);
    }
#else
    void nanosleep(unsigned long nanos)
    {
        static sem_t sem;
        static bool didinit = false;
        if (!didinit) {
            didinit = true;
            sem_init(&sem, 0, 0);
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += nanos;
        while (ts.tv_nsec >= 1000000000) ++ts.tv_sec, ts.tv_nsec -= 1000000000;
        sem_timedwait(&sem, &ts);
    }
#endif

    const ModuleParmMap & getModuleParms()
    {
        static const ModuleParmMap dummyMap;
        return moduleParms ? *moduleParms : dummyMap;
    }

    bool getModuleParm(const std::string & name, int & val_out)
    {
        const ModuleParmMap & m = getModuleParms();
        ModuleParmMap::const_iterator it = m.find(name);
        if (it != m.end() && it->second.type == ModuleParm::Int) {
            val_out = *reinterpret_cast<int *>(it->second.p);
            return true;
        }
        return false;
    }

    bool getModuleParm(const std::string & name, int * & val_out)
    {
        const ModuleParmMap & m = getModuleParms();
        ModuleParmMap::const_iterator it = m.find(name);
        if (it != m.end() && it->second.type == ModuleParm::Int) {
            val_out = reinterpret_cast<int *>(it->second.p);
            return true;
        }
        return false;
    }

    bool getModuleParm(const std::string & name, std::string & val_out)
    {
        const ModuleParmMap & m = getModuleParms();
        ModuleParmMap::const_iterator it = m.find(name);
        if (it != m.end() && it->second.type == ModuleParm::CharP) {
            val_out = *reinterpret_cast<char **>(it->second.p);
            return true;
        }
        return false;
    }

    PrintFunctor::~PrintFunctor() { /* noop */ }

    void setModuleParm(const std::string & name, int val)
    {
        bool found = false;
        if (moduleParms) {
            ModuleParmMap::iterator it = moduleParms->find(name);
            if (it != moduleParms->end()) {
                if (it->second.type == ModuleParm::Int) {
                    int & parm = *reinterpret_cast<int *>(it->second.p);
                    parm = val;
                    found = true;
                }
            }
        }
        if (!found) printf("module parm %s not set!\n", name.c_str());
    }

    void setModuleParm(const std::string & name, const char *val)
    {
        if (moduleParms) {
            ModuleParmMap::iterator it = moduleParms->find(name);
            if (it != moduleParms->end()) {
                if (it->second.type == ModuleParm::CharP) {
                    char * & parm = *reinterpret_cast<char **>(it->second.p);
                    parm = strdup(val); // HACK! WARNING! FIXME!! Possible memory leak here!
                }
            }
        }        
    }

    static PrintFunctor *printFunctor = 0;
    
    void setPrintFunctor(PrintFunctor *functor)
    {
        printFunctor = functor;
    }
} 



extern "C" {

/* mbuff stuff */
void *mbuff_alloc(const char *name, unsigned long size)
{
    return RTOS::shmAttach(name, size, 0, true);    
}
void *mbuff_attach(const char *name, unsigned long size)
{
    return RTOS::shmAttach(name, size, 0, false);    
}
void  mbuff_detach(const char *name, void *mem)
{
    (void)name;
    RTOS::shmDetach(mem, 0, false);
}
void  mbuff_free(const char *name, void *mem)
{
    (void)name;
    RTOS::shmDetach(mem, 0, true);
}

/* fifo stuff */
int rtf_find_free(unsigned *minor_out, unsigned long size)
{    
    return 0 == RTOS::createFifo(*minor_out, size);
}
int rtf_destroy(unsigned fifo)
{
    RTOS::closeFifo(fifo);
    return 0;
}
int rtf_put(unsigned fifo, const void *data, unsigned long size)
{
    return RTOS::writeFifo(fifo, data, size, false);
}
int rtf_get_if(unsigned fifo, void *data, unsigned long size)
{
    return RTOS::readFifo(fifo, data, size, false);
}
int RTF_FREE(unsigned minor)
{
    rt_printk("INTENRAL ERROR: RTF_FREE unimplemented!\n");
    (void)minor;
    return 0;
}

static pthread_mutex_t critical_mutex = PTHREAD_MUTEX_INITIALIZER;
long rt_start_critical(void)
{
    pthread_mutex_lock(&critical_mutex);
    return 0;
}
void rt_end_critical(long flags)
{
    (void)flags;
    pthread_mutex_unlock(&critical_mutex);
}

struct SoftTask
{
    SoftTask_Handler handler;
    std::string name;
    void *arg;
    static void *fun(void *arg) {
        SoftTask *s = reinterpret_cast<SoftTask *>(arg);
        s->handler(s->arg);
        return 0;
    }
};

SoftTask *softTaskCreate(SoftTask_Handler fun, const char *task)
{
    SoftTask *s = new SoftTask;
    s->name = task;
    s->handler = fun;
    return s;
}

void softTaskDestroy(struct SoftTask *s)
{
    s->handler = 0;
    s->name = "";
    delete s;
}

int softTaskPend(struct SoftTask *s, void *arg)
{
    pthread_t thr;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    s->arg = arg;
    int ret = pthread_create(&thr, &attr, SoftTask::fun, reinterpret_cast<void *>(s));
    pthread_attr_destroy(&attr);
    return ret;
}

comedi_t *comedi_open(const char *devfile)
{
    return ComediEmul::getInstance(devfile);
}
int comedi_close(comedi_t *d)
{
    ComediEmul::putInstance(d);
    return 0;
}
int comedi_lock(comedi_t *d, unsigned sd)
{
    (void)d; (void)sd; return -ENOSYS;
}
int comedi_unlock(comedi_t *d, unsigned sd)
{
    (void)d; (void)sd; return -ENOSYS;
}
int comedi_cancel(comedi_t *d, unsigned sd)
{
    (void)d; (void)sd; return -ENOSYS;
}
int comedi_find_subdevice_by_type(comedi_t *dev, int type, unsigned first_sd)
{
    return dev->findByType(type, first_sd);
}
int comedi_get_n_channels(comedi_t *dev, unsigned sd)
{
    return dev->getNChans(sd);
}
int comedi_get_n_ranges(comedi_t *dev, unsigned sd, unsigned ch)
{
    return dev->getNRanges(sd, ch);
}
int comedi_get_krange(comedi_t *dev, unsigned sd, unsigned ch, unsigned r, comedi_krange *out)
{
    return dev->getKRange(sd, ch, r, out);
}
lsampl_t comedi_get_maxdata(comedi_t *dev, unsigned sd, unsigned ch)
{
    return dev->getMaxData(sd, ch);
}
int comedi_dio_config(comedi_t *dev,unsigned int subdev,unsigned int chan,
                      unsigned int io)
{
    return dev->dioConfig(subdev, chan, io);
}
int comedi_data_write(comedi_t *dev,unsigned int subdev,unsigned int chan,
                      unsigned int range,unsigned int aref,lsampl_t data)
{
    (void)aref;
    return dev->dataWrite(subdev, chan, range, data);
}
int comedi_data_read(comedi_t *dev,unsigned int subdev,unsigned int chan,
                     unsigned int range,unsigned int aref,lsampl_t *data)
{
    (void)aref;
    return dev->dataRead(subdev, chan, range, data);
}

int comedi_dio_bitfield(comedi_t * dev, unsigned subdev, unsigned mask,
                        unsigned *bits)
{
    return dev->dioBitField(subdev, mask, bits);
}

int rt_printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    int ret = vsnprintf(buf, sizeof(buf)-1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf)-1] = 0;
    if (ret >= 0 && Emul::printFunctor) {
        (*Emul::printFunctor)(buf);
    }
    return ret;
}


void set_bit(unsigned bit, volatile unsigned long *bits)
{
    unsigned word = bit/(sizeof(*bits)*8), rem =  bit%(sizeof(*bits)*8);
    bits[word] |= (1UL<<rem);

}

#define BITS_PER_LONG (sizeof(long)*8)
#define BITOP_WORD(nr)          ((nr) / BITS_PER_LONG)

/**
 * find_next_bit - find the next set bit in a memory region
 * @addr: The address to base the search on
 * @offset: The bitnumber to start searching at
 * @size: The maximum size to search
 */
unsigned long find_next_bit(const unsigned long *addr, unsigned long size,
                unsigned long offset)
{
        const unsigned long *p = addr + BITOP_WORD(offset);
        unsigned long result = offset & ~(BITS_PER_LONG-1);
        unsigned long tmp;

        if (offset >= size)
                return size;
        size -= result;
        offset %= BITS_PER_LONG;
        if (offset) {
                tmp = *(p++);
                tmp &= (~0UL << offset);
                if (size < BITS_PER_LONG)
                        goto found_first;
                if (tmp)
                        goto found_middle;
                size -= BITS_PER_LONG;
                result += BITS_PER_LONG;
        }
        while (size & ~(BITS_PER_LONG-1)) {
                if ((tmp = *(p++)))
                        goto found_middle;
                result += BITS_PER_LONG;
                size -= BITS_PER_LONG;
        }
        if (!size)
                return result;
        tmp = *p;

found_first:
        tmp &= (~0UL >> (BITS_PER_LONG - size));
        if (tmp == 0UL)         /* Are any bits set? */
                return result + size;   /* Nope. */
found_middle:
        return result + __ffs(tmp);
}

#define BITOP_MASK(nr)          (1UL << ((nr) % BITS_PER_LONG))

void clear_bit(int nr, volatile unsigned long *addr)
{
        unsigned long mask = BITOP_MASK(nr);
        unsigned long *p = ((unsigned long *)addr) + BITOP_WORD(nr);

        *p &= ~mask;
}

u32 random32(void)
{
    static bool rand_seeded = false;
    if (!rand_seeded) {
        srand(time(0));
        rand_seeded = true;
    }
    double r = (double)rand();
    return u32(r/RAND_MAX * 0xffffffffU);
}

static int mpctr = 0;

int register_module_param(const char *name, void *param, const char *type)
{
    using namespace Emul;
    ModuleParm mp;
    if (!moduleParms) moduleParms = new ModuleParmMap;
    ModuleParmMap::iterator it = moduleParms->find(name);
    if (it != moduleParms->end()) mp = it->second;
    mp.p = param;
    mp.name = name;    
    mp.type = std::string(type) == "int" ? ModuleParm::Int : ModuleParm::CharP;
    (*moduleParms)[name] = mp;
    return ++mpctr;
}

int register_module_param_desc(const char *name, void *param, const char *desc)
{
    using namespace Emul;
    if (!moduleParms) moduleParms = new ModuleParmMap;
    ModuleParmMap::iterator it = moduleParms->find(name);
    if (it == moduleParms->end()) {
        (*moduleParms)[name];
        it = moduleParms->find(name);
    }
    ModuleParm & mp = it->second;
    mp.name = name;
    mp.p = param;
    mp.desc = desc;
    return ++mpctr;
}

hrtime_t gethrtime(void)
{
    return static_cast<hrtime_t>(Emul::getTime());
}

static long long timeNowNS()
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return static_cast<long long>(tv.tv_sec) * 1000000000LL + static_cast<long long>(tv.tv_usec)*1000LL;
}

int clock_nanosleep_emul(int clkid, int flags, const struct timespec *req, struct timespec *rem)
{
    (void)clkid; (void) flags; (void)req; (void)rem;
    /* NB: this function is really severely limited: it unconditionally 
       waits for 1 clock cycle regardless of what's passed-in */ 

    // try and sleep as close to 1 task cycle as possible, by remembering
    // the last time we woke and sleep by "timeleft this period" amount..
    
    static long long lastWake = 0;
    if (!Emul::fastFSM) {
        long tick = 1000000000/Emul::taskRate(), amount = tick;
        if (lastWake) {
            amount -= timeNowNS()-lastWake;
            if (amount < 0) amount = 0;
            if (amount > tick) amount = tick;
        }
        Emul::nanosleep(amount);
    }
    lastWake = timeNowNS();
    Emul::heartBeat();
    return 0;
}

void timespec_add_ns(struct timespec *ts, long ns)
{
    ts->tv_nsec += ns;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec += ts->tv_nsec / 1000000000;
        ts->tv_nsec = ts->tv_nsec % 1000000000;
    }
}

int clock_gettime_emul(int clkid, struct timespec *ts)
{
    (void)clkid;
    hrtime_t t = Emul::getTime();
    ts->tv_sec = static_cast<time_t>(t/1000000000);
    hrtime_t nsec = t - (static_cast<hrtime_t>(ts->tv_sec)*1000000000LL);
    if (nsec >= 1000000000LL) ts->tv_sec += static_cast<time_t>(nsec/1000000000LL);
    ts->tv_nsec = static_cast<time_t>(nsec) % static_cast<unsigned long>(1e9);
    return 0;
}

int seq_printf(struct seq_file *m, const char *fmt, ...) 
{
    (void)m;
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (ret > -1) {
        buf[sizeof(buf)-1] = 0;
        Emul::procFileEmulStream << buf;
    }
    return ret;
}

const char * GetTmpPath(void)
{
    static std::string tmp = "";
    if (!tmp.length()) {
#if defined(OS_WINDOWS)
        char buf[256];    
        GetTempPathA(sizeof(buf), buf);
        buf[sizeof(buf)-1] = 0;
        tmp = buf;
#else
        tmp = "/tmp/";
#endif
    }
    return tmp.c_str();
}

}

int (*fsm_seq_show_func)(struct seq_file *m, void *dummy) = 0;

