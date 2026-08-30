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
    int i, sign = 0;
    unsigned int un;
    if (n < 0) { sign = 1; un = -(unsigned int)n; }
    else       { un = (unsigned int)n; }
    i = 0;
    do {
        str[i++] = (un % 10) + '0';
    } while ((un /= 10) > 0);
    if (sign) str[i++] = '-';
    str[i] = '\0';
    // reverse
    for (int j = 0, k = i - 1; j < k; j++, k--) {
        char temp = str[j];
        str[j] = str[k];
        str[k] = temp;
    }
}// Convert ASCII string to integer, handle optional minus
int atoi(char* str) {
    int res = 0, sign = 1, i = 0;
    if (str[0] == '-') { sign = -1; i++; }
    for (; str[i] != '\0'; ++i) {
        if (str[i] < '0' || str[i] > '9') break;
        res = res * 10 + str[i] - '0';
    }
    return sign * res;
}

// Convert 32-bit hex to "0xXXXXXXXX" format (helper for snprintf %x)
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

// Linux-style vsnprintf.  Supports %d %u %x %X %o %s %c %p %% plus
// width / zero-padding and l/ll/zu/zx length modifiers.
int vsnprintf(char* buf, unsigned int size, const char* fmt, va_list args) {
    char* dst = buf;
    unsigned int remaining = size;
    va_list ap;
    va_copy(ap, args);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (remaining > 1) { *dst++ = *fmt; remaining--; }
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            if (remaining > 1) { *dst++ = '%'; remaining--; }
            continue;
        }

        int left = 0;
        int plus = 0;
        int space = 0;
        int zero = 0;
        int alt = 0;
        for (;;) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '#') alt = 1;
            else break;
            fmt++;
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }

        int lng = 0;
        for (;;) {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'z' || *fmt == 't') lng = 1;
            else break;
            fmt++;
        }

        char tmp[32];
        int tmp_len = 0;
        char fill = zero ? '0' : ' ';
        char sign = 0;

        switch (*fmt) {
        case 'd':
        case 'i': {
            long long v = (lng >= 2) ? va_arg(ap, long long) :
                          (lng >= 1) ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0) { sign = '-'; v = -v; }
            else if (plus) sign = '+';
            else if (space) sign = ' ';
            unsigned long long uv = (unsigned long long)v;
            do { tmp[tmp_len++] = (uv % 10) + '0'; uv /= 10; } while (uv);
            break;
        }
        case 'u': {
            unsigned long long v = (lng >= 2) ? va_arg(ap, unsigned long long) :
                                   (lng >= 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            do { tmp[tmp_len++] = (v % 10) + '0'; v /= 10; } while (v);
            break;
        }
        case 'o': {
            unsigned long long v = (lng >= 2) ? va_arg(ap, unsigned long long) :
                                   (lng >= 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            do { tmp[tmp_len++] = (v & 7) + '0'; v >>= 3; } while (v);
            if (alt && tmp[tmp_len - 1] != '0') tmp[tmp_len++] = '0';
            break;
        }
        case 'x':
        case 'X': {
            unsigned long long v = (lng >= 2) ? va_arg(ap, unsigned long long) :
                                   (lng >= 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            do {
                int d = (v & 0xF);
                tmp[tmp_len++] = (d < 10) ? (d + '0') : (d - 10 + ((*fmt == 'x') ? 'a' : 'A'));
                v >>= 4;
            } while (v);
            if (alt && width <= 0 && tmp_len >= 1) { /* prefix handled by caller */ }
            break;
        }
        case 'p': {
            unsigned long v = (unsigned long)va_arg(ap, void*);
            tmp[tmp_len++] = 'x';
            tmp[tmp_len++] = '0';
            do {
                int d = (v & 0xF);
                tmp[tmp_len++] = (d < 10) ? (d + '0') : (d - 10 + 'a');
                v >>= 4;
            } while (v);
            fill = ' ';
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int len = 0;
            while (s[len] && (prec < 0 || len < prec)) len++;
            if (left) {
                for (int i = 0; i < len && remaining > 1; i++) { *dst++ = s[i]; remaining--; }
                for (int i = len; i < width && remaining > 1; i++) { *dst++ = ' '; remaining--; }
            } else {
                for (int i = len; i < width && remaining > 1; i++) { *dst++ = ' '; remaining--; }
                for (int i = 0; i < len && remaining > 1; i++) { *dst++ = s[i]; remaining--; }
            }
            continue;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (remaining > 1) { *dst++ = c; remaining--; }
            continue;
        }
        default:
            continue;
        }

        // reverse tmp and emit with width/sign/zero handling
        int digits = tmp_len;
        int pad = width - digits;
        if (sign && fill == '0') pad--;

        if (!left) {
            if (sign && fill == '0' && remaining > 1) { *dst++ = sign; remaining--; }
            for (int i = 0; i < pad && remaining > 1; i++) { *dst++ = fill; remaining--; }
            if (sign && fill == ' ' && remaining > 1) { *dst++ = sign; remaining--; }
        } else {
            if (sign && remaining > 1) { *dst++ = sign; remaining--; }
        }
        for (int i = digits - 1; i >= 0 && remaining > 1; i--) { *dst++ = tmp[i]; remaining--; }
        if (left) {
            for (int i = 0; i < pad && remaining > 1; i++) { *dst++ = ' '; remaining--; }
        }
    }

    if (remaining > 0) *dst = '\0';
    else if (size > 0) buf[size - 1] = '\0';
    va_end(ap);
    return (int)(dst - buf);
}

int snprintf(char* buf, unsigned int size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
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

// String append with length limit (safer version of strcat)
void strlcat(char *dst, const char *src, int n) {
    int i = 0;
    while (dst[i] && i < n) i++;
    int pos = i;
    while (src[i - pos] && i < n - 1) { dst[i] = src[i - pos]; i++; }
    dst[i] = '\0';
}

// Print 32-bit hex to kernel console
void printk_hex(uint32_t n) {
    char buf[12];
    hex_to_ascii(n, buf);
    printk(buf);
}