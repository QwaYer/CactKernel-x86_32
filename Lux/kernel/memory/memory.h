#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096                
#define MEM_START 0x100000             
#define MEM_SIZE  (128 * 1024 * 1024)   
#define TOTAL_PAGES (MEM_SIZE / PAGE_SIZE)
#define BITMAP_SIZE (TOTAL_PAGES / 8)

#define HEAP_START (MEM_START + (BITMAP_SIZE * PAGE_SIZE))
#define HEAP_SIZE  (16 * 1024 * 1024)  
#define HEAP_MAGIC 0xDEADBEEF

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

#define PD_INDEX(vaddr) ((vaddr >> 22) & 0x3FF)
#define PT_INDEX(vaddr) ((vaddr >> 12) & 0x3FF)

struct heap_block {
    uint32_t magic;     
    uint32_t size;
    uint32_t is_free;
    struct heap_block* next;
};

void init_memory_manager();
void* kalloc();
void init_paging();
uint32_t get_free_heap_memory();
uint32_t* vmm_create_address_space(); 
void vmm_map(uint32_t* pd, uint32_t virtual_addr, uint32_t physical_addr, int flags);
extern void load_page_directory(uint32_t* directory); 
extern void enable_paging(); 

void init_heap();
void kfree_page(void* ptr);
void* kmalloc(uint32_t size);
void* kmalloc_aligned(uint32_t size, uint32_t align);
void vmm_free_address_space(uint32_t* pd);
void vmm_copy_address_space(uint32_t* src_pd, uint32_t* dst_pd);
void kfree_heap(void* ptr);

#endif