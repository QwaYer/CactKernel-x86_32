#include "memory.h"
#include "page_fault.h"   
#include "kernel.h"

static void _zero_page(void* p)
{
    uint8_t* b = (uint8_t*)p;
    for (uint32_t i = 0; i < PAGE_SIZE; i++) b[i] = 0;
}

void vmm_fork_address_space(uint32_t* src_pd, uint32_t* dst_pd)
{
    if (!src_pd || !dst_pd) return;

    for (int i = 32; i < 1024; i++) {
        if (!(src_pd[i] & PAGE_PRESENT)) {
            dst_pd[i] = 0;
            continue;
        }

        uint32_t* src_pt = (uint32_t*)(src_pd[i] & ~0xFFFu);

        uint32_t* dst_pt = (uint32_t*)kalloc();
        if (!dst_pt) continue;
        _zero_page(dst_pt);

        for (int j = 0; j < 1024; j++) {
            uint32_t pte = src_pt[j];

            if (!(pte & PAGE_PRESENT)) {
                dst_pt[j] = pte;
                continue;
            }

            uint32_t cow_pte = (pte & ~(uint32_t)PAGE_RW) | PAGE_COW;
            src_pt[j] = cow_pte;  
            dst_pt[j] = cow_pte;   
        }

        dst_pd[i] = ((uint32_t)dst_pt & ~0xFFFu) | (src_pd[i] & 0xFFFu);
    }

    __asm__ __volatile__(
        "mov %%cr3, %%eax\n\t"
        "mov %%eax, %%cr3\n\t"
        ::: "eax", "memory"
    );
}