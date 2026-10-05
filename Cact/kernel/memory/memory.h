#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include "multiboot2.h"

#define PAGE_SIZE 4096

/* Kernel load address */
#define MEM_START 0x00100000u

/*
 * PCI_HOLE_START is the user/kernel VA split (3 GiB / 1 GiB) and the default
 * physical MMIO base.  The real RAM/MMIO boundary is discovered at boot from
 * the Multiboot2 map (rust_mm pmm::ram_end): a board may have usable RAM above
 * this address, up to its PCI hole (e.g. 0xE0000000 = 3.5 GiB).
 */
#define PCI_HOLE_START  0xC0000000u

/* Worst-case frame counts for the full 4 GiB space; the static PMM arrays in
 * rust_mm are sized from these. */
#define TOTAL_PAGES     (1024u * 1024u)
#define BITMAP_SIZE     (TOTAL_PAGES / 8)       /* 128 KB */

/*
 * Fixed kernel windows (ACPI temp map + PCIe ECAM) live in the shared kernel
 * half (VA >= PCI_HOLE_START), so any mapping there is valid under any CR3.
 * Because of that sharing, two subsystems that pick the same VA silently alias
 * each other's physical pages, so the windows are laid out from one base.
 *
 * The base is chosen at boot by the PMM: max(RAM top, PCI_HOLE_START), 2 MiB
 * aligned.  Keeping it at or above the RAM top means the identity map (VA ==
 * PA) of the windows never shadows a managed frame.
 */
uint32_t cact_mmio_window_base(void);               /* rust_mm: pmm.rs */
#define KERNEL_MMIO_WINDOW_SIZE (256u * PAGE_SIZE)  /* 1 MB */

/*
 * Heap window.  The heap allocator starts right after the hard-reserved
 * low-memory zone so kmalloc() never overlaps kernel-reserved pages.
 */
#define HEAP_START (32u * 1024u * 1024u)   /* 0x02000000 */
#define HEAP_SIZE  (16u * 1024u * 1024u)   /* 16 MB heap window */
#define HEAP_MAGIC 0xDEADBEEFu

/* Hard reservation: everything below RESERVED_END is never given to kalloc(). */
#define RESERVED_END (32u * 1024u * 1024u)

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4
#define PAGE_PWT     0x8   /* write-through cache policy for MMIO mappings */
#define PAGE_PCD     0x10  /* cache-disable for MMIO/device memory          */
#define PAGE_PAT     0x80  /* PAT bit (selects PAT entry 4 = WC when PCD|PWT=0) */


void      pmm_init_from_mmap(const mb2_mmap_table_t* mmap);
void      init_memory_manager(void);
void*     kalloc(void);
void      free_page(void* ptr);
void      init_paging(void);
void      init_heap(void);
void*     kmalloc(uint32_t size);
void*     kmalloc_aligned(uint32_t size, uint32_t align);
void      kfree(void* ptr);
void      kfree(void* ptr);
uint32_t  get_free_heap_memory(void);
uint32_t  pmm_total_frames(void);
uint32_t  pmm_free_frames(void);
void      vmm_map(uint32_t* pd, uint32_t virtual_addr, uint32_t physical_addr, int flags);
uint32_t  vmm_get_phys(uint32_t* pd, uint32_t virtual_addr);
void      vmm_sync_kernel_mmio_mappings(uint32_t* pd);
uint32_t* vmm_create_address_space(void);
void      vmm_free_address_space(uint32_t* pd);
void      vmm_copy_address_space(uint32_t* src_pd, uint32_t* dst_pd);
extern void load_page_directory(uint32_t* directory);
extern void enable_paging(void);

#endif
