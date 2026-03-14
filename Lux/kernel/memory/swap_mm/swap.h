#ifndef SWAP_H
#define SWAP_H

#include <stdint.h>
#include "memory.h"


#define SWAP_MAGIC          0x53574150u   
#define SWAP_MAX_SLOTS      65536u        
#define SWAP_BITMAP_SIZE    (SWAP_MAX_SLOTS / 8)
#define SWAP_DATA_START_LBA 1u            

#define PAGE_SWAPPED        0x200u

#define SWAP_SLOT_MASK      0xFFFFF000u
#define SWAP_SLOT_SHIFT     12u

typedef uint32_t swap_slot_t;

typedef struct {
    uint32_t total_slots;
    uint32_t used_slots;
    uint32_t pages_swapped_out;
    uint32_t pages_swapped_in;
    uint32_t swap_failures;
} swap_stats_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t total_slots;
    uint32_t reserved[5];
} __attribute__((packed)) swap_header_t;

typedef int (*swap_read_fn) (uint32_t lba, void* buf,       uint32_t sectors);
typedef int (*swap_write_fn)(uint32_t lba, const void* buf, uint32_t sectors);


//Public api
int          swap_init(swap_read_fn read_fn, swap_write_fn write_fn,
                       uint32_t slots);
int          swap_is_enabled(void);
int          swap_out_page(uint32_t phys_addr, swap_slot_t* out_slot);
int          swap_in_page(swap_slot_t slot, uint32_t phys_addr);
void         swap_free_slot(swap_slot_t slot);
swap_stats_t swap_get_stats(void);
void         swap_print_stats(void);

int swap_evict_page(uint32_t* pd);
int swap_handle_fault(uint32_t* pd, uint32_t fault_addr);

static inline uint32_t swap_encode_pte(swap_slot_t slot)
{
    return ((slot << SWAP_SLOT_SHIFT) & SWAP_SLOT_MASK) | PAGE_SWAPPED;
}

static inline swap_slot_t swap_decode_pte(uint32_t pte)
{
    return (pte & SWAP_SLOT_MASK) >> SWAP_SLOT_SHIFT;
}

static inline int swap_pte_is_swapped(uint32_t pte)
{
    return (!(pte & PAGE_PRESENT)) && (pte & PAGE_SWAPPED);
}

#endif 