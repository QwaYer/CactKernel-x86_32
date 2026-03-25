#include "stdlib.h"
#include "syscall.h"
#include "string.h"

void exit(int status) {
    syscall(SYS_EXIT, status, 0, 0);
    while(1);
}


#define BLOCK_MAGIC 0xA110CA7E
#define ALIGN8(x)  (((x) + 7) & ~7)
#define BLOCK_SIZE  sizeof(struct block_header)

struct block_header {
    unsigned int magic;
    unsigned int size;
    unsigned int is_free;
    struct block_header *next;
};

static struct block_header *free_list = 0;
static int heap_initialized = 0;

static void heap_init(void) {
    if (heap_initialized) return;
    heap_initialized = 1;
    free_list = 0;
}

static struct block_header *find_free_block(unsigned int size) {
    struct block_header *cur = free_list;
    while (cur) {
        if (cur->is_free && cur->size >= size)
            return cur;
        cur = cur->next;
    }
    return 0;
}

static struct block_header *request_space(unsigned int size) {
    unsigned int total = BLOCK_SIZE + size;
    unsigned int request = total;
    if (request < 4096) request = 4096;

    void *ptr = (void*)syscall(SYS_BRK, 0, 0, 0);
    if ((int)ptr < 0) return 0;

    int new_brk = syscall(SYS_BRK, (int)ptr + (int)request, 0, 0);
    if (new_brk < 0) return 0;

    struct block_header *block = (struct block_header *)ptr;
    block->magic   = BLOCK_MAGIC;
    block->size    = request - BLOCK_SIZE;
    block->is_free = 0;
    block->next    = 0;

    if (block->size > size + BLOCK_SIZE + 16) {
        struct block_header *remainder = (struct block_header *)
            ((char *)block + BLOCK_SIZE + size);
        remainder->magic   = BLOCK_MAGIC;
        remainder->size    = block->size - size - BLOCK_SIZE;
        remainder->is_free = 1;
        remainder->next    = 0;
        block->size = size;
        block->next = remainder;

        if (!free_list) {
            free_list = block;
        } else {
            struct block_header *last = free_list;
            while (last->next) last = last->next;
            last->next = block;
        }
        return block;
    }

    if (!free_list) {
        free_list = block;
    } else {
        struct block_header *last = free_list;
        while (last->next) last = last->next;
        last->next = block;
    }
    return block;
}

void *malloc(size_t size) {
    if (size == 0) return 0;
    heap_init();

    unsigned int aligned = ALIGN8((unsigned int)size);

    struct block_header *block = find_free_block(aligned);
    if (block) {
        if (block->size > aligned + BLOCK_SIZE + 16) {
            struct block_header *remainder = (struct block_header *)
                ((char *)block + BLOCK_SIZE + aligned);
            remainder->magic   = BLOCK_MAGIC;
            remainder->size    = block->size - aligned - BLOCK_SIZE;
            remainder->is_free = 1;
            remainder->next    = block->next;
            block->size = aligned;
            block->next = remainder;
        }
        block->is_free = 0;
        return (void *)((char *)block + BLOCK_SIZE);
    }

    block = request_space(aligned);
    if (!block) return 0;
    block->is_free = 0;
    return (void *)((char *)block + BLOCK_SIZE);
}

void free(void *ptr) {
    if (!ptr) return;
    struct block_header *block = (struct block_header *)
        ((char *)ptr - BLOCK_SIZE);
    if (block->magic != BLOCK_MAGIC) return;
    block->is_free = 1;

    struct block_header *cur = free_list;
    while (cur) {
        if (cur->is_free && cur->next && cur->next->is_free) {
            cur->size += BLOCK_SIZE + cur->next->size;
            cur->next = cur->next->next;
            continue; 
        }
        cur = cur->next;
    }
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return 0; }

    struct block_header *block = (struct block_header *)
        ((char *)ptr - BLOCK_SIZE);
    if (block->magic != BLOCK_MAGIC) return 0;
    if (block->size >= (unsigned int)size) return ptr;

    void *new_ptr = malloc(size);
    if (!new_ptr) return 0;
    memcpy(new_ptr, ptr, block->size);
    free(ptr);
    return new_ptr;
}