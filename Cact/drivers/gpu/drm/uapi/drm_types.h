#ifndef CACT_DRM_TYPES_H
#define CACT_DRM_TYPES_H

/*
 * Fixed-width integer aliases used by the DRM ioctl ABI headers.
 *
 * The DRM ABI is written in terms of __u8/__u16/__u32/__u64, so these names
 * have to exist somewhere.  This is that somewhere: a small, self-contained
 * header owned by the DRM core, so the kernel never reaches for a hosted
 * <sys/types.h> or a libc-flavoured types header.
 *
 * 32-bit kernel: long is 32 bits, so the __kernel_* aliases agree with the
 * C99 types above.
 */

#include <stdint.h>

typedef int8_t   __s8;
typedef uint8_t  __u8;
typedef int16_t  __s16;
typedef uint16_t __u16;
typedef int32_t  __s32;
typedef uint32_t __u32;
typedef int64_t  __s64;
typedef uint64_t __u64;

typedef uint16_t __le16;
typedef uint16_t __be16;
typedef uint32_t __le32;
typedef uint32_t __be32;
typedef uint64_t __le64;
typedef uint64_t __be64;

typedef __u8   __kernel_uchar;
typedef __u16  __kernel_ushort;
typedef __u32  __kernel_uint;
typedef __u64  __kernel_ulong_long;
typedef __s32  __kernel_long;
typedef __u32  __kernel_ulong;
typedef __u32  __kernel_size_t;

#endif /* CACT_DRM_TYPES_H */
