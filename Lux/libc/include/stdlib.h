#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

void exit(int status);
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int atoi(const char *str);
void itoa(int n, char str[]);
void hex_to_ascii(unsigned int n, char str[]);

#endif