#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include "bigcontarea.h"

struct allocation
{
    struct list_head list;
    unsigned long *parray; /**< array of page addresses */
    unsigned len; /**< length of above array */   
};

LIST_HEAD(alloclist);
DEFINE_MUTEX(alloc_mut);

static void free_parray(unsigned long *parray, unsigned len, int do_free_pages)
{
    if (!parray) return;
    if (do_free_pages) {
        int i;
        for (i = 0; i < len; ++i) {
            if (parray[i]) free_pages(parray[i], 0);
        }
    }
    if (len*sizeof(unsigned long) >= (PAGE_SIZE) ) 
        vfree(parray);
    else 
        kfree(parray);
}

static int ensure_parray_capacity(struct allocation *l, unsigned newlen)
{
    unsigned long *p = 0;    
    if (l->len >= newlen) return 0;
    if (newlen*sizeof(unsigned long) >= (PAGE_SIZE)) 
        p = vmalloc(newlen * sizeof(unsigned long));
    else
        p = kmalloc(newlen * sizeof(unsigned long), GFP_KERNEL);
    if (p) memset(p, 0, newlen*sizeof(*p));
    if (p && l->parray) {
        memcpy(p, l->parray, sizeof(unsigned long)*l->len);
        free_parray(l->parray, l->len, 0);
    } else if (p) {
        memset(p, 0, sizeof(unsigned long)*newlen);
    }
    if (!p && l->parray) return 0;

    l->parray = p;
    l->len = newlen;
    return p != 0;
}

static int cmpul(const void *a, const void *b) 
{ 
    const unsigned long *al = (const unsigned long *)a, *bl = (const unsigned long *)b;
    if (*al > *bl) return 1;
    else if (*al < *bl) return -1;
    return 0;
}

static void swapul(void *a, void *b, int c)
{
	unsigned long *p1 = (unsigned long *)a,
                      *p2 = (unsigned long *)b,
		      tmp = *p1;
	*p1 = *p2;
	*p2 = tmp;
}

struct ConsecMem
{
    unsigned long start;  /**< address of first page */
    unsigned long length; /**< in bytes */
    unsigned num; /**< the number of pages, each one is of the 'diff' size passed to calcConsecAreas.. to get back to the pgsz divide length by num */
    unsigned idx; /**< index into mem array */
};

typedef struct ConsecMem ConsecMem;

/** inserts addr,ct,pgsz into ConsecMem in descending order based
    on the size of the consec mem. Returns true if it was inserted
    or false otherwise (no room in a and ct is too small to displace 
    another entry). */
static int putConsecArr(ConsecMem *a, unsigned alen, 
                        unsigned long addr,
                        unsigned long ct, 
                        unsigned long pgsz,
                        unsigned idx)
{
    int i;
    for (i = 0; i < alen; ++i) {
        /* find insert pos, note our consec_arr is sorted in reverse order
           on length */
        if (ct*pgsz > a[i].length) break;
    }
    if (i >= alen) return 0;
    /* now that we found insert pos, shift everyone else over.. */
    if (i+1 < alen) { 
        memmove(&a[i+1], &a[i], alen-i-1);
    }
    a[i].start = addr;
    a[i].length = ct*pgsz;
    a[i].num = ct;
    a[i].idx = idx;
    return 1;
}

static int calcConsecMem(ConsecMem *out_arr, unsigned num_out_arr,
                         unsigned long * arr, int num, unsigned long diff)
{
    int i, j, ct, ninarr=0;
    memset(out_arr, 0, sizeof(*out_arr)*num_out_arr);
    sort(arr, num, sizeof(unsigned long), cmpul, swapul);
    for (i = 0; i < num; i+=ct) {
        ct = 1;
        for (j = i+1; j < num; ++j) {
            if (arr[j] - arr[j-1] != diff) break;
            ++ct;
        }
        if ( putConsecArr(out_arr, num_out_arr, arr[i], ct, diff, i) )
            ++ninarr;
    }
    if (ninarr > num_out_arr) ninarr = num_out_arr;
    return ninarr;
}

void *bigcontarea_alloc(unsigned long size)
{
    int i;
    int num;
    int nconsecs;
    ConsecMem consecs[3];
    struct allocation *allocation = 0;
    struct allocation altmp;
    const unsigned nps = (size / PAGE_SIZE) + ((size % PAGE_SIZE) ? 1 : 0);

    if (size < PAGE_SIZE) {
        printk(KERN_CRIT "bigcontarea_alloc: unsupported usage -- please call this function with size > %lu\n", (unsigned long)PAGE_SIZE);
        return 0;
    }
    memset(&altmp, 0, sizeof(altmp));
    if (!ensure_parray_capacity(&altmp, nps)) {
        printk(KERN_CRIT "bigcontarea_alloc: failed to allocate the final parray! Argh!\n");
        goto out4;
    }

    allocation = kmalloc(sizeof(*allocation), GFP_KERNEL);
    if (!allocation) {
        printk(KERN_CRIT "bigcontarea_alloc: failed to allocation struct allocation!  Allocation failed!!\n");
        goto out3;
    }
    memset(allocation, 0, sizeof(*allocation));
    {/* this is a horrible kludge -- tried to allocate as many pages as 
        possible all in one run to find free memory regions.. */
        /* set num pages to be about 95% of system ram minus about 100 pages */
        struct sysinfo si;
        unsigned long r;
        si_meminfo(&si);
        r = si.freeram*si.mem_unit;
        /* NB: pointless to allocate more than freeram for now.. ? (it seems to also hang kernel)
         r = si.totalram*si.mem_unit;
        if (si.freeram*si.mem_unit + si.freeswap*si.mem_unit < r) 
            r = si.freeram*si.mem_unit + si.freeswap*si.mem_unit;
        if (r > si.totalram*si.mem_unit) r = si.totalram*si.mem_unit;
        */
        num = (r/PAGE_SIZE) - 256; /* leave about 1MB of free pages just in case */
        if (num <= 0) num = 1;
    }
    if (!ensure_parray_capacity(allocation, num)) { /* start off with array of size num */
        printk(KERN_CRIT "bigcontarea_alloc: failed to allocate the temporary page array! Argh!\n");
        goto out2;
    }
    for (i = 0; i < num; ++i) {
        if (!(allocation->parray[i] = __get_free_pages(GFP_KERNEL, 0)))
            break; /* out of memory? */ 
    }
    nconsecs = 
        calcConsecMem(consecs, 3, 
                      allocation->parray, num, PAGE_SIZE);
    if (!nconsecs) {
        printk(KERN_CRIT "bigcontarea_alloc: could not find a single consecutive run of pages! Argh!\n");
        goto out;
    }

    if (consecs[0].length <= size) {
        printk(KERN_CRIT "bigcontarea_alloc: found a consecutive run of pages, but it is of size %lu and we need %lu! Argh!\n", consecs[0].length, size);
        goto out;
    }
    
    /* now, copy just the addresses of pages we *KEEP* into
       final array, ignorting the rest of the pages we grabbed but won't keep.*/
    memcpy(altmp.parray, allocation->parray + consecs[0].idx, nps*sizeof(unsigned long));
    /* clear out pages we kept from this array so we don't free them with next call to free_parray() */
    memset(allocation->parray + consecs[0].idx, 0, nps*sizeof(unsigned long));
    /* only frees pages we didn't keep.  frees the parray itself */
    free_parray(allocation->parray, allocation->len, 1);
    allocation->parray = altmp.parray;
    allocation->len = altmp.len;

    mutex_lock(&alloc_mut);
    list_add(&allocation->list, &alloclist);
    mutex_unlock(&alloc_mut);
    return ((void *)allocation->parray[0]);

 out:
    free_parray(allocation->parray, allocation->len, 1);
 out2:
    kfree(allocation);
 out3:
    free_parray(altmp.parray, altmp.len, 0);
 out4:
    return 0;
}

void bigcontarea_free(void *mem)
{
    struct list_head *pos;
    struct allocation *allocation = 0;
    if (!mem) return;
    mutex_lock(&alloc_mut);
    list_for_each(pos, &alloclist) {
        struct allocation *al = (struct allocation *)pos;
        if (al->parray && ((void *)al->parray[0]) == mem) {
            allocation = al;
            break;
        }
    }
    if (allocation) {
        free_parray(allocation->parray, allocation->len, 1);
        list_del(&allocation->list);
        kfree(allocation);        
    } else {
        BUG_ON(!allocation);
    }
    mutex_unlock(&alloc_mut);
}
