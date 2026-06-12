#ifndef SC_VALIDATE_H
#define SC_VALIDATE_H

#include "kernel.h"

#ifndef KERNEL_BASE
#define KERNEL_BASE      0xC0000000U
#endif
#define USER_SPACE_START 0x08000000U  /* low 128 MiB is identity-mapped kernel space */
#define USER_STR_MAX     4096

int validate_user_ptr(const void* ptr, uint32_t size);
int validate_user_str(const char* str);

#endif 
