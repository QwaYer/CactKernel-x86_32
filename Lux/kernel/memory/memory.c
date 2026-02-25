#include "memory.h"
#include "libc.h"
#include "kernel.h"
#include "sync.h"

uint8_t memory_bitmap[BITMAP_SIZE];
struct heap_block* heap_start = (struct heap_block*)HEAP_START;

uint32_t page_directory[1024] __attribute__((aligned(4096)));
uint32_t page_tables[32][1024] __attribute__((aligned(4096)));

static int first_available_page = 0;
static irq_spinlock_t heap_lock;
static irq_spinlock_t page_lock;

void bitmap_set(int page_idx) {
    memory_bitmap[page_idx / 8] |= (1 << (page_idx % 8));
}

void init_memory_manager() {
    irq_spinlock_init(&page_lock);
    for (int i = 0; i < BITMAP_SIZE; i++) memory_bitmap[i] = 0;
    
    uint32_t reserved_end = HEAP_START + HEAP_SIZE;
    int reserved_pages = (reserved_end - MEM_START + PAGE_SIZE - 1) / PAGE_SIZE;
    
    for (int i = 0; i < reserved_pages; i++) {
        bitmap_set(i);
    }
    first_available_page = reserved_pages;
}

void init_heap() {
    irq_spinlock_init(&heap_lock);
    heap_start = (struct heap_block*)HEAP_START;
    
    heap_start->magic = HEAP_MAGIC;
    heap_start->size = HEAP_SIZE - sizeof(struct heap_block);
    heap_start->is_free = 1;
    heap_start->next = 0;
}

void init_paging() {
    for(int i = 0; i < 1024; i++) {
        if (page_directory[i] == 0) {
            page_directory[i] = 0x00000002; 
        }
    }

    for(int j = 0; j < 32; j++) {
        for(int i = 0; i < 1024; i++) {
            page_tables[j][i] = ((j * 1024 + i) * 4096) | 3;
        }
        page_directory[j] = ((uint32_t)page_tables[j]) | 3;
    }

    load_page_directory(page_directory);
    enable_paging();
}


void vmm_map(uint32_t* pd, uint32_t virtual_addr, uint32_t physical_addr, int flags) {
    if (!pd) return;
    if (virtual_addr % PAGE_SIZE != 0 || physical_addr % PAGE_SIZE != 0) {
        kprint("[ERR] vmm_map: addresses must be page-aligned\n");
        return;
    }

    uint32_t pd_idx = PD_INDEX(virtual_addr);
    uint32_t pt_idx = PT_INDEX(virtual_addr);

    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        uint32_t* pt = (uint32_t*)kalloc();
        for (int i = 0; i < 1024; i++) pt[i] = 0;
        pd[pd_idx] = ((uint32_t)pt) | flags | PAGE_PRESENT;
    } else {
        pd[pd_idx] |= (flags & (PAGE_USER | PAGE_RW));
    }

    uint32_t* pt = (uint32_t*)(pd[pd_idx] & ~0xFFF);
    pt[pt_idx] = (physical_addr & ~0xFFF) | flags | PAGE_PRESENT;
}

uint32_t* vmm_create_address_space() {
    uint32_t* pd = (uint32_t*)kalloc();
    if (!pd) return 0;

    for (int i = 0; i < 32; i++) {
        pd[i] = page_directory[i]; 
    }

    for (int i = 32; i < 1024; i++) {
        pd[i] = 0;
    }

    return pd;
}

void kfree_page(void* ptr) {
    if (!ptr) return;
    uint32_t addr = (uint32_t)ptr;
    if (addr < MEM_START) return;
    int page_idx = (addr - MEM_START) / PAGE_SIZE;
    if (page_idx < 0 || page_idx >= TOTAL_PAGES) return;
    irq_spinlock_acquire(&page_lock);
    memory_bitmap[page_idx / 8] &= ~(1 << (page_idx % 8));
    if (page_idx < first_available_page) {
        first_available_page = page_idx;
    }
    irq_spinlock_release(&page_lock);
}

void vmm_free_address_space(uint32_t* pd) {
    if (!pd) return;
    for (int i = 32; i < 1024; i++) {
        if (pd[i] & PAGE_PRESENT) {
            uint32_t* pt = (uint32_t*)(pd[i] & ~0xFFF);
            for (int j = 0; j < 1024; j++) {
                if (pt[j] & PAGE_PRESENT) {
                    kfree_page((void*)(pt[j] & ~0xFFF));
                }
            }
            kfree_page(pt);
        }
    }
    kfree_page(pd);
}

void vmm_copy_address_space(uint32_t* src_pd, uint32_t* dst_pd) {
    if (!src_pd || !dst_pd) return;
    for (int i = 32; i < 1024; i++) {
        if (src_pd[i] & PAGE_PRESENT) {
            uint32_t* src_pt = (uint32_t*)(src_pd[i] & ~0xFFF);
            uint32_t* dst_pt = (uint32_t*)kalloc();
            if (!dst_pt) continue;
            for (int j = 0; j < 1024; j++) dst_pt[j] = 0;

            for (int j = 0; j < 1024; j++) {
                if (src_pt[j] & PAGE_PRESENT) {
                    void* new_page = kalloc();
                    if (!new_page) continue;
                    memory_copy(new_page, (void*)(src_pt[j] & ~0xFFF), PAGE_SIZE);
                    dst_pt[j] = ((uint32_t)new_page & ~0xFFF) | (src_pt[j] & 0xFFF);
                }
            }
            dst_pd[i] = ((uint32_t)dst_pt) | (src_pd[i] & 0xFFF);
        }
    }
}

void* kalloc() {
    irq_spinlock_acquire(&page_lock);
    for (int i = first_available_page; i < TOTAL_PAGES; i++) {
        if (!(memory_bitmap[i / 8] & (1 << (i % 8)))) {
            bitmap_set(i);
            first_available_page = i + 1;
            irq_spinlock_release(&page_lock);
            return (void*)(MEM_START + i * PAGE_SIZE);
        }
    }
    irq_spinlock_release(&page_lock);
    return 0;
}

void* kmalloc(uint32_t size) {
    if (size == 0) return 0;

    size = (size + 7) & ~7;

    irq_spinlock_acquire(&heap_lock);

    struct heap_block* current = heap_start;
    struct heap_block* best_fit = 0;

    while (current != 0) {
        if (current->magic != HEAP_MAGIC) {
            kprint("[FATAL] Heap corruption detected!\n");
            irq_spinlock_release(&heap_lock);
            return 0;
        }

        if (current->is_free && current->size >= size) {
            if (!best_fit || current->size < best_fit->size) {
                best_fit = current;
            }
        }
        current = current->next;
    }

    if (best_fit) {
        if (best_fit->size >= (size + sizeof(struct heap_block) + 8)) {
            struct heap_block* next_block = (struct heap_block*)((uint8_t*)best_fit + sizeof(struct heap_block) + size);
            
            next_block->magic = HEAP_MAGIC;
            next_block->size = best_fit->size - size - sizeof(struct heap_block);
            next_block->is_free = 1;
            next_block->next = best_fit->next;
            
            best_fit->size = size;
            best_fit->next = next_block;
        }
        
        best_fit->is_free = 0;
        irq_spinlock_release(&heap_lock);
        return (void*)((uint8_t*)best_fit + sizeof(struct heap_block));
    }

    irq_spinlock_release(&heap_lock);
    kprint("[ERR] kmalloc: Out of heap memory\n");
    return 0;
}

void* kmalloc_aligned(uint32_t size, uint32_t align) {
    uint8_t* raw = (uint8_t*)kmalloc(size + align);
    if (!raw) return NULL;
    uint32_t addr = (uint32_t)(uintptr_t)raw;
    uint32_t aligned = (addr + align - 1) & ~(align - 1);
    return (void*)(uintptr_t)aligned;
}

void kfree_heap(void* ptr) {
    if (!ptr) return;

    irq_spinlock_acquire(&heap_lock);

    struct heap_block* block = (struct heap_block*)((uint8_t*)ptr - sizeof(struct heap_block));
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
    irq_spinlock_release(&heap_lock);
}

uint32_t get_free_heap_memory() {
    uint32_t free_mem = 0;
    irq_spinlock_acquire(&heap_lock);
    struct heap_block* current = heap_start;
    while (current != 0) {
        if (current->is_free) {
            free_mem += current->size;
        }
        current = current->next;
    }
    irq_spinlock_release(&heap_lock);
    return free_mem;
}