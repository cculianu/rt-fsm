#include "softtask.h"

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/version.h>
#include <asm/atomic.h>

#if defined(RTLINUX) && !defined(RTAI)
/* NB: assumption is Linux 2.4 */
#  if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0)
#    error RTLinux version of this file only supports 2.4 kernels!
#  endif
#  include <rtl_tqueue.h>
#  include <rtl_core.h>
#  include <rtl_sync.h>
#elif defined(RTAI) && !defined(RTLINUX)
#  if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
#    error RTAI version of this file only supports 2.6 kernels!
#  endif
#  include <asm/rtai_hal.h>
#  include <linux/ipipe.h>
#  include <linux/workqueue.h>
#  define MAX_IRQ IPIPE_NR_IRQS
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

  int sirq;
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

static SoftTask *irqTasks[MAX_IRQ]; /* Table mapping irq -> SoftTask * */
static volatile int did_init = 0;

struct SoftTask *softTaskCreate(SoftTask_Handler handler_function,
                                const char *name)
{
  SoftTask *t = 0;
  int err = 0;

  if (!did_init) {
    memset(irqTasks, 0, sizeof(*irqTasks)*MAX_IRQ);
    did_init = 1;
  }

  t = (SoftTask *)vmalloc(sizeof(*t));
  if (!t) return 0;

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

#ifdef RTLINUX
  t->sirq = rtl_get_soft_irq(soft_irq_handler, t->name);
#else /* RTAI */
  t->sirq = hal_alloc_irq();

  if ( t->sirq < 0 || t->sirq >= MAX_IRQ || (err = hal_virtualize_irq(hal_current_domain(hal_processor_id()), t->sirq, soft_irq_handler, 0, IPIPE_DEFAULT_MASK|IPIPE_EXCLUSIVE_MASK)) ) {
    /* Argh, out of resources..? */
    printk(KERN_CRIT "  softtask - Wtf sirq is: %d MAX_IRQ is: %d  err is: %d\n", t->sirq, MAX_IRQ, err);
    /* argh, some error -- shouldn't happen! */
    hal_free_irq(t->sirq);
    t->sirq = -1; /* to trigger the conditional below.. */
  }
#endif

  if (t->sirq < 0) {
    vfree(t);
    return 0;
  }

  irqTasks[t->sirq] = t;
  /*printk(KERN_DEBUG" softtask created - %p\n", (void *)t);*/
  return t;
}

int softTaskPend(struct SoftTask *task, void *arg)
{
  if (!task || !irqTasks[task->sirq]) return EINVAL;

  task->Arg = arg;

  /* atomically test the busy flag and set it -- if it was 0, 
     we are ok.. if it wasn't, we are busy and we pended twice? */
  if (atomic_cmpxchg(&task->busy, 0, 1) != 0) 
    return EBUSY;

#ifdef RTLINUX
  rtl_global_pend_irq(task->sirq);
#else /* RTAI */
  rt_pend_linux_irq(task->sirq);
#endif

  return 0;
}

void softTaskDestroy(struct SoftTask *t)
{
  if (!t) return; /* paranoia */

#ifdef RTLINUX
  rtl_free_soft_irq(t->sirq);
#else /* RTAI */
  hal_virtualize_irq(hal_current_domain(hal_processor_id()), t->sirq, 0, 0, 0);
  hal_free_irq(t->sirq);
#endif
  irqTasks[t->sirq] = 0;
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
  (void) arg; 
#ifdef RTLINUX
  (void)regs; /* ignore unused params.. */
#endif
  if (irq >= 0 && irq < MAX_IRQ)
    task = irqTasks[irq];

  if (task) 
#ifdef RTLINUX
    schedule_task(&task->task);  
#else /* RTAI */
    /*printk(KERN_DEBUG" softtask DEBUG1 - irq %u  task %p  work %p\n", irq, (void *)task, &task->work);*/
    schedule_work(&task->work);
#endif
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
      SoftTask dummy;
      unsigned long offset = ((char *)&dummy.work) - ((char *)&dummy);
      task = (SoftTask *)(((char *)work)-offset);
  }
  /*printk(KERN_DEBUG" softtask DEBUG2 - task %p  work %p\n", (void *)task, (void *)work);*/
  task->Func(task->Arg);
  atomic_set(&task->busy, 0);
  wake_up(&task->WaitQ);	
}
#endif

