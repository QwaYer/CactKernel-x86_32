#include "memory.h"
#include "libc.h" 
#include "kernel.h"

unsigned char memory_bitmap[BITMAP_SIZE];
struct heap_block* heap_start = (struct heap_block*)HEAP_START;

unsigned int page_directory[1024] __attribute__((aligned(4096)));
unsigned int page_tables[8][1024] __attribute__((aligned(4096)));


void bitmap_set(int page_idx) {
    memory_bitmap[page_idx / 8] |= (1 << (page_idx % 8));
}

void bitmap_unset(int page_idx) {
    memory_bitmap[page_idx / 8] &= ~(1 << (page_idx % 8));
}

void* kalloc() {
    for (int i = 0; i < TOTAL_PAGES; i++) {
        if (!(memory_bitmap[i / 8] & (1 << (i % 8)))) {
            bitmap_set(i);
            return (void*)(MEM_START + (i * PAGE_SIZE));
        }
    }
    return 0; 
}

void init_memory_manager() {
    memory_set(memory_bitmap, 0, BITMAP_SIZE);

    int heap_start_page = (HEAP_START - MEM_START) / PAGE_SIZE;
    int heap_pages = HEAP_SIZE / PAGE_SIZE;
    for (int i = 0; i < heap_pages; i++) {
        bitmap_set(heap_start_page + i);
    }
    init_heap();
}

unsigned int get_free_heap_memory() {
    unsigned int free_size = 0;
    struct heap_block* current = heap_start;
    while (current) {
        if (current->is_free) {
            free_size += current->size;
        }
        current = current->next;
    }
    return free_size;
}

unsigned int* vmm_create_address_space() {
    unsigned int* new_pd = (unsigned int*)kalloc();
    if (!new_pd) return 0;

    for (int i = 0; i < 1024; i++) {
        new_pd[i] = PAGE_RW; 
    }

    for (int i = 0; i < 8; i++) {
        new_pd[i] = page_directory[i];
    }

    return new_pd;
}

void vmm_map(unsigned int virtual_addr, unsigned int physical_addr, int flags) {
    unsigned int pd_idx = PD_INDEX(virtual_addr);
    unsigned int pt_idx = PT_INDEX(virtual_addr);
    unsigned int* table;

    if (page_directory[pd_idx] & PAGE_PRESENT) {
        table = (unsigned int*)(page_directory[pd_idx] & 0xFFFFF000);
    } else {
        table = (unsigned int*)kalloc();
        memory_set(table, 0, PAGE_SIZE);
        page_directory[pd_idx] = ((unsigned int)table) | flags | PAGE_PRESENT;
    }

    table[pt_idx] = (physical_addr & 0xFFFFF000) | flags | PAGE_PRESENT;

    __asm__ volatile("invlpg (%0)" :: "r" (virtual_addr) : "memory");
}

void init_paging() {
    for (int i = 0; i < 1024; i++) {
        page_directory[i] = PAGE_RW; 
    }

    for (int t = 0; t < 8; t++) {
        for (int i = 0; i < 1024; i++) {
            unsigned int phys = (t * 1024 * 4096) + (i * 4096);
            page_tables[t][i] = phys | PAGE_PRESENT | PAGE_RW;
        }
        page_directory[t] = ((unsigned int)page_tables[t]) | PAGE_PRESENT | PAGE_RW;
    }

    load_page_directory(page_directory);
    enable_paging();
}


void init_heap() {
    heap_start->magic = HEAP_MAGIC;
    heap_start->size = HEAP_SIZE - sizeof(struct heap_block);
    heap_start->is_free = 1;
    heap_start->next = 0;
}

void* kmalloc(unsigned int size) {
    struct heap_block* current = heap_start;
    while (current) {
        if (current->is_free && current->size >= size) {
            if (current->size > size + sizeof(struct heap_block) + 16) {
                struct heap_block* next_block = (struct heap_block*)((unsigned char*)current + sizeof(struct heap_block) + size);
                next_block->magic = HEAP_MAGIC;
                next_block->size = current->size - size - sizeof(struct heap_block);
                next_block->is_free = 1;
                next_block->next = current->next;

                current->size = size;
                current->next = next_block;
            }
            current->is_free = 0;
            return (void*)((unsigned char*)current + sizeof(struct heap_block));
        }
        current = current->next;
    }
    return 0;
}

void kfree_heap(void* ptr) {
    if (!ptr) return;
    struct heap_block* block = (struct heap_block*)((unsigned char*)ptr - sizeof(struct heap_block));
    if (block->magic != HEAP_MAGIC) return; 

    block->is_free = 1;

    struct heap_block* curr = heap_start;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            curr->size += sizeof(struct heap_block) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}