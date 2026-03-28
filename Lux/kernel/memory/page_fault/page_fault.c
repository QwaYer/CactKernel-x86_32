#include "page_fault.h"
#include "memory.h"
#include "kernel.h"
#include "task.h"
#include "libc.h"
#include "swap.h"
#include "oom.h"

static pf_stats_t g_stats;

static uint32_t* pte_get(uint32_t* pd, uint32_t vaddr)
{
    if (!pd) return 0;
    uint32_t pde = pd[PD_INDEX(vaddr)];
    if (!(pde & PAGE_PRESENT)) return 0;
    uint32_t* pt = (uint32_t*)(pde & ~0xFFFu);
    return &pt[PT_INDEX(vaddr)];
}

static void zero_page(void* phys)
{
    uint8_t* p = (uint8_t*)phys;
    for (uint32_t i = 0; i < PAGE_SIZE; i++) p[i] = 0;
}

static inline void tlb_flush(uint32_t vaddr)
{
    __asm__ __volatile__("invlpg (%0)" :: "r"(vaddr) : "memory");
}

static void kill_current(uint32_t fault_addr, uint32_t err, uint32_t eip)
{
    struct task_struct* t = (struct task_struct*)current_task;

    kprint_color("\n[PF] SEGFAULT pid=", COLOR_LIGHT_RED);
    char buf[12];
    itoa(t ? (int)t->pid : -1, buf);
    kprint_color(buf, COLOR_LIGHT_RED);

    kprint_color(" addr=0x", COLOR_LIGHT_RED); kprint_hex(fault_addr);
    kprint_color(" err=0x",  COLOR_LIGHT_RED); kprint_hex(err);
    kprint_color(" eip=0x",  COLOR_LIGHT_RED); kprint_hex(eip);
    kprint("\n");

    g_stats.protection_faults++;

    if (t && !t->is_kernel) {
        task_signal(t->pid, SIGKILL);
        schedule();
        return;
    }

    kprint_color("[PF] KERNEL PAGE FAULT — SYSTEM HALTED\n", COLOR_LIGHT_RED);
    while (1) __asm__ __volatile__("hlt");
}

void page_fault_init(void)
{
    kprint("[PF] clearing stats  handlers: COW demand-paging stack-grow swap-in SEGFAULT\n");
    uint8_t* p = (uint8_t*)&g_stats;
    for (uint32_t i = 0; i < sizeof(g_stats); i++) p[i] = 0;
}

void page_fault_handler(struct context_frame* regs)
{
    uint32_t fault_addr = read_cr2();
    uint32_t err        = regs->err_code;
    uint32_t eip        = regs->eip;

    g_stats.total_faults++;

    struct task_struct* t = (struct task_struct*)current_task;
    uint32_t* pd = (t && t->page_directory) ? t->page_directory
                                             : get_current_pd();

    if ((err & PF_PRESENT) && (err & PF_WRITE)) {
        uint32_t* pte = pte_get(pd, fault_addr);
        if (pte && (*pte & PAGE_COW)) {
            if (vmm_handle_cow(pd, fault_addr & ~0xFFFu) == 0) {
                g_stats.cow_copies++;
                return;
            }
        }
        kill_current(fault_addr, err, eip);
        return;
    }

    if (!(err & PF_PRESENT)) {
        uint32_t page_va = fault_addr & ~0xFFFu;
        uint32_t* pte    = pte_get(pd, fault_addr);

        if (pte && swap_pte_is_swapped(*pte)) {
            if (swap_handle_fault(pd, fault_addr) == 0) {
                g_stats.swap_ins++;
                return;
            }
            kill_current(fault_addr, err, eip);
            return;
        }

        if (pte && (*pte & PAGE_DEMAND)) {
            void* phys = kalloc();
            if (!phys && oom_kill() == 0)
                phys = kalloc();
            if (!phys) { kill_current(fault_addr, err, eip); return; }

            if (*pte & PAGE_ZERO) {
                zero_page(phys);
                g_stats.zero_pages++;
            }

            uint32_t flags = (*pte & 0xFFFu) & ~(PAGE_DEMAND | PAGE_ZERO);
            flags |= PAGE_PRESENT | PAGE_RW;
            *pte = ((uint32_t)phys & ~0xFFFu) | flags;
            tlb_flush(page_va);

            g_stats.demand_allocs++;
            return;
        }

        if (pte && (*pte & PAGE_COW)) {
            void* phys = kalloc();
            if (!phys && oom_kill() == 0)
                phys = kalloc();
            if (!phys) { kill_current(fault_addr, err, eip); return; }
            zero_page(phys);

            uint32_t flags = (*pte & 0xFFFu) & ~(uint32_t)PAGE_COW;
            flags |= PAGE_PRESENT | PAGE_RW;
            *pte = ((uint32_t)phys & ~0xFFFu) | flags;
            tlb_flush(page_va);

            g_stats.demand_allocs++;
            return;
        }

        if (vmm_is_valid_stack_addr(fault_addr)) {
            void* phys = kalloc();
            if (!phys && oom_kill() == 0)
                phys = kalloc();
            if (!phys) { kill_current(fault_addr, err, eip); return; }

            zero_page(phys);
            vmm_map(pd, page_va, (uint32_t)phys,
                    PAGE_PRESENT | PAGE_RW | PAGE_USER);
            tlb_flush(page_va);

            g_stats.stack_grows++;
            return;
        }

        if (fault_addr < 0xC0000000u && (err & PF_USER)) {
            uint32_t pdi = PD_INDEX(fault_addr);
            if (!(pd[pdi] & PAGE_PRESENT)) {
                kprint("[PF] DBG: no PDE for addr=0x"); kprint_hex(fault_addr); kprint("\n");
            } else if (!pte) {
                kprint("[PF] DBG: pte_get returned null for addr=0x"); kprint_hex(fault_addr); kprint("\n");
            } else {
                kprint("[PF] DBG: PTE=0x"); kprint_hex(*pte); kprint(" addr=0x"); kprint_hex(fault_addr); kprint("\n");
            }
        }

        g_stats.invalid_access++;
        kill_current(fault_addr, err, eip);
        return;
    }
    kill_current(fault_addr, err, eip);
}

int vmm_map_demand(uint32_t* pd, uint32_t virtual_addr,
                   uint32_t size, int flags)
{
    if (!pd || size == 0) return -1;

    uint32_t start = virtual_addr & ~0xFFFu;
    uint32_t end   = (virtual_addr + size + 0xFFF) & ~0xFFFu;

    for (uint32_t va = start; va < end; va += PAGE_SIZE) {
        uint32_t pdi = PD_INDEX(va);

        if (!(pd[pdi] & PAGE_PRESENT)) {
            uint32_t* pt = (uint32_t*)kalloc();
            if (!pt) return -1;
            zero_page(pt);
            pd[pdi] = ((uint32_t)pt & ~0xFFFu)
                    | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);
        }

        uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
        pt[PT_INDEX(va)] = PAGE_DEMAND | (flags & PAGE_USER);
    }
    return 0;
}

int vmm_map_zero(uint32_t* pd, uint32_t virtual_addr,
                 uint32_t size, int flags)
{
    if (!pd || size == 0) return -1;

    uint32_t start = virtual_addr & ~0xFFFu;
    uint32_t end   = (virtual_addr + size + 0xFFF) & ~0xFFFu;

    for (uint32_t va = start; va < end; va += PAGE_SIZE) {
        uint32_t pdi = PD_INDEX(va);

        if (!(pd[pdi] & PAGE_PRESENT)) {
            uint32_t* pt = (uint32_t*)kalloc();
            if (!pt) return -1;
            zero_page(pt);
            pd[pdi] = ((uint32_t)pt & ~0xFFFu)
                    | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);
        }

        uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
        pt[PT_INDEX(va)] = PAGE_DEMAND | PAGE_ZERO | (flags & PAGE_USER);
    }
    return 0;
}

int vmm_map_cow(uint32_t* pd, uint32_t virtual_addr)
{
    uint32_t* pte = pte_get(pd, virtual_addr);
    if (!pte || !(*pte & PAGE_PRESENT)) return -1;

    *pte = (*pte & ~(uint32_t)PAGE_RW) | PAGE_COW;
    tlb_flush(virtual_addr & ~0xFFFu);
    return 0;
}

int vmm_is_cow_page(uint32_t* pd, uint32_t virtual_addr)
{
    uint32_t* pte = pte_get(pd, virtual_addr);
    return (pte && (*pte & PAGE_COW)) ? 1 : 0;
}

int vmm_handle_cow(uint32_t* pd, uint32_t virtual_addr)
{
    uint32_t* pte = pte_get(pd, virtual_addr);
    if (!pte || !(*pte & PAGE_COW)) return -1;

    uint32_t old_phys = *pte & ~0xFFFu;

    void* new_phys = kalloc();
    if (!new_phys) return -1;

    uint8_t* src = (uint8_t*)old_phys;
    uint8_t* dst = (uint8_t*)new_phys;
    for (uint32_t i = 0; i < PAGE_SIZE; i++) dst[i] = src[i];

    uint32_t flags = (*pte & 0xFFFu) & ~(uint32_t)PAGE_COW;
    flags |= PAGE_RW | PAGE_PRESENT;
    *pte = ((uint32_t)new_phys & ~0xFFFu) | flags;

    tlb_flush(virtual_addr & ~0xFFFu);
    return 0;
}

uint32_t vmm_setup_user_stack(uint32_t* pd, uint32_t initial_size)
{
    if (!pd) return 0;

    initial_size = (initial_size + 0xFFFu) & ~0xFFFu;
    if (initial_size == 0) initial_size = PAGE_SIZE;

    uint32_t bottom = USER_STACK_TOP - initial_size;

    for (uint32_t va = bottom; va < USER_STACK_TOP; va += PAGE_SIZE) {
        void* phys = kalloc();
        if (!phys) return 0;
        zero_page(phys);
        vmm_map(pd, va, (uint32_t)phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    return USER_STACK_TOP;
}

pf_stats_t pf_get_stats(void) { return g_stats; }

void pf_print_stats(void)
{
    char buf[16];
    kprint("[PF] === Page Fault Statistics ===\n");
#define PF_STAT(label, field) \
    kprint("  " label ": "); itoa((int)g_stats.field, buf); \
    kprint(buf); kprint("\n");
    PF_STAT("total_faults",     total_faults)
    PF_STAT("demand_allocs",    demand_allocs)
    PF_STAT("cow_copies",       cow_copies)
    PF_STAT("stack_grows",      stack_grows)
    PF_STAT("zero_pages",       zero_pages)
    PF_STAT("swap_ins",         swap_ins)
    PF_STAT("prot_faults",      protection_faults)
    PF_STAT("invalid_access",   invalid_access)
#undef PF_STAT
}