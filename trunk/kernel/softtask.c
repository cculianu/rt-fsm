#include "softtask.h"

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/version.h>
#include <asm/atomic.h>
#include <asm/semaphore.h>

#if defined(RTLINUX) && !defined(RTAI)
/* NB: assumption is Linux 2.4 */
#  if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0)
#    error RTLinux version of this file only supports 2.4 kernels!
#  endif
#  include <rtl_tqueue.h>
#  include <rtl_core.h>
#  include <rtl_sync.h>
#  include <rtl_mutex.h>
#  define rt_spinlock_t pthread_spinlock_t
#  define RT_SPINLOCK_INITIALIZER PTHREAD_SPINLOCK_INITIALIZER
#  define rt_spin_lock_irqsave(x) pthread_spin_lock(x) /* returns flags */
#  define rt_spin_unlock_irqrestore(flags, x) do { (void)flags; pthread_spin_unlock(x); }
#elif defined(RTAI) && !defined(RTLINUX)
#  if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
#    error RTAI version of this file only supports 2.6 kernels!
#  endif
#  include <rtai.h>
#  include <rtai_sched.h>
#  include <asm/rtai_hal.h>
#  include <linux/ipipe.h>
#  include <linux/workqueue.h>
#  define MAX_IRQ IPIPE_NR_IRQS
#  define rt_spinlock_t spinlock_t
#  define RT_SPINLOCK_INITIALIZER SPIN_LOCK_UNLOCKED
#else
#  error  Need to define exactly one of RTLINUX or RTAI to use this file!
#endif

#ifdef RTLINUX
static void soft_irq_handler(int, void *, struct pt_regs *);
static void task_wrapper(void *);
#else /* RTAI */
static void soft_irq_handler(unsigned, void *);
static void task_wrapper(struct work_struct *);
#endif

struct SoftTask
{
  struct list_head list;

#define MAX_NAME_LEN 64
  char name[MAX_NAME_LEN];

  wait_queue_head_t WaitQ;
#ifdef RTLINUX
  struct tq_struct task;
#else /* RTAI, use 2.6 work queue mechanism */
  struct work_struct work;
#endif
  atomic_t busy;

  SoftTask_Handler Func;
  void *Arg;  
};

typedef struct SoftTask SoftTask;
static volatile int sirq = -1;
static LIST_HEAD(pendingSoftTasks);
static atomic_t numPendingSoftTasks = ATOMIC_INIT(0);
static atomic_t numExtantSoftTasks = ATOMIC_INIT(0);
static rt_spinlock_t spinlock = RT_SPINLOCK_INITIALIZER;
DECLARE_MUTEX(mut);

struct SoftTask *softTaskCreate(SoftTask_Handler handler_function,
                                const char *name)
{
  SoftTask *t = 0;
  int err = 0;

  down(&mut);
  
  if (sirq < 0) {
#ifdef RTLINUX
    sirq = rtl_get_soft_irq(soft_irq_handler, "SoftTaskIRQ");
#else /* RTAI */
    sirq = hal_alloc_irq();

    if ( sirq < 0 || sirq >= MAX_IRQ || (err = hal_virtualize_irq(hal_current_domain(hal_processor_id()), sirq, soft_irq_handler, 0, IPIPE_DEFAULT_MASK|IPIPE_EXCLUSIVE_MASK)) ) {
        /* Argh, out of resources..? */
        printk(KERN_CRIT "softtask: Wtf sirq is: %d MAX_IRQ is: %d  err is: %d\n", sirq, MAX_IRQ, err);
        /* argh, some error -- shouldn't happen! */
        hal_free_irq(sirq);
        sirq = -1; /* to trigger the conditional below.. */
    }
#endif
#ifdef DEBUG
    if (sirq >= 0) printk(KERN_DEBUG "softtask: reserved soft irq %d\n", sirq);
#endif
  }
   
  up(&mut);
  
  if (sirq < 0) {
      printk(KERN_CRIT "softtask: could not reserve an sirq, failure!\n");
      return 0; /* always phail! */
  }
  
  t = (SoftTask *)vmalloc(sizeof(*t));
  if (!t) {
      printk(KERN_CRIT "softtask: failed to allocate memory\n");
      return 0;
  }

  memset(t, 0, sizeof(*t));
  init_waitqueue_head(&t->WaitQ);
  atomic_set(&t->busy, 0);
#ifdef RTLINUX
  t->task.routine = task_wrapper;
  t->task.data = t;
#else /* RTAI, use new 2.6 work queue mechanism */
  INIT_WORK(&t->work, task_wrapper); 
#endif

  strncpy(t->name, name, MAX_NAME_LEN);
  t->name[MAX_NAME_LEN-1] = 0;
  t->Func = handler_function;

#ifdef DEBUG
  printk(KERN_DEBUG"softtask: created - %p\n", (void *)t);
#endif
  atomic_inc(&numExtantSoftTasks);
  return t;
}

int softTaskPend(struct SoftTask *task, void *arg)
{
  unsigned long flags;
  
  if (!task || sirq < 0) return EINVAL;

  task->Arg = arg;

  /* atomically test the busy flag and set it -- if it was 0, 
     we are ok.. if it wasn't, we are busy and we pended twice? */
  if (atomic_cmpxchg(&task->busy, 0, 1) != 0) 
    return EBUSY;

  flags = rt_spin_lock_irqsave(&spinlock);
  
  list_add(&task->list, &pendingSoftTasks);
  atomic_inc(&numPendingSoftTasks);
  
  rt_spin_unlock_irqrestore(flags, &spinlock);
  
#ifdef RTLINUX
  rtl_global_pend_irq(sirq);
#else /* RTAI */
  rt_pend_linux_irq(sirq);
#endif

  return 0;
}

void softTaskDestroy(struct SoftTask *t)
{
  if (!t) return; /* paranoia */

  if (atomic_dec_and_test(&numExtantSoftTasks)) { /* true iff zero */
    down(&mut);
    
#ifdef RTLINUX
    rtl_free_soft_irq(sirq);
#else /* RTAI */
    hal_virtualize_irq(hal_current_domain(hal_processor_id()), sirq, 0, 0, 0);
    hal_free_irq(sirq);
#endif
#ifdef DEBUG
    printk(KERN_DEBUG "softtask: last soft task freed, releasing soft irq %d\n", sirq);
#endif
    sirq = -1;
    
    up(&mut);
  }
#ifdef RTAI
  flush_scheduled_work(); /* globally flush queue? */
#endif
  wait_event(t->WaitQ, !atomic_read(&t->busy)); /* wait for pended work to complete.. */
  memset(t, 0, sizeof(*t));
  vfree(t);
}

#ifdef RTLINUX
static void soft_irq_handler(int irq, void *arg, struct pt_regs *regs)
#else /* RTAI */
static void soft_irq_handler(unsigned irq, void *arg)
#endif
{
  SoftTask *task = 0;
  LIST_HEAD(list);
  struct list_head *pos, *n;
  unsigned long flags;
  (void) arg; 
#ifdef RTLINUX
  (void)regs; /* ignore unused params.. */
#endif
  
  if (sirq != irq) {
    printk(KERN_WARNING "softtask: sirq %d != handled irq %u\n", sirq, irq);
  }
  
  flags = rt_spin_lock_irqsave(&spinlock);
  list_splice_init(&pendingSoftTasks, &list); /* copy in the list of pending tasks in O(1) */
  rt_spin_unlock_irqrestore(flags, &spinlock);
  
  list_for_each_safe(pos, n, &list) {
      task = (SoftTask *)pos;
      list_del(pos);
      atomic_dec(&numPendingSoftTasks);
#ifdef RTLINUX
      schedule_task(&task->task);  
#else /* RTAI */
      /*printk(KERN_DEBUG" softtask DEBUG1 - irq %u  task %p  work %p\n", irq, (void *)task, &task->work);*/
      schedule_work(&task->work);
#endif
  }
}

#ifdef RTLINUX
static void task_wrapper(void *arg)
{
  SoftTask *task = (SoftTask *) arg;

  task->Func(task->Arg);
  atomic_set(&task->busy, 0);
  wake_up(&task->WaitQ);
}
#else /* RTAI */
static void task_wrapper(struct work_struct *work)
{
  SoftTask *task = 0;
  /* ARGH - linux work queues don't save any cookie/arg data!  So we need to play some pointer tricks to get back to the SoftTask pointer! */
  {
      unsigned long offset = ((char *)&task->work) - ((char *)task);
      task = (SoftTask *)(((char *)work)-offset);
  }
  /*printk(KERN_DEBUG" softtask DEBUG2 - task %p  work %p\n", (void *)task, (void *)work);*/
  task->Func(task->Arg);
  atomic_set(&task->busy, 0);
  wake_up(&task->WaitQ);	
}
#endif

