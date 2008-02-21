#ifndef IntTypes_H
#define IntTypes_H

#ifdef __KERNEL__
#  include <linux/types.h>
#else
#  include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

  typedef int32_t int32;
  typedef uint32_t uint32;
  typedef int64_t int64;
  typedef uint64_t uint64;
  typedef int8_t int8;
  typedef uint8_t uint8;
  typedef int16_t int16;
  typedef uint16_t uint16;

#ifdef __cplusplus
}
#endif

#endif
