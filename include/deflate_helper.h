#ifndef DEFLATE_HELPER_H
#define DEFLATE_HELPER_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NO_DEFLATE
/** 1. Copies the buflen bytes of the data in buf to a temporary buffer.
    2. Deflates data from buf back into buf.  buf should have at least
       space for bufsize_max bytes (which should always be >= buflen,
       of course).  If there is not enough space for the uncompressed
       data (that is, it would require more than bufsize_max bytes) -1
       is returned -- so make sure there is enough space in buf for
       the data.  A good rule of thumb is to make the buffer at least
       buflen + buflen/1000 + 12 bytes.
    3. Returns the size of the compressed data, in bytes, or -1 on error.
    
    NB: this function assumes data is TEXT data.
    NB2: buflen is the length of pre-compressed valid data, in bytes, 
    bufsize_max is the amount of space in buffer buf which should always be 
    >= buflen!
*/
extern int deflateInplace(char *buf, unsigned buflen, unsigned bufsize_max);
#endif
/**
   1. Copies buflen_comp bytes of data from the buffer buf to a temp. buffer.
   2. Inflates the data from the temp buffer back to buf.  (Inflated
      data should fit in bufsize_max, and buf itself should have space for
      bufsize_max bytes!!).
   3. Returns the actual size of the uncompressed data, or -1 on error.
*/
extern int inflateInplace(char *buf, unsigned buflen_comp, unsigned bufsize_max);

#ifndef NO_DEFLATE
/** deflates buf, returning a buffer of size comp_size_out.  The buffer
    is suitable to be deleted using free (or vfree in kernel mode). */
extern char *deflateCpy(const char *buf, unsigned buflen, unsigned * comp_size_out);
#endif

/** deflates buf, returning a buffer of size uncomp_buf_max.  The buffer
    is suitable to be deleted using free (or vfree in kernel mode). */
extern char *inflateCpy(const char *buf, unsigned buflen, unsigned uncomp_buf_max, unsigned * uncomp_size_out);

  /** Frees a buffer returned from inflateCpy or deflateCpy.  The DH stands for "deflate helper". */
extern void freeDHBuf(void *buf);

#ifdef __cplusplus
}
#endif

#endif
