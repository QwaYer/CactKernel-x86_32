#include "validate.h"

int validate_user_ptr(const void* ptr, uint32_t size) {
    uint32_t addr = (uint32_t)ptr;
    if (!ptr)                     return 0;
    if (addr < USER_SPACE_START)  return 0;
    if (addr >= KERNEL_BASE)      return 0;
    if (size == 0)                return 1;
    uint32_t end = addr + size;
    if (end < addr)               return 0;
    if (end > KERNEL_BASE)        return 0;
    return 1;
}

int validate_user_str(const char* str) {
    uint32_t addr = (uint32_t)str;
    if (!str)                     return 0;
    if (addr < USER_SPACE_START)  return 0;
    if (addr >= KERNEL_BASE)      return 0;
    for (uint32_t i = 0; i < USER_STR_MAX; i++) {
        if (addr + i >= KERNEL_BASE) return 0;
        if (str[i] == '\0')          return 1;
    }
    return 0;
}
