#include "softtask.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void *task_wrapper(void *);

struct SoftTask
{
#define MAX_NAME_LEN 64
  char name[MAX_NAME_LEN];

  pthread_t thread;
  sem_t sem;
  
  SoftTask_Handler Func;
  volatile void *Arg;  
};

typedef struct SoftTask SoftTask;


struct SoftTask *softTaskCreate(SoftTask_Handler handler_function,
                                const char *name)
{
  SoftTask *t = (SoftTask *)malloc(sizeof(*t));
  if (!t) return 0;

  memset(t, 0, sizeof(*t));
  strncpy(t->name, name, MAX_NAME_LEN-1);
  sem_init(&t->sem, 0, 0);
  t->Func = handler_function;
  if ( pthread_create(&t->thread, 0, task_wrapper, t) ) {
    free(t);
    return 0;
  }
  return t;
}

int softTaskPend(struct SoftTask *task, void *arg)
{
  int sval;
  if (!task) return EINVAL;
  sem_getvalue(&task->sem, &sval);
  if (sval) return EBUSY;
  task->Arg = arg;
  sem_post(&task->sem);
  
  return 0;
}

void softTaskDestroy(struct SoftTask *t)
{
  if (!t) return; /* paranoia */

  pthread_cancel(t->thread);
  pthread_join(t->thread, 0);
  sem_destroy(&t->sem);
  memset(t, 0, sizeof(*t));
  free(t);
}

static void *task_wrapper(void *arg)
{
  SoftTask *task = (void *) arg;  

  while (1) {
    sem_wait(&task->sem); /* this is a cancellation point */
    task->Func((void *)task->Arg);
    pthread_testcancel(); /* so is this.. */
  }
  return 0; /* never reached */
}


