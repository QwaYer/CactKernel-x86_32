#ifndef MEMORY_H
#define MEMORY_H

#define PAGE_SIZE 4096                
#define MEM_START 0x100000             
#define MEM_SIZE  (128 * 1024 * 1024)   
#define TOTAL_PAGES (MEM_SIZE / PAGE_SIZE)
#define BITMAP_SIZE (TOTAL_PAGES / 8)

// Heap для kmalloc
#define HEAP_START (MEM_START + (BITMAP_SIZE * PAGE_SIZE))
#define HEAP_SIZE  (16 * 1024 * 1024)  // 16MB для heap
#define HEAP_MAGIC 0xDEADBEEF

struct heap_block {
    unsigned int magic;       // Для проверки целостности
    unsigned int size;
    unsigned int is_free;
    struct heap_block* next;
};

// Функции управления
void init_memory_manager();
void* kalloc();
void kfree(void* ptr);

void init_heap();
void* kmalloc(unsigned int size);
void kfree_heap(void* ptr);
unsigned int get_free_memory();

// Утилиты
void memory_copy(void* source, void* dest, int n);
void memory_set(void* dest, unsigned char val, int n);
int memory_compare(void* s1, void* s2, int n);

#endif