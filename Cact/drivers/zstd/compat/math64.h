#ifndef CACT_ZSTD_COMPAT_MATH64_H
#define CACT_ZSTD_COMPAT_MATH64_H

#include "types.h"

static inline uint64_t div_u64(uint64_t dividend, uint32_t divisor)
{
	return dividend / divisor;
}

#endif /* CACT_ZSTD_COMPAT_MATH64_H */
