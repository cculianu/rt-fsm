/**
 * Embedded C .so Wrapper -- this gets linked with every generated 
 * embedded-C .so file so that we have hooks to call into this code.
 *
 * Calin A. Culianu <calin@ajvar.org>
 * License: GPL v2 or later.
 */
#include <string.h>
#define mbuff_alloc __x1
#define mbuff_free __x2
#define rt_printk __x3
#include "kernel_emul.h"
#undef mbuff_alloc
#undef mbuff_free
#undef rt_printk

#define EMBC_MOD_INTERNAL
#include "EmbC.h"

struct EmbC *__embc = 0;

static void lock(void) { /* noop for now? */ }
static void unlock(void) { /* noop for now? */ }
void *(*mbuff_alloc)(const char *, unsigned long) = 0;
void (*mbuff_free)(const char *, void *) = 0;
int (*rt_printk)(const char *, ...) = 0;

int init(void)
{
  __embc = mbuff_alloc(__embc_ShmName, sizeof(struct EmbC));
  if (!__embc) {
    rt_printk("%s: could not allocate shm!\n", __embc_ShmName);
    return -ENOMEM;
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
