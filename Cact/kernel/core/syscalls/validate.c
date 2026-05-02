#include "validate.h"

// Validate that a pointer range falls entirely within userspace.
// Checks for NULL, address below USER_SPACE_START, address >= KERNEL_BASE,
// integer overflow on end calculation, and end beyond KERNEL_BASE.
int validate_user_ptr(const void* ptr, uint32_t size) {
    uint32_t addr = (uint32_t)ptr;
    if (!ptr)                     return 0;   // NULL pointer
    if (addr < USER_SPACE_START)  return 0;   // kernel identity-mapped region
    if (addr >= KERNEL_BASE)      return 0;   // kernel space
    if (size == 0)                return 1;   // zero-size access is always valid
    uint32_t end = addr + size;
    if (end < addr)               return 0;   // overflow wrap
    if (end > KERNEL_BASE)        return 0;   // crosses into kernel space
    return 1;
}

// Validate a null-terminated userspace string.
// Ensures the string starts in userspace and a null terminator is found
// within USER_STR_MAX bytes without crossing into kernel space.
int validate_user_str(const char* str) {
    uint32_t addr = (uint32_t)str;
    if (!str)                     return 0;   // NULL pointer
    if (addr < USER_SPACE_START)  return 0;   // kernel identity-mapped region
    if (addr >= KERNEL_BASE)      return 0;   // kernel space
    for (uint32_t i = 0; i < USER_STR_MAX; i++) {
        if (addr + i >= KERNEL_BASE) return 0; // crossed into kernel space
        if (str[i] == '\0')          return 1; // found null terminator
    }
    return 0;   // string too long or missing null terminator
}