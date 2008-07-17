#ifndef mbuffemul_H
#define mbuffemul_H


#if defined(__cplusplus) && defined(MBUF_EMUL_USE_NAMESPACE)
namespace MbufEmul {
#elif defined(__cplusplus)
extern "C" {
#endif

  extern void *mbuff_alloc(const char *name, unsigned size);
  extern void mbuff_free(const char *, void *mbuf);
  extern void *mbuff_attach(const char *name, unsigned size);
  extern void mbuff_detach(const char *, void *mbuf);
  
#if defined(__cplusplus) && defined(MBUF_EMUL_USE_NAMESPACE)
};
#elif defined(__cplusplus)
}
#endif


#endif
