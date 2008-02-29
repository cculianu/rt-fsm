/** @file SoftRTSoundTrig.c
 * This is a simple module that accepts hard realtime 'external trigger' events
 * from the FSM and forwards them to userspace via a RT-FIFO.  Userspace then
 * handles them in non-realtime, presumably.
 */
/***************************************************************************
 *   Copyright (C) 2008 by Calin A. Culianu   *
 *   cculianu@yahoo.com   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#include "Version.h"
#include "UserspaceExtTrig.h"
#include "rtos_compat.h"
#include "FSMExternalTrig.h"
#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/atomic.h>

#define MODULE_NAME KBUILD_MODNAME
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif

MODULE_AUTHOR("Calin A. Culianu <calin@ajvar.org>");
MODULE_DESCRIPTION("A module that simply forwards kernelspace FSM ext-trig requests to userspace via a fifo.");
int debug = 0;
module_param(debug, int, 0444);
MODULE_PARM_DESC(debug, "If true, print verbuse debug messages to kernel console.  Defaults to 0 (false).");

static UTShm *shm = 0;
static volatile FSMExtTrigShm *extShm = 0;

static int initShm(void);
static void cleanupShm(void);
static int init(void);
static void cleanup(void);
#define ABS(x) (x < 0 ? -x : x)

static int trigFunc(unsigned, int);
        
int init(void)
{
    int ret;
    if ( (ret = initShm()) ) {
        cleanup();
        return -ABS(ret);
    }
    
    atomic_set(&extShm->valid, 1);

    printk(KERN_INFO MODULE_NAME ": Loaded ok,  using rtf %u to talk to userspace\n", shm->fifo_out);
    return 0;
}

void cleanup(void)
{
    cleanupShm();
    printk(KERN_INFO MODULE_NAME ": Unloaded.\n");
}

module_init(init);
module_exit(cleanup);

static int initFifos(void);
static void cleanupFifos(void);

static int  initShm(void)
{
    int ret;
    shm = (UTShm *)mbuff_alloc(UT_SHM_NAME, UT_SHM_SIZE);
    if (!shm) {
        printk(KERN_ERR MODULE_NAME ": Could not create shm %s\n", UT_SHM_NAME);
        return -EINVAL;
    }
    ret = initFifos();

    if (!ret) {
        
        extShm = (FSMExtTrigShm *)mbuff_attach(FSM_EXT_TRIG_SHM_NAME, FSM_EXT_TRIG_SHM_SIZE);
        if (!extShm) return -EINVAL;
        if (FSM_EXT_TRIG_SHM_IS_VALID(extShm)) {
            printk(KERN_ERR MODULE_NAME ": The FSM already has an external trigger handler installed!\n");
            mbuff_detach(FSM_EXT_TRIG_SHM_NAME, extShm);
            extShm = 0;
            return -EBUSY;
        }
        extShm->function = &trigFunc;
        extShm->magic = FSM_EXT_TRIG_SHM_MAGIC;
        atomic_set(&extShm->valid, 0); /* force invalid for now.. */
        shm->magic = UT_SHM_MAGIC;
    }
    
    return ret;
}

static void cleanupShm(void)
{
    if (extShm) {
        atomic_set(&extShm->valid, 0);
        extShm->function = 0;
        extShm->magic = 0;
        mbuff_detach(FSM_EXT_TRIG_SHM_NAME, extShm);
        extShm = 0;
    }
    if (shm)  {
        cleanupFifos();
        mbuff_free(UT_SHM_NAME, shm); 
        shm = 0; 
    }
}

static int  initFifos(void)
{
    unsigned minor = 0;
    int ret;

    shm->fifo_out = -1;

    ret = rtf_find_free(&minor, UT_FIFO_SZ);
    if (ret) {
        printk(KERN_ERR MODULE_NAME ": Could not create fifo\n");
        return ret;
    }
    shm->fifo_out = minor;
    return 0; 
}

static void cleanupFifos(void)
{
    if (shm->fifo_out >= 0) {
        rtf_destroy(shm->fifo_out);
        shm->fifo_out = -1;
    }
}

static int trigFunc(unsigned which, int data)
{
    struct UTFifoMsg msg;
    int ret;
    
    if (debug)
        rt_printk(KERN_DEBUG MODULE_NAME ": Got trigger %u,%d\n", which, data);
    msg.target = which;
    msg.data = data;
    ret = rtf_put(shm->fifo_out, &msg, sizeof(msg));
    if (ret < 0) {
        rt_printk(KERN_ERR MODULE_NAME ": rtf_put() returned error: %d\n", ret);
        return 0;
    }
    else if (ret != sizeof(msg))
        rt_printk(KERN_WARNING MODULE_NAME ": rtf_put() returned %d, but we expected %d!\n", ret, (int)sizeof(msg));
    return 1;
}
