#include "memory.h"
#include "libc.h" 

unsigned char memory_bitmap[BITMAP_SIZE];
struct heap_block* heap_start = (struct heap_block*)HEAP_START;


void bitmap_set(int page_idx) {
    int byte_idx = page_idx / 8;
    int bit_idx = page_idx % 8;
    memory_bitmap[byte_idx] |= (1 << bit_idx);
}

void bitmap_unset(int page_idx) {
    int byte_idx = page_idx / 8;
    int bit_idx = page_idx % 8;
    memory_bitmap[byte_idx] &= ~(1 << bit_idx);
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

void* kalloc() {
    for (int i = 0; i < TOTAL_PAGES; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (!(memory_bitmap[byte_idx] & (1 << bit_idx))) {
            bitmap_set(i);
            return (void*)(MEM_START + (i * PAGE_SIZE));
        }
    }
    return 0; 
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