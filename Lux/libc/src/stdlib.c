#include "stdlib.h"
#include "syscall.h"

void exit(int status) {
    syscall(SYS_EXIT, status, 0, 0);
    while(1);
}

// Simple bump allocator for now
static char heap[65536];
static size_t heap_ptr = 0;

void* malloc(size_t size) {
    if (heap_ptr + size > sizeof(heap)) {
        return NULL;
    }
    void* ptr = &heap[heap_ptr];
    heap_ptr += size;
    return ptr;
}

void free(void* ptr) {
    // Not implemented
}
