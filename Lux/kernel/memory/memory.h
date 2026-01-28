#ifndef MEMORY_H
#define MEMORY_H

#define PAGE_SIZE 4096                
#define MEM_START 0x100000             
#define MEM_SIZE  (128 * 1024 * 1024)   
#define TOTAL_PAGES (MEM_SIZE / PAGE_SIZE)
#define BITMAP_SIZE (TOTAL_PAGES / 8)

#define HEAP_START (MEM_START + (BITMAP_SIZE * PAGE_SIZE))
#define HEAP_SIZE  (16 * 1024 * 1024)  
#define HEAP_MAGIC 0xDEADBEEF

// Флаги страниц
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

// Макросы индексов
#define PD_INDEX(vaddr) ((vaddr >> 22) & 0x3FF)
#define PT_INDEX(vaddr) ((vaddr >> 12) & 0x3FF)
#define PAGE_ALIGN(addr) (addr & 0xFFFFF000)

// Структура кучи
struct heap_block {
    unsigned int magic;     
    unsigned int size;
    unsigned int is_free;
    struct heap_block* next;
};

// Прототипы: Физическая память
void init_memory_manager();
void* kalloc();
void bitmap_set(int page_idx);
void bitmap_unset(int page_idx);

// Прототипы: Виртуальная память (Paging)
void init_paging();
void vmm_map(unsigned int virtual_addr, unsigned int physical_addr, int flags);
extern void load_page_directory(unsigned int* directory);
extern void enable_paging();

// Прототипы: Куча
void init_heap();
void* kmalloc(unsigned int size);
void kfree_heap(void* ptr);

#endif