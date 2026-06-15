#include "klib.h"

// Compare strings until mismatch or '\0'
int compare_string(const char* s1, const char* s2) {
    int i;
    for (i = 0; s1[i] == s2[i]; i++) {
        if (s1[i] == '\0') return 0;   // both strings ended
    }
    return s1[i] - s2[i];               // ASCII difference
}

// POSIX-compatible wrapper
int strcmp(const char* s1, const char* s2) {
    return compare_string(s1, s2);
}

// Return string length, excluding '\0'
int strlen(const char* s) {
    int len = 0;
    while (s[len] != '\0') len++;
    return len;
}

// Append src to dest, return dest
char* strcat(char* dest, const char* src) {
    if (!dest || !src) return dest;
    char* ptr = dest + strlen(dest);
    uint32_t remaining = (uint32_t)-1;
    while (*src != '\0' && remaining--) *ptr++ = *src++;
    *ptr = '\0';
    return dest;
}

// Copy src to dest, return dest
char* strcpy(char* dest, const char* src) {
    if (!dest || !src) return dest;
    char* ptr = dest;
    uint32_t remaining = (uint32_t)-1;
    while (*src != '\0' && remaining--) *ptr++ = *src++;
    *ptr = '\0';
    return dest;
}

// Alias for strcpy
char* copy_string(char* dest, const char* src) {
    return strcpy(dest, src);
}

// Convert integer to ASCII string (base 10), reversed then reversed back
void itoa(int n, char str[]) {
    int i, sign;
    if ((sign = n) < 0) n = -n;
    i = 0;
    do {
        str[i++] = n % 10 + '0';
    } while ((n /= 10) > 0);
    if (sign < 0) str[i++] = '-';
    str[i] = '\0';
    // reverse
    for (int j = 0, k = i - 1; j < k; j++, k--) {
        char temp = str[j];
        str[j] = str[k];
        str[k] = temp;
    }
}

// Convert ASCII string to integer, handle optional minus
int atoi(char* str) {
    int res = 0, sign = 1, i = 0;
    if (str[0] == '-') { sign = -1; i++; }
    for (; str[i] != '\0'; ++i) {
        if (str[i] < '0' || str[i] > '9') break;
        res = res * 10 + str[i] - '0';
    }
    return sign * res;
}

// Convert 32-bit hex to "0xXXXXXXXX" format
void hex_to_ascii(unsigned int n, char str[]) {
    str[0] = '0';
    str[1] = 'x';
    int i;
    for (i = 7; i >= 0; i--) {
        unsigned int nibble = (n >> (i * 4)) & 0x0F;
        if (nibble < 10)
            str[9 - i] = nibble + '0';
        else
            str[9 - i] = nibble - 10 + 'A';
    }
    str[10] = '\0';
}

// Fill memory with byte value, return dest
void* memory_set(void* dest, int val, int len) {
    unsigned char* ptr = (unsigned char*)dest;
    while (len-- > 0) *ptr++ = (unsigned char)val;
    return dest;
}

// Copy memory block, return dest
void* memory_copy(void* dest, const void* src, int len) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (len-- > 0) *d++ = *s++;
    return dest;
}

// Compare two memory blocks, return difference at first mismatch
int memory_compare(const void* s1, const void* s2, int n) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    for (int i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return p1[i] - p2[i];
    }
    return 0;
}

// memset wrapper with unsigned length
void* memset(void* dest, int val, unsigned int len) {
    unsigned char* ptr = (unsigned char*)dest;
    while (len-- > 0) *ptr++ = (unsigned char)val;
    return dest;
}

// memcpy wrapper with unsigned length
void* memcpy(void* dest, const void* src, unsigned int len) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (len-- > 0) *d++ = *s++;
    return dest;
}

// Compare up to first mismatch or until either string ends
int strncmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a - *b;
}

// Copy up to n-1 chars, null-terminate
void strncpy(char* dst, const char* src, int n) {
    int i = 0;
    while (src[i] && i < n - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// Append string to buffer with bounds check, return new position
int buf_append(char* buf, int pos, int max, const char* s) {
    while (*s && pos < max - 1) buf[pos++] = *s++;
    buf[pos] = '\0';
    return pos;
}

// Append integer to buffer via itoa, return new position
int buf_append_int(char* buf, int pos, int max, int n) {
    char tmp[32];
    itoa(n, tmp);
    return buf_append(buf, pos, max, tmp);
}

// Check if strings are equal (including length)
int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// String copy with length limit (safer version)
void strlcpy(char *dst, const char *src, int n) {
    int i = 0;
    while (src[i] && i < n - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// Print 32-bit hex to kernel console
void kprint_hex(uint32_t n) {
    char buf[12];
    hex_to_ascii(n, buf);
    kprint(buf);
}