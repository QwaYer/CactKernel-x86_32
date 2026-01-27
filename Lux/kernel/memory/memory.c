#include "memory.h"

unsigned char memory_bitmap[BITMAP_SIZE];
struct heap_block* heap_start = (struct heap_block*)HEAP_START;

// ==================== BITMAP UTILS ====================

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

// ==================== MANAGER INIT ====================

void init_memory_manager() {
    // 1. Полная очистка битмапа страниц
    for (int i = 0; i < BITMAP_SIZE; i++) {
        memory_bitmap[i] = 0;
    }

    // 2. Резервирование страниц кучи в битмапе, чтобы kalloc их не выдавал
    int heap_start_page = (HEAP_START - MEM_START) / PAGE_SIZE;
    int heap_pages = HEAP_SIZE / PAGE_SIZE;
    for (int i = 0; i < heap_pages; i++) {
        bitmap_set(heap_start_page + i);
    }

    // 3. Инициализация структуры кучи внутри зарезервированной области
    init_heap();
}

// ==================== PAGE ALLOCATOR ====================

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

// ==================== HEAP ALLOCATOR (kmalloc) ====================

void init_heap() {
    heap_start->magic = HEAP_MAGIC;
    heap_start->size = HEAP_SIZE - sizeof(struct heap_block);
    heap_start->is_free = 1;
    heap_start->next = 0;
}

void* kmalloc(unsigned int size) {
    if (size == 0) return 0;
    
    // Выравнивание запроса до 4 байт для производительности
    if (size % 4 != 0) {
        size += 4 - (size % 4);
    }
    
    struct heap_block* current = heap_start;
    
    while (current) {
        if (current->is_free && current->size >= size) {
            // Разделение блока, если он значительно больше запроса
            if (current->size > size + sizeof(struct heap_block) + 16) {
                struct heap_block* new_block = (struct heap_block*)((unsigned char*)current + sizeof(struct heap_block) + size);
                new_block->magic = HEAP_MAGIC;
                new_block->size = current->size - size - sizeof(struct heap_block);
                new_block->is_free = 1;
                new_block->next = current->next;
                
                current->size = size;
                current->next = new_block;
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
    
    // Проверка границ: адрес должен быть внутри области кучи
    unsigned int addr = (unsigned int)ptr;
    if (addr < HEAP_START || addr >= (HEAP_START + HEAP_SIZE)) return;

    struct heap_block* block = (struct heap_block*)((unsigned char*)ptr - sizeof(struct heap_block));
    
    // Проверка магического числа для предотвращения повреждения структуры
    if (block->magic != HEAP_MAGIC) return;

    block->is_free = 1;
    
    // Coalescing: объединение идущих подряд свободных блоков
    struct heap_block* current = heap_start;
    while (current && current->next) {
        if (current->is_free && current->next->is_free) {
            current->size += sizeof(struct heap_block) + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}

unsigned int get_free_memory() {
    unsigned int free_mem = 0;
    struct heap_block* current = heap_start;
    
    while (current) {
        if (current->is_free) {
            free_mem += current->size;
        }
        current = current->next;
    }
    
    return free_mem;
}

// ==================== UTILITIES ====================

void memory_copy(void* source, void* dest, int n) {
    char* src = (char*)source;
    char* dst = (char*)dest;
    for (int i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

void memory_set(void* dest, unsigned char val, int n) {
    unsigned char* dst = (unsigned char*)dest;
    for (int i = 0; i < n; i++) {
        dst[i] = val;
    }
}

int memory_compare(void* s1, void* s2, int n) {
    unsigned char* p1 = (unsigned char*)s1;
    unsigned char* p2 = (unsigned char*)s2;
    for (int i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return (p1[i] < p2[i]) ? -1 : 1;
        }
    }
    return 0;
}