/**
 * Embedded C Module Wrapper -- this gets linked with every generated 
 * embedded-C .o file so that we are compatible with the linux kernel 
 * module interface.
 *
 * Calin A. Culianu <calin@ajvar.org>
 * License: GPL v2 or later.
 */
#include <linux/module.h> 
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <asm/semaphore.h> /* for synchronization primitives           */
#include <asm/bitops.h>    /* for set/clear bit                        */
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h> /* some memory copyage                       */
#include <linux/proc_fs.h>
#include <asm/div64.h>    /* for do_div 64-bit division macro          */
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/seq_file.h>
#include <linux/random.h>
#ifdef RTLINUX
#  include <mbuff.h>
#else /* RTAI */
#  include <rtai_nam2num.h>
#  include <rtai_shm.h>
#  define mbuff_alloc(name, size) rt_shm_alloc(nam2num(name), size, USE_VMALLOC)
#  define mbuff_free(name, ptr) rt_shm_free(nam2num(name))
#  define mbuff_attach(name, size) rt_shm_alloc(nam2num(name), size, USE_VMALLOC)
#  define mbuff_detach(name, ptr) rt_shm_free(nam2num(name))
#endif /*defined(RTLINUX) */

#define EMBC_MOD_INTERNAL
#include "EmbC.h"

#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif

struct EmbC *__embc = 0;

#if defined(MOD_INC_USE_COUNT) && defined(MOD_DEC_USE_COUNT)
/* Kernel 2.4 mechanism allowed for simple atomic increase of module use count
   to avoid race conditions.  */
#  define MINC MOD_INC_USE_COUNT
#  define MDEC MOD_DEC_USE_COUNT
#else 
/* Kernel 2.6 uses a different mechanism .. which *ISN'T* callable from RT-Context so we
   have to *MAKE SURE* this is linux context! */
#  define MINC do { try_module_get(THIS_MODULE); } while(0)
#  define MDEC do { module_put(THIS_MODULE); } while(0)
#endif
static void lock(void) { MINC; }
static void unlock(void) { MDEC; }
#undef MINC
#undef MDEC

int init(void)
{
  __embc = mbuff_alloc(__embc_ShmName, sizeof(struct EmbC));
  if (!__embc) {
    printk("%s: could not allocate shm!\n", __embc_ShmName);
    return ENOMEM;
  }
  memset(__embc, 0, sizeof(*__embc));
  __embc->init = __embc_init;
  __embc->cleanup = __embc_cleanup;
  __embc->statetransition = __embc_transition;
  __embc->tick = __embc_tick; /* may be NULL if user code didn't specify a function */
  __embc->entry = &__embc_fsm_do_state_entry;
  __embc->exit = &__embc_fsm_do_state_exit;
  __embc->get_at = &__embc_fsm_get_at;
  __embc->lock = &lock;
  __embc->unlock = &unlock;
  __embc->threshold_detect = __embc_threshold_detect; /* may be NULL if user code didn't specify a function! */
  return 0;
}

void cleanup(void)
{
  if (__embc) {
    memset(__embc, 0, sizeof(*__embc));
    mbuff_free(__embc_ShmName, __embc);
    __embc = 0;  
  }
}

module_init(init);
module_exit(cleanup);
