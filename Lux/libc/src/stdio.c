#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include <stdarg.h>

int putchar(int c) {
    char buf[2] = {(char)c, 0};
    syscall(SYS_PRINT, (int)buf, 0, 0);
    return c;
}

int puts(const char* str) {
    syscall(SYS_PRINT, (int)str, 0, 0);
    putchar('\n');
    return 0;
}

int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    int count = 0;
    for (int i = 0; format[i] != '\0'; i++) {
        if (format[i] == '%') {
            i++;
            if (format[i] == 'd') {
                int val = va_arg(args, int);
                char buf[32];
                itoa(val, buf);
                syscall(SYS_PRINT, (int)buf, 0, 0);
                count += strlen(buf);
            } else if (format[i] == 's') {
                char* val = va_arg(args, char*);
                syscall(SYS_PRINT, (int)val, 0, 0);
                count += strlen(val);
            } else if (format[i] == 'x') {
                unsigned int val = va_arg(args, unsigned int);
                char buf[32];
                hex_to_ascii(val, buf);
                syscall(SYS_PRINT, (int)buf, 0, 0);
                count += strlen(buf);
            } else if (format[i] == 'c') {
                int val = va_arg(args, int);
                putchar(val);
                count++;
            } else if (format[i] == '%') {
                putchar('%');
                count++;
            }
        } else {
            putchar(format[i]);
            count++;
        }
    }

    va_end(args);
    return count;
}

int rename(const char *oldpath, const char *newpath) {
    return syscall(SYS_RENAME, (int)oldpath, (int)newpath, 0);
}
