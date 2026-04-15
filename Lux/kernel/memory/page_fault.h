#ifndef PAGE_FAULT_H
#define PAGE_FAULT_H

#include <stdint.h>
#include "memory.h"

#define PAGE_COW       0x200
#define PAGE_DEMAND    0x400
#define PAGE_ZERO      0x800

#define USER_STACK_TOP    0xC0000000
#define USER_STACK_LIMIT  0xBF000000
#define USER_HEAP_START   0x40000000
#define USER_HEAP_LIMIT   0x80000000

#define PF_PRESENT     0x01
#define PF_WRITE       0x02
#define PF_USER        0x04
#define PF_RESERVED    0x08
#define PF_INSTR_FETCH 0x10

typedef struct {
    uint32_t total_faults;
    uint32_t demand_allocs;
    uint32_t cow_copies;
    uint32_t stack_grows;
    uint32_t zero_pages;
    uint32_t swap_ins;
    uint32_t protection_faults;
    uint32_t invalid_access;
} pf_stats_t;

struct context_frame;

void       page_fault_init(void);
void       page_fault_handler(struct context_frame* regs);
int        vmm_map_cow(uint32_t* pd, uint32_t virtual_addr);
int        vmm_map_demand(uint32_t* pd, uint32_t virtual_addr, uint32_t size, int flags);
int        vmm_map_zero(uint32_t* pd, uint32_t virtual_addr, uint32_t size, int flags);
uint32_t   vmm_setup_user_stack(uint32_t* pd, uint32_t initial_size);
pf_stats_t pf_get_stats(void);
void       pf_print_stats(void);
int        vmm_is_cow_page(uint32_t* pd, uint32_t virtual_addr);
int        vmm_handle_cow(uint32_t* pd, uint32_t virtual_addr);

static inline int vmm_is_user_address(uint32_t addr) {
    return addr >= 0x08000000U && addr < USER_STACK_TOP;
}

static inline int vmm_is_valid_stack_addr(uint32_t addr) {
    return addr >= USER_STACK_LIMIT && addr < USER_STACK_TOP;
}

static inline int vmm_is_valid_heap_addr(uint32_t addr) {
    return addr >= USER_HEAP_START && addr < USER_HEAP_LIMIT;
}

#endif
