#ifndef KLIB_H
#define KLIB_H

#include <stddef.h>
#include <stdint.h>

// kernel console output
void kprint(char* message);

// string operations
int strcmp(const char* s1, const char* s2);
int compare_string(const char* s1, const char* s2);
int strlen(const char* s);
char* strcat(char* dest, const char* src);
char* strcpy(char* dest, const char* src);
char* copy_string(char* dest, const char* src);

// numeric conversions
void itoa(int n, char str[]);
int atoi(char* str);
void hex_to_ascii(unsigned int n, char str[]);

// memory operations (signed length)
void* memory_set(void* dest, int val, int len);
void* memory_copy(void* dest, const void* src, int len);
int memory_compare(const void* s1, const void* s2, int n);

// memory operations (unsigned length)
void* memset(void* dest, int val, unsigned int len);
void* memcpy(void* dest, const void* src, unsigned int len);

// bounded string operations
int  strncmp(const char* a, const char* b);
void strncpy(char* dst, const char* src, int n);

// buffer helpers
int  buf_append(char* buf, int pos, int max, const char* s);
int  buf_append_int(char* buf, int pos, int max, int n);

// string utilities
int  streq  (const char *a, const char *b);
void strlcpy(char *dst, const char *src, int n);
void strlcat(char *dst, const char *src, int n);

// hex output
void kprint_hex(uint32_t n);

#endif