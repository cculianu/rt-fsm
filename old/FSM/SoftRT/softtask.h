#ifndef softtask_h
#define softtask_h
/**
   Soft tasks, by Calin A. Culianu <calin@ajvar.org>
   License: GPL.
*/
struct SoftTask;

typedef void (*SoftTask_Handler)(void *);

/** This creates a soft task.  
    May return null of not enough resources are available to create 
    internal data structures, etc.
    Call this only from linux process context (such as init module).  */
extern struct SoftTask *softTaskCreate(SoftTask_Handler handler_function,
                                       const char *taskName);

/** This destroys a soft task.  May block if the softtask is currently 
    running.

    Call this only from linux process context (such as cleanup module). */
extern void softTaskDestroy(struct SoftTask *);

/** This function is callable from RT, it pends the soft task
    to run in linux process context. Pass your softtask handler an argument 
    if you wish.
    
    Returns 0 on success or errno on error. 
    Errors: EINVAL, EBUSY. */
extern int softTaskPend(struct SoftTask *, void *arg);

#endif
