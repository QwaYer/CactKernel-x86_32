#ifndef CACT_ZSTD_COMPAT_TYPES_H
#define CACT_ZSTD_COMPAT_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;

/* Cact is x86-only (little-endian); mem.h/xxhash.c branch on this macro. */
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1
#endif

#endif /* CACT_ZSTD_COMPAT_TYPES_H */
