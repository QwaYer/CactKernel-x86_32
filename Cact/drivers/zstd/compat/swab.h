#ifndef CACT_ZSTD_COMPAT_SWAB_H
#define CACT_ZSTD_COMPAT_SWAB_H

#include "types.h"

static inline uint16_t swab16(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t swab32(uint32_t x) { return __builtin_bswap32(x); }
static inline uint64_t swab64(uint64_t x) { return __builtin_bswap64(x); }

#endif /* CACT_ZSTD_COMPAT_SWAB_H */
