#ifndef rtfemul_H
#define rtfemul_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTF_NO 64

/** for use in "kernel-like" code */
extern int rtf_create(unsigned minor, unsigned size);
extern int rtf_get(unsigned minor, void *buf, unsigned bytes);
extern int rtf_put(unsigned minor, const void *buf, unsigned bytes);
extern int rtf_destroy(unsigned minor);
extern int rtf_num_avail(unsigned minor);
#define RTF_FREE(x)  1
#define RTF_EMPTY(x) (rtf_num_avail(x) == 0)
/** for use in userspace-like code -- returns an fd -- call unix close() to close the fd */
extern int rtf_open(unsigned minor); /**< call close to close this! */

#ifdef __cplusplus
}  
#endif

#endif
