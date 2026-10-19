#ifndef CACT_ZSTD_COMPAT_STRING_H
#define CACT_ZSTD_COMPAT_STRING_H

#include "types.h"

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif /* CACT_ZSTD_COMPAT_STRING_H */
