#ifndef LIBC_H
#define LIBC_H

#include <stddef.h>

int strcmp(char* s1, char* s2);
int compare_string(char* s1, char* s2);
int strlen(const char* s);

void itoa(int n, char str[]);
int atoi(char* str);

void* memory_set(void* dest, int val, int len);
void* memory_copy(void* dest, const void* src, int len);
int memory_compare(const void* s1, const void* s2, int n);

#endif