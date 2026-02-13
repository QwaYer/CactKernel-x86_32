#include "memory.h"
#include "libc.h"
#include "kernel.h"

unsigned char memory_bitmap[BITMAP_SIZE];
struct heap_block* heap_start = (struct heap_block*)HEAP_START;

unsigned int page_directory[1024] __attribute__((aligned(4096)));
unsigned int page_tables[32][1024] __attribute__((aligned(4096)));

static int first_available_page = 0;

void bitmap_set(int page_idx) {
    memory_bitmap[page_idx / 8] |= (1 << (page_idx % 8));
}

void init_memory_manager() {
    for (int i = 0; i < BITMAP_SIZE; i++) memory_bitmap[i] = 0;
    
    unsigned int reserved_end = HEAP_START + HEAP_SIZE;
    int reserved_pages = (reserved_end - MEM_START + PAGE_SIZE - 1) / PAGE_SIZE;
    
    for (int i = 0; i < reserved_pages; i++) {
        bitmap_set(i);
    }
    first_available_page = reserved_pages;
}

void init_heap() {
    heap_start = (struct heap_block*)HEAP_START;
    
    heap_start->magic = HEAP_MAGIC;
    heap_start->size = HEAP_SIZE - sizeof(struct heap_block);
    heap_start->is_free = 1;
    heap_start->next = 0;
}

void init_paging() {
    for(int i = 0; i < 1024; i++) {
        page_directory[i] = 0x00000002; 
    }

    for(int j = 0; j < 32; j++) {
        for(int i = 0; i < 1024; i++) {
            page_tables[j][i] = ((j * 1024 + i) * 4096) | 3;
        }
        page_directory[j] = ((unsigned int)page_tables[j]) | 3;
    }

    load_page_directory(page_directory);
    enable_paging();
}

void* kalloc() {
    for (int i = first_available_page; i < TOTAL_PAGES; i++) {
        if (!(memory_bitmap[i / 8] & (1 << (i % 8)))) {
            bitmap_set(i);
            return (void*)(MEM_START + i * PAGE_SIZE);
        }
    }
    return 0;
}

void* kmalloc(unsigned int size) {
    if (size == 0) return 0;

    size = (size + 7) & ~7;

    __asm__ __volatile__("cli");

    struct heap_block* current = heap_start;
    while (current != 0) {
        if (current->magic != HEAP_MAGIC) {
            kprint("[FATAL] Heap corruption detected!\n");
            __asm__ __volatile__("sti");
            return 0;
        }

        if (current->is_free && current->size >= size) {
            if (current->size > (size + sizeof(struct heap_block) + 16)) {
                struct heap_block* next_block = (struct heap_block*)((unsigned char*)current + sizeof(struct heap_block) + size);
                
                next_block->magic = HEAP_MAGIC;
                next_block->size = current->size - size - sizeof(struct heap_block);
                next_block->is_free = 1;
                next_block->next = current->next;
                
                current->size = size;
                current->next = next_block;
            }
            
            current->is_free = 0;
            __asm__ __volatile__("sti");
            return (void*)((unsigned char*)current + sizeof(struct heap_block));
        }
        current = current->next;
    }

    __asm__ __volatile__("sti");
    kprint("[ERR] kmalloc: Out of heap memory\n");
    return 0;
}

void kfree_heap(void* ptr) {
    if (!ptr) return;

    __asm__ __volatile__("cli");

    struct heap_block* block = (struct heap_block*)((unsigned char*)ptr - sizeof(struct heap_block));
    if (block->magic == HEAP_MAGIC) {
        block->is_free = 1;

        struct heap_block* curr = heap_start;
        while (curr && curr->next) {
            if (curr->is_free && curr->next->is_free) {
                curr->size += curr->next->size + sizeof(struct heap_block);
                curr->next = curr->next->next;
            } else {
                curr = curr->next;
            }
        }
    }
    __asm__ __volatile__("sti");
}