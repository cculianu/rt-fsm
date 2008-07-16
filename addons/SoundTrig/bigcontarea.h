#ifndef BIGCONTAREA_H
#define BIGCONTAREA_H

/** Allocate a contiguous region.  

    Note this *DOESN'T* use virtual memory mappings, so it doesn't have
    the same limits as vmalloc. 

    Instead, it tries to find the biggest region it can.

    HOWEVER there is no guarantee a big 
    contiguous region can be found of size `size'.

    This actually calls get_free_pages for all free ram and then searches
    the returned list for a contigious region.  It's a HORRIBLE KLUDGE, but it
    works most of the time better than vmalloc! */
extern void *bigcontarea_alloc(unsigned long size);
/** Free a pointer previously returned from bigcontarea_alloc(). */
extern void bigcontarea_free(void *mem);

#endif
