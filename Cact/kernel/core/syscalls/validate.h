#ifndef SC_VALIDATE_H
#define SC_VALIDATE_H

#include "kernel.h"

#define KERNEL_BASE      0xC0000000U
#define USER_SPACE_START 0x08000000U  /* первые 128 МБ — identity-mapped kernel */
#define USER_STR_MAX     4096

int validate_user_ptr(const void* ptr, uint32_t size);
int validate_user_str(const char* str);

#endif /* SC_VALIDATE_H */
