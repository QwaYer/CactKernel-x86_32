#ifndef CACT_ZSTD_COMPAT_COMPILER_H
#define CACT_ZSTD_COMPAT_COMPILER_H

#include "types.h"

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

#ifndef __maybe_unused
#define __maybe_unused __attribute__((unused))
#endif

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* zstd's compiler.h spells ZSTD_FALLTHROUGH as `fallthrough`, the Linux
 * compiler_attributes shorthand. */
#ifndef fallthrough
#define fallthrough __attribute__((__fallthrough__))
#endif

#endif /* CACT_ZSTD_COMPAT_COMPILER_H */
