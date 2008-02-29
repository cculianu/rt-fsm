#ifndef HALENV_LINUX_H
#define HALENV_LINUX_H

/* C interface to Linux, meant to be called from C++ code like LinuxGlue.cpp */


#include "LynxTWO-RT.h" /* for L22_* defines.. */

#define LOG_MSG(x...) linux_message(x)
#define WARNING(x...) linux_warning(x)
#define ERROR(x...) linux_error(x)
#ifndef NDEBUG
#  define DEBUG_MSG(x...) do { if (debug) linux_debug(x); } while(0)
#else
#  define DEBUG_MSG(x...) do { } while(0)
#endif
#ifdef __cplusplus
extern "C" {
#endif

  struct pci_pool;

  struct MemRegion
  {
    void *virt;
    unsigned long phys;
    unsigned long len;
  };

  struct DMAInfo
  {
    struct pci_pool *pool2k; /* 2048-byte blocks, aligned to 2048 bytes */
    struct pci_pool *poolDMA; /* DMABufferSize-byte blocks, DWORD-aligned */
    
      /* The addess and length of the DMA BufferList pointer... the only thing
         the HAL allocates using exported functions..  
         it is in the 2k pool above.. */
    struct MemRegion DBL; /* 2048-byte buffer list given to DMA controller.. */

    /* Our audio DMA stream buffers.. max out at 16? */
#   define NUM_DMA_AUDIO_BUFS 2
    struct MemRegion audio[L22_NUM_CHANS][NUM_DMA_AUDIO_BUFS];
  };
  typedef struct MemRegion MMIORegion;
  struct Private; /* A C++ Private structure that is opaque to C. */
  struct ThreadData; /* A C struct that is private and is in LinuxModule.c */
  struct L22Lock; /* A C struct that is private and is in LinuxModule.c */
  struct LinuxContext 
  {
    struct pci_dev *dev;
    struct DMAInfo dma;
#   define MAX_REGIONS 5
    MMIORegion regions[MAX_REGIONS]; /* BADR Regions, not dma regions..    */
    struct Private *priv; /* A C++ private struct that's up to LinuxGlue.cpp */
    struct ThreadData *td; /* A struct that is private and is in LinuxModule.c 
                              mnemonic: 'thread data' */
    struct L22Lock *lock;
    int did_lynx_startup;
    const struct pci_device_id *id;
    unsigned adapter_num;
    unsigned region_ct;
  };

#  ifndef __cplusplus
     typedef struct LinuxContext LinuxContext;
#  endif

  /* code to find a pci device */
  L22LINKAGE extern int linux_find_pcidev(LinuxContext * pContext, unsigned short vendorID, unsigned short deviceID);
  
  L22LINKAGE extern int linux_allocate_dma_mem(LinuxContext *pContext, void **pVirt, unsigned long *pPhys, unsigned long size);
  L22LINKAGE extern int linux_free_dma_mem(LinuxContext *pContext, void *virt, unsigned long phys, unsigned long size);

  L22LINKAGE extern void *linux_map_io(LinuxContext *, unsigned bar, unsigned long *phys, unsigned long *len);
  L22LINKAGE extern int linux_unmap_io(LinuxContext *, void *virtAddr);

  
  /* for C++ to override operator new() and operator delete().. */
  L22LINKAGE extern void linux_free(void *memory);
  L22LINKAGE extern void *linux_allocate(unsigned size);

  L22LINKAGE extern unsigned long linux_virt_to_phys(volatile void *address);

  L22LINKAGE extern int linux_printk(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
  L22LINKAGE extern int linux_warning(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));   /**< Prepends KERN_WARNING */
  L22LINKAGE extern int linux_debug(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));   /**< Prepends KERN_DEBUG */
  L22LINKAGE extern int linux_message(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));    /**< Prepends KERN_INFO */
  L22LINKAGE extern int linux_error(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));    /**< Prepends KERN_ERR */

  /* Wait (synchronously) for microseconds to pass by
     -- it's a tight busy loop that keeps checking the pentium's tsc! */
  L22LINKAGE extern void linux_microwait(unsigned long microseconds);

  /* returns the Pentium TSC (time since powerup in ns) */
  L22LINKAGE extern unsigned long long linux_get_cycles(void);

  /** Call this after writing to DMA buffers? */
  L22LINKAGE extern void linux_memory_barrier(void);

  /* Returns true on success, false on failure */
  L22LINKAGE extern void rtlinux_l22_lock(struct LinuxContext *);
  /* Returns true on success, false on failure */
  L22LINKAGE extern void rtlinux_l22_unlock(struct LinuxContext *);
  L22LINKAGE extern void rtlinux_yield(void);
  
  struct RTLinux_Lock_t;  
  L22LINKAGE extern struct RTLinux_Lock_t *rtlinux_lock_create(void);
  L22LINKAGE extern void rtlinux_lock_destroy(struct RTLinux_Lock_t *);
  L22LINKAGE extern void rtlinux_lock(struct RTLinux_Lock_t *);
  L22LINKAGE extern void rtlinux_unlock(struct RTLinux_Lock_t *);

  /** The size, in bytes, of each of the DMA buffers.  Keep this reasonably
      small to reduce latency, or larger to minimize DMA starvation risks.
      This comes from the module parameter dma_size */
  extern unsigned DMABufferSize;
  extern unsigned SampleRate; /**< Comes from a module param -- the sample
                                 rate the all L22s are running at. */

  /* If set, produce verbose debug output, otherwise don't. */
  extern int debug;
       
#ifdef __cplusplus
}
#endif

#endif
