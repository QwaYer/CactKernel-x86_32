#ifndef CACT_ZSTD_COMPAT_UNALIGNED_H
#define CACT_ZSTD_COMPAT_UNALIGNED_H

#include "types.h"

/* Native-width access, used by mem.h's MEM_readNN/MEM_writeNN. */
#define get_unaligned(ptr)                                                   \
	({                                                                   \
		uint64_t __v = 0;                                            \
		__builtin_memcpy(&__v, (const void *)(ptr), sizeof(*(ptr)));  \
		__v;                                                         \
	})
#define put_unaligned(val, ptr)                                              \
	do {                                                                 \
		uint64_t __v = (uint64_t)(val);                              \
		__builtin_memcpy((void *)(ptr), &__v, sizeof(*(ptr)));       \
	} while (0)

static inline uint16_t get_unaligned_le16(const void *p)
{
	uint16_t v;
	__builtin_memcpy(&v, p, sizeof(v));
	return v;
}

static inline uint32_t get_unaligned_le32(const void *p)
{
	uint32_t v;
	__builtin_memcpy(&v, p, sizeof(v));
	return v;
}

static inline uint64_t get_unaligned_le64(const void *p)
{
	uint64_t v;
	__builtin_memcpy(&v, p, sizeof(v));
	return v;
}

static inline void put_unaligned_le16(uint16_t v, void *p)
{
	__builtin_memcpy(p, &v, sizeof(v));
}

static inline void put_unaligned_le24(uint32_t v, void *p)
{
	uint8_t b[3] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16) };
	__builtin_memcpy(p, b, sizeof(b));
}

static inline void put_unaligned_le32(uint32_t v, void *p)
{
	__builtin_memcpy(p, &v, sizeof(v));
}

static inline void put_unaligned_le64(uint64_t v, void *p)
{
	__builtin_memcpy(p, &v, sizeof(v));
}

static inline uint32_t get_unaligned_be32(const void *p)
{
	return __builtin_bswap32(get_unaligned_le32(p));
}

static inline uint64_t get_unaligned_be64(const void *p)
{
	return __builtin_bswap64(get_unaligned_le64(p));
}

static inline void put_unaligned_be32(uint32_t v, void *p)
{
	put_unaligned_le32(__builtin_bswap32(v), p);
}

static inline void put_unaligned_be64(uint64_t v, void *p)
{
	put_unaligned_le64(__builtin_bswap64(v), p);
}

#endif /* CACT_ZSTD_COMPAT_UNALIGNED_H */
