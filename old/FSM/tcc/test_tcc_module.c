
#include <linux/module.h>
#include <linux/kernel.h>
#include "libtcc.h"


static const char code[] = 
"extern int printk(const char *fmt, ...);\n"
"int foo(void) { printk(\"foo() called!\\n\"); return 1234; }\n";

TCCState *st = 0;

int init(void)
{
  int err = 0;
  st = tcc_new();
  if (st) {    
    int (*f)(void) = 0;
    err = tcc_compile_string(st, code);
    if (err) {
      printk("tcc_compile_string returned %d\n", err);
      return err;
    }

    err = tcc_add_symbol(st, "printk", (unsigned long)(&printk));
    if (err) {
        printk("tcc_add_symbol returned %d\n", err);
        return err;
    }

    err = tcc_relocate(st);
    if (err) {
      printk("tcc_relocate returned %d\n", err);
      return err;
    }
    
    err = tcc_get_symbol(st, (unsigned long *)&f, "foo");
    if (err) {
      printk("tcc_get_symbol returned %d\n", err);
      return err;
    }

    f(); /* run the compiled function! */

  } else {

    printk("tcc_new returned NULL!\n");
    return 1;

  }

  return err;
}

void cleanup(void)
{
  if (st)   tcc_delete(st);
  st = 0;
}

module_init(init);
module_exit(cleanup);
