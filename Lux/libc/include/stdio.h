#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>

int printf(const char* format, ...);
int puts(const char* str);
int putchar(int c);
int rename(const char *oldpath, const char *newpath);

#endif
