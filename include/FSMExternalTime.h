#ifndef FSMExternalTime_H
#define FSMExternalTime_H

#ifndef __KERNEL__
#   error This is a kernel header, and is not really compatible or intended to be used by userspace programs!
#endif

#define FSM_EXT_TIME_SHM_NAME "FTimeSHM"
#define FSM_EXT_TIME_SHM_MAGIC 0x123463fe
#define FSM_EXT_TIME_SHM_SIZE (sizeof(struct FSMExtTimeShm))
#define FSM_EXT_TIME_SHM_IS_VALID(s) (s->magic == FSM_EXT_TIME_SHM_MAGIC && s->func)
#define FSM_EXT_TIME_GET(s) (s && FSM_EXT_TIME_SHM_IS_VALID(s) ? s->func() : 0ULL)

/** Typedef for the virtual function used to get the external reference time.
    This virtual function should return a timestamp expressed
    in nanoseconds. */
typedef unsigned long long (*GetTime_Func)(void);

struct FSMExtTimeShm
{
  int magic;
  GetTime_Func func;
};

#endif


