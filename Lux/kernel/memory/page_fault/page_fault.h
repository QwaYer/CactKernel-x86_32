#ifndef PAGE_FAULT_H
#define PAGE_FAULT_H

#include <stdint.h>
#include "memory.h"
#include "kernel.h"


#define PAGE_COW       0x200  // Copy-on-Write страница (bit 9)         
#define PAGE_DEMAND    0x400  // Demand paging (bit 10)                  
#define PAGE_ZERO      0x800  // Zero page (bit 11)                      


#define USER_STACK_TOP    0xC0000000   // Верхушка стека пользователя  
#define USER_STACK_LIMIT  0xBF000000   // Нижний лимит стека (16 MB)   
#define USER_HEAP_START   0x40000000   // Начало кучи                  
#define USER_HEAP_LIMIT   0x80000000   // Лимит кучи                   


#define PF_PRESENT     0x01   // Страница присутствует (защита)          
#define PF_WRITE       0x02   // Попытка записи                          
#define PF_USER        0x04   // Из user mode                           
#define PF_RESERVED    0x08   // Зарезервированные биты                  
#define PF_INSTR_FETCH 0x10   // Instruction fetch                       


typedef struct {
    uint32_t total_faults;        // Всего page fault'ов             
    uint32_t demand_allocs;       // Demand paging аллокаций         
    uint32_t cow_copies;          // Copy-on-Write копирований       
    uint32_t stack_grows;         // Расширений стека                
    uint32_t zero_pages;          // Zero page аллокаций             
    uint32_t protection_faults;   // Нарушений защиты (killed)      
    uint32_t invalid_access;      // Невалидных обращений (killed)   
} pf_stats_t;


// Public API                                                             
void page_fault_init(void);

void page_fault_handler(struct context_frame* regs);


int vmm_map_cow(uint32_t* pd, uint32_t virtual_addr);


int vmm_map_demand(uint32_t* pd, uint32_t virtual_addr, uint32_t size, int flags);

int vmm_map_zero(uint32_t* pd, uint32_t virtual_addr, uint32_t size, int flags);


uint32_t vmm_setup_user_stack(uint32_t* pd, uint32_t initial_size);

pf_stats_t pf_get_stats(void);

void pf_print_stats(void);

int vmm_is_cow_page(uint32_t* pd, uint32_t virtual_addr);

int vmm_handle_cow(uint32_t* pd, uint32_t virtual_addr);


static inline int vmm_is_user_address(uint32_t addr) {
    return addr < USER_STACK_TOP;
}

static inline int vmm_is_valid_stack_addr(uint32_t addr) {
    return addr >= USER_STACK_LIMIT && addr < USER_STACK_TOP;
}

static inline int vmm_is_valid_heap_addr(uint32_t addr) {
    return addr >= USER_HEAP_START && addr < USER_HEAP_LIMIT;
}

#endif 