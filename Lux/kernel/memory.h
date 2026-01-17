#ifndef MEMORY_H
#define MEMORY_H

#define PAGE_SIZE 4096                
#define MEM_START 0x100000             
#define MEM_SIZE  (128 * 1024 * 1024)   
#define TOTAL_PAGES (MEM_SIZE / PAGE_SIZE)
#define BITMAP_SIZE (TOTAL_PAGES / 8)

void init_memory_manager();

void* kalloc();

void kfree(void* ptr);

#endif