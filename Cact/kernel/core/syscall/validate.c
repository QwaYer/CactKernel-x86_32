#include "validate.h"
#include "klib.h"
#include "memory.h"

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

int copy_to_user(void* dst, const void* src, uint32_t size) {
    if (!validate_user_ptr(dst, size)) return -1;
    memcpy(dst, src, size);
    return 0;
}

int copy_from_user(void* dst, const void* src, uint32_t size) {
    if (!validate_user_ptr(src, size)) return -1;
    memcpy(dst, src, size);
    return 0;
}

// Validate a user string and copy it to a kernel heap buffer.
// Returns a kmalloc'd buffer on success, NULL on failure.
// Caller must kfree() the returned buffer.
char* copy_path_from_user(const char* user_str) {
    if (!validate_user_str(user_str)) return 0;
    int len = 0;
    while (user_str[len] && len < USER_STR_MAX - 1) len++;
    char* buf = (char*)kmalloc((uint32_t)len + 1);
    if (!buf) return 0;
    int i;
    for (i = 0; i < len; i++) buf[i] = user_str[i];
    buf[i] = '\0';
    return buf;
}