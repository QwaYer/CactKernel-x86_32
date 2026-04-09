#include "mmap.h"
#include "memory.h"
#include "page_fault.h"
#include "kernel.h"
#include "vfs.h"
#include "task.h"
#include "klib.h"

static struct vfs_node* fd_to_node(int fd)
{
    if (!current_task) return 0;
    if (fd < 0 || fd >= MAX_FD) return 0;
    return current_task->fd_table[fd];
}

static int prot_to_page_flags(int prot, int user)
{
    int f = PAGE_PRESENT;
    if (prot & PROT_WRITE) f |= PAGE_RW;
    if (user)               f |= PAGE_USER;
    return f;
}

static uint32_t find_free_va(mmap_table_t* tbl, uint32_t length)
{
    uint32_t candidate = tbl->next_base;
    candidate = (candidate + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1u);

retry:
    if (candidate + length > MMAP_LIMIT || candidate + length < candidate)
        return 0;

    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        mmap_region_t* r = &tbl->regions[i];
        if (!r->is_used) continue;
        uint32_t r_end = r->base + r->length;
        uint32_t c_end = candidate + length;
        if (candidate < r_end && c_end > r->base) {
            candidate = r_end;
            candidate = (candidate + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1u);
            goto retry;
        }
    }

    return candidate;
}

static mmap_region_t* alloc_region_slot(mmap_table_t* tbl)
{
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (!tbl->regions[i].is_used)
            return &tbl->regions[i];
    }
    return 0;
}

void mmap_table_init(mmap_table_t* tbl)
{
    if (!tbl) return;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        tbl->regions[i].is_used = 0;
        tbl->regions[i].fd      = -1;
    }
    tbl->next_base = MMAP_BASE;
}

mmap_region_t* mmap_find_region(mmap_table_t* tbl, uint32_t addr)
{
    if (!tbl) return 0;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        mmap_region_t* r = &tbl->regions[i];
        if (!r->is_used) continue;
        if (addr >= r->base && addr < r->base + r->length)
            return r;
    }
    return 0;
}

void* do_mmap(uint32_t* pd, mmap_table_t* tbl,
              uint32_t hint, uint32_t length,
              int prot, int flags,
              int fd, uint32_t offset)
{
    if (!pd || !tbl || length == 0) return MAP_FAILED;

    if (!(flags & MAP_SHARED) && !(flags & MAP_PRIVATE))
        return MAP_FAILED;

    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1u);

    uint32_t va;
    if ((flags & MAP_FIXED) && hint != 0) {
        if (hint % PAGE_SIZE != 0) return MAP_FAILED;
        va = hint;
    } else {
        va = find_free_va(tbl, length);
        if (va == 0) {
            kprint("[MMAP] do_mmap: no free virtual address space\n");
            return MAP_FAILED;
        }
    }

    mmap_region_t* region = alloc_region_slot(tbl);
    if (!region) {
        kprint("[MMAP] do_mmap: region table full\n");
        return MAP_FAILED;
    }

    int page_flags = prot_to_page_flags(prot, 1);
    uint32_t pages = length / PAGE_SIZE;

    if (flags & MAP_ANON) {
        if (vmm_map_zero(pd, va, length, page_flags) != 0) {
            kprint("[MMAP] do_mmap: vmm_map_zero failed\n");
            return MAP_FAILED;
        }
    } else {
        if (fd < 0) return MAP_FAILED;

        struct vfs_node* node = fd_to_node(fd);
        uint32_t file_off = offset;

        for (uint32_t i = 0; i < pages; i++) {
            void* phys = kalloc();
            if (!phys) {
                do_munmap(pd, tbl, va, i * PAGE_SIZE);
                return MAP_FAILED;
            }

            uint8_t* p = (uint8_t*)phys;
            for (uint32_t b = 0; b < PAGE_SIZE; b++) p[b] = 0;

            if (node) read_vfs(node, file_off, PAGE_SIZE, (char*)phys);

            vmm_map(pd, va + i * PAGE_SIZE, (uint32_t)phys, page_flags);
            file_off += PAGE_SIZE;
        }
    }

    region->base     = va;
    region->length   = length;
    region->flags    = (uint32_t)flags;
    region->prot     = (uint32_t)prot;
    region->fd       = (flags & MAP_ANON) ? -1 : fd;
    region->file_off = offset;
    region->is_used  = 1;

    if (va + length > tbl->next_base)
        tbl->next_base = va + length;

    return (void*)va;
}

int do_munmap(uint32_t* pd, mmap_table_t* tbl,
              uint32_t addr, uint32_t length)
{
    if (!pd || !tbl || length == 0) return -1;
    if (addr % PAGE_SIZE != 0) return -1;

    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1u);

    mmap_region_t* region = 0;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        mmap_region_t* r = &tbl->regions[i];
        if (!r->is_used) continue;
        if (r->base == addr && r->length == length) {
            region = r;
            break;
        }
        if (r->base <= addr && addr + length <= r->base + r->length) {
            region = r;
            break;
        }
    }

    if (!region) return -1;

    uint32_t pages = length / PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t va  = addr + i * PAGE_SIZE;
        uint32_t pdi = PD_INDEX(va);
        if (!(pd[pdi] & PAGE_PRESENT)) continue;
        uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
        uint32_t pte = pt[PT_INDEX(va)];

        if (pte & PAGE_PRESENT) {
            kfree_page((void*)(pte & ~0xFFFu));
        }
        pt[PT_INDEX(va)] = 0;
        __asm__ __volatile__("invlpg (%0)" :: "r"(va) : "memory");
    }

    if (addr == region->base && length >= region->length) {
        region->is_used = 0;
        region->fd      = -1;
    } else if (addr == region->base) {
        region->base     += length;
        region->length   -= length;
        region->file_off += length;
    } else {
        region->length = addr - region->base;
    }

    return 0;
}

int do_mprotect(uint32_t* pd, mmap_table_t* tbl,
                uint32_t addr, uint32_t length, int prot)
{
    if (!pd || !tbl || length == 0) return -1;
    if (addr % PAGE_SIZE != 0) return -1;

    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1u);

    mmap_region_t* region = mmap_find_region(tbl, addr);
    if (!region) return -1;

    int page_flags = prot_to_page_flags(prot, 1);
    uint32_t pages = length / PAGE_SIZE;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t va  = addr + i * PAGE_SIZE;
        uint32_t pdi = PD_INDEX(va);
        if (!(pd[pdi] & PAGE_PRESENT)) continue;
        uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
        uint32_t pte = pt[PT_INDEX(va)];
        if (!(pte & PAGE_PRESENT)) continue;
        pt[PT_INDEX(va)] = (pte & ~0xFFFu) | (uint32_t)page_flags;
        __asm__ __volatile__("invlpg (%0)" :: "r"(va) : "memory");
    }

    region->prot = (uint32_t)prot;
    return 0;
}

int mmap_handle_fault(uint32_t* pd, mmap_table_t* tbl, uint32_t fault_addr)
{
    if (!pd || !tbl) return -1;

    mmap_region_t* region = mmap_find_region(tbl, fault_addr);
    if (!region) return -1;

    uint32_t page_va = fault_addr & ~0xFFFu;
    uint32_t pdi     = PD_INDEX(page_va);
    uint32_t pti     = PT_INDEX(page_va);

    if (!(pd[pdi] & PAGE_PRESENT)) return -1;
    uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);

    if (pt[pti] & PAGE_PRESENT) return -1;

    void* phys = kalloc();
    if (!phys) return -1;

    uint8_t* p = (uint8_t*)phys;
    for (uint32_t b = 0; b < PAGE_SIZE; b++) p[b] = 0;

    if (region->fd >= 0) {
        uint32_t page_offset = page_va - region->base;
        uint32_t file_off    = region->file_off + page_offset;
        struct vfs_node* node = fd_to_node(region->fd);
        if (node) read_vfs(node, file_off, PAGE_SIZE, (char*)phys);
    }

    int page_flags = prot_to_page_flags((int)region->prot, 1);
    pt[pti] = ((uint32_t)phys & ~0xFFFu) | (uint32_t)page_flags;
    __asm__ __volatile__("invlpg (%0)" :: "r"(page_va) : "memory");

    return 0;
}

void mmap_table_clone(mmap_table_t* src, mmap_table_t* dst,
                      uint32_t* src_pd, uint32_t* dst_pd)
{
    if (!src || !dst) return;

    dst->next_base = src->next_base;

    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        mmap_region_t* sr = &src->regions[i];
        mmap_region_t* dr = &dst->regions[i];

        if (!sr->is_used) {
            dr->is_used = 0;
            continue;
        }

        *dr = *sr;

        uint32_t pages = sr->length / PAGE_SIZE;

        if (sr->flags & MAP_SHARED) {
            for (uint32_t p = 0; p < pages; p++) {
                uint32_t va  = sr->base + p * PAGE_SIZE;
                uint32_t pdi = PD_INDEX(va);
                uint32_t pti = PT_INDEX(va);

                if (!(src_pd[pdi] & PAGE_PRESENT)) continue;
                uint32_t* src_pt = (uint32_t*)(src_pd[pdi] & ~0xFFFu);
                uint32_t pte = src_pt[pti];
                if (!pte) continue;

                if (!(dst_pd[pdi] & PAGE_PRESENT)) {
                    uint32_t* new_pt = (uint32_t*)kalloc();
                    if (!new_pt) continue;
                    for (int k = 0; k < 1024; k++) new_pt[k] = 0;
                    dst_pd[pdi] = ((uint32_t)new_pt & ~0xFFFu)
                                | PAGE_PRESENT | PAGE_RW | PAGE_USER;
                }
                uint32_t* dst_pt = (uint32_t*)(dst_pd[pdi] & ~0xFFFu);
                dst_pt[pti] = pte;
            }
        } else {
            for (uint32_t p = 0; p < pages; p++) {
                uint32_t va  = sr->base + p * PAGE_SIZE;
                uint32_t pdi = PD_INDEX(va);
                uint32_t pti = PT_INDEX(va);

                if (!(src_pd[pdi] & PAGE_PRESENT)) continue;
                uint32_t* src_pt = (uint32_t*)(src_pd[pdi] & ~0xFFFu);
                uint32_t pte = src_pt[pti];

                if (!(dst_pd[pdi] & PAGE_PRESENT)) {
                    uint32_t* new_pt = (uint32_t*)kalloc();
                    if (!new_pt) continue;
                    for (int k = 0; k < 1024; k++) new_pt[k] = 0;
                    dst_pd[pdi] = ((uint32_t)new_pt & ~0xFFFu)
                                | PAGE_PRESENT | PAGE_RW | PAGE_USER;
                }
                uint32_t* dst_pt = (uint32_t*)(dst_pd[pdi] & ~0xFFFu);

                if (!(pte & PAGE_PRESENT)) {
                    dst_pt[pti] = pte;
                    continue;
                }

                uint32_t cow_pte = (pte & ~(uint32_t)PAGE_RW) | PAGE_COW;
                src_pt[pti] = cow_pte;
                dst_pt[pti] = cow_pte;
            }

            __asm__ __volatile__(
                "mov %%cr3, %%eax\n\t"
                "mov %%eax, %%cr3\n\t"
                ::: "eax", "memory");
        }
    }
}

void mmap_table_free(mmap_table_t* tbl, uint32_t* pd)
{
    if (!tbl || !pd) return;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        mmap_region_t* r = &tbl->regions[i];
        if (!r->is_used) continue;
        do_munmap(pd, tbl, r->base, r->length);
    }
}

void mmap_print_regions(const mmap_table_t* tbl)
{
    if (!tbl) return;
    char buf[16];
    kprint("[MMAP] === Memory Regions ===\n");
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        const mmap_region_t* r = &tbl->regions[i];
        if (!r->is_used) continue;
        kprint("  [");
        itoa(i, buf);                  kprint(buf);
        kprint("] base=0x");           hex_to_ascii(r->base, buf);    kprint(buf);
        kprint(" len=0x");             hex_to_ascii(r->length, buf);  kprint(buf);
        kprint(" prot=");              itoa((int)r->prot, buf);       kprint(buf);
        kprint(" flags=");             itoa((int)r->flags, buf);      kprint(buf);
        kprint(" fd=");                itoa(r->fd, buf);              kprint(buf);
        kprint("\n");
    }
}