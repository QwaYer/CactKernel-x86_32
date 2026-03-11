#ifndef LIBC_H
#define LIBC_H

#include <stddef.h>
#include <stdint.h>

void kprint(char* message);

int strcmp(char* s1, char* s2);
int compare_string(char* s1, char* s2);
int strlen(const char* s);
char* strcat(char* dest, const char* src);
char* strcpy(char* dest, const char* src);
char* copy_string(char* dest, const char* src);

void itoa(int n, char str[]);
int atoi(char* str);
void hex_to_ascii(unsigned int n, char str[]);

void* memory_set(void* dest, int val, int len);
void* memory_copy(void* dest, const void* src, int len);
int memory_compare(const void* s1, const void* s2, int n);

void* memset(void* dest, int val, unsigned int len);
void* memcpy(void* dest, const void* src, unsigned int len);

int  strncmp(const char* a, const char* b);
void strncpy(char* dst, const char* src, int n);
int  buf_append(char* buf, int pos, int max, const char* s);
int  buf_append_int(char* buf, int pos, int max, int n);

int  streq  (const char *a, const char *b);
void strlcpy(char *dst, const char *src, int n);

void kprint_hex(uint32_t n);

#endif