#include "memory.h"

unsigned char memory_bitmap[BITMAP_SIZE];

void init_memory_manager() {
    for (int i = 0; i < BITMAP_SIZE; i++) {
        memory_bitmap[i] = 0;
    }
    
    // В будущем здесь можно пометить первые страницы как занятые
}

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

void* kalloc() {
    for (int i = 0; i < BITMAP_SIZE; i++) {
        if (memory_bitmap[i] != 0xFF) {
            for (int j = 0; j < 8; j++) {
                if (!(memory_bitmap[i] & (1 << j))) {
                    int page_idx = i * 8 + j;
                    bitmap_set(page_idx);

                    return (void*)(MEM_START + (page_idx * PAGE_SIZE));
                }
            }
        }
    }
    return 0; 
}

void kfree(void* ptr) {
    unsigned int addr = (unsigned int)ptr;

    if (addr < MEM_START || addr >= (MEM_START + MEM_SIZE)) return;

    int page_idx = (addr - MEM_START) / PAGE_SIZE;
    bitmap_unset(page_idx);
}