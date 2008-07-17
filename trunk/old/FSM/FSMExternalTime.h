#ifndef FSMExternalTime_H
#define FSMExternalTime_H

#define FSM_EXT_TIME_SHM_NAME "FSMExtTime"
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


