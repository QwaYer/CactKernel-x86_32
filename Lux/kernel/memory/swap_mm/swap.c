#include "swap.h"
#include "memory.h"
#include "kernel.h"
#include "sync.h"

static swap_read_fn  g_read  = 0;
static swap_write_fn g_write = 0;

static uint8_t       g_bitmap[SWAP_BITMAP_SIZE]; 
static uint32_t      g_total_slots = 0;
static int           g_enabled     = 0;

static swap_stats_t  g_stats;
static irq_spinlock_t g_swap_lock;

static uint32_t g_clock_pdi = 32;   
static uint32_t g_clock_ptj = 0;

static swap_slot_t bitmap_alloc(void)
{
    for (uint32_t i = 0; i < g_total_slots; i++) {
        if (!(g_bitmap[i / 8] & (1u << (i % 8)))) {
            g_bitmap[i / 8] |= (1u << (i % 8));
            g_stats.used_slots++;
            return (swap_slot_t)i;
        }
    }
    return (swap_slot_t)-1;   
}

static void bitmap_free(swap_slot_t slot)
{
    if (slot >= g_total_slots) return;
    if (g_bitmap[slot / 8] & (1u << (slot % 8))) {
        g_bitmap[slot / 8] &= ~(1u << (slot % 8));
        if (g_stats.used_slots) g_stats.used_slots--;
    }
}

static inline uint32_t slot_to_lba(swap_slot_t slot)
{
    return SWAP_DATA_START_LBA + slot * (PAGE_SIZE / 512);
}


//Public api
int swap_init(swap_read_fn read_fn, swap_write_fn write_fn, uint32_t slots)
{
    if (!read_fn || !write_fn) {
        kprint_color("[SWAP] no read/write callbacks — swap disabled\n", COLOR_LIGHT_RED);
        return -1;
    }

    g_read  = read_fn;
    g_write = write_fn;
    g_total_slots = (slots == 0 || slots > SWAP_MAX_SLOTS)
                    ? SWAP_MAX_SLOTS : slots;

    kprint("[SWAP] slots="); char buf[12]; itoa((int)g_total_slots, buf); kprint(buf);
    kprint("  space="); itoa((int)(g_total_slots * PAGE_SIZE / 1024 / 1024), buf); kprint(buf);
    kprint(" MB  bitmap="); itoa(SWAP_BITMAP_SIZE, buf); kprint(buf); kprint(" B\n");

    for (uint32_t i = 0; i < SWAP_BITMAP_SIZE; i++) g_bitmap[i] = 0;
    uint8_t* p = (uint8_t*)&g_stats;
    for (uint32_t i = 0; i < sizeof(g_stats); i++) p[i] = 0;
    g_stats.total_slots = g_total_slots;

    irq_spinlock_init(&g_swap_lock);
    g_enabled = 1;
    kprint("[SWAP] clock-hand eviction  start_lba=");
    { char buf[12]; itoa(SWAP_DATA_START_LBA, buf); kprint(buf); kprint("\n"); }
    klog(LOG_OK, "swap ready");
    return 0;
}

int swap_is_enabled(void) { return g_enabled; }

int swap_out_page(uint32_t phys_addr, swap_slot_t* out_slot)
{
    if (!g_enabled) return -1;
    if (phys_addr % PAGE_SIZE) return -1;

    irq_spinlock_acquire(&g_swap_lock);
    swap_slot_t slot = bitmap_alloc();
    irq_spinlock_release(&g_swap_lock);

    if (slot == (swap_slot_t)-1) {
        g_stats.swap_failures++;
        kprint("[SWAP] swap_out_page: no free slots!\n");
        return -1;
    }

    uint32_t lba = slot_to_lba(slot);
    int rc = g_write(lba, (const void*)phys_addr, PAGE_SIZE / 512);
    if (rc != 0) {
        irq_spinlock_acquire(&g_swap_lock);
        bitmap_free(slot);
        irq_spinlock_release(&g_swap_lock);
        g_stats.swap_failures++;
        return -1;
    }

    g_stats.pages_swapped_out++;
    *out_slot = slot;
    return 0;
}

int swap_in_page(swap_slot_t slot, uint32_t phys_addr)
{
    if (!g_enabled) return -1;
    if (slot >= g_total_slots) return -1;
    if (phys_addr % PAGE_SIZE) return -1;

    uint32_t lba = slot_to_lba(slot);
    int rc = g_read(lba, (void*)phys_addr, PAGE_SIZE / 512);
    if (rc != 0) {
        g_stats.swap_failures++;
        return -1;
    }

    irq_spinlock_acquire(&g_swap_lock);
    bitmap_free(slot);
    irq_spinlock_release(&g_swap_lock);

    g_stats.pages_swapped_in++;
    return 0;
}

void swap_free_slot(swap_slot_t slot)
{
    if (!g_enabled) return;
    irq_spinlock_acquire(&g_swap_lock);
    bitmap_free(slot);
    irq_spinlock_release(&g_swap_lock);
}

#define PTE_ACCESSED  0x20u

int swap_evict_page(uint32_t* pd)
{
    if (!g_enabled || !pd) return -1;

    uint32_t iterations = 0;
    uint32_t max_iter   = 2 * 1024 * 992; 

    uint32_t pdi = g_clock_pdi;
    uint32_t ptj = g_clock_ptj;

    while (iterations++ < max_iter) {
        if (pdi >= 1024) { pdi = 32; ptj = 0; }

        if (!(pd[pdi] & PAGE_PRESENT)) {
            pdi++; ptj = 0;
            continue;
        }

        uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
        uint32_t  pte = pt[ptj];

        if (!(pte & PAGE_PRESENT) || swap_pte_is_swapped(pte)) {
            goto next;
        }

        if (pte & PTE_ACCESSED) {
            pt[ptj] = pte & ~(uint32_t)PTE_ACCESSED;
            __asm__ __volatile__(
                "invlpg (%0)" ::
                "r"((pdi << 22) | (ptj << 12)) : "memory");
            goto next;
        }

        {
            uint32_t phys = pte & ~0xFFFu;
            swap_slot_t slot;

            if (swap_out_page(phys, &slot) != 0) return -1;

            pt[ptj] = swap_encode_pte(slot);

            pt[ptj] |= (pte & (PAGE_RW | PAGE_USER)) & ~PAGE_PRESENT;

            __asm__ __volatile__(
                "invlpg (%0)" ::
                "r"((pdi << 22) | (ptj << 12)) : "memory");

            kfree_page((void*)phys);

            ptj++;
            if (ptj >= 1024) { ptj = 0; pdi++; }
            g_clock_pdi = pdi;
            g_clock_ptj = ptj;
            return 0;
        }

    next:
        ptj++;
        if (ptj >= 1024) { ptj = 0; pdi++; }
    }

    kprint("[SWAP] evict: no evictable page found\n");
    return -1;
}

int swap_handle_fault(uint32_t* pd, uint32_t fault_addr)
{
    if (!pd) return -1;

    uint32_t page_va = fault_addr & ~0xFFFu;
    uint32_t pdi     = PD_INDEX(page_va);
    uint32_t pti     = PT_INDEX(page_va);

    if (!(pd[pdi] & PAGE_PRESENT)) return -1;

    uint32_t* pt  = (uint32_t*)(pd[pdi] & ~0xFFFu);
    uint32_t  pte = pt[pti];

    if (!swap_pte_is_swapped(pte)) return -1;

    swap_slot_t slot = swap_decode_pte(pte);

    void* phys = kalloc();
    if (!phys) {
        if (swap_evict_page(pd) != 0) return -1;
        phys = kalloc();
        if (!phys) return -1;
    }

    if (swap_in_page(slot, (uint32_t)phys) != 0) {
        kfree_page(phys);
        return -1;
    }

    uint32_t old_flags = pte & (PAGE_RW | PAGE_USER);
    pt[pti] = ((uint32_t)phys & ~0xFFFu) | old_flags | PAGE_PRESENT;

    __asm__ __volatile__("invlpg (%0)" :: "r"(page_va) : "memory");

    return 0;
}

swap_stats_t swap_get_stats(void) { return g_stats; }

void swap_print_stats(void)
{
    char buf[16];
    kprint("[SWAP] === Swap Statistics ===\n");
    kprint("  total_slots:       "); itoa((int)g_stats.total_slots,       buf); kprint(buf); kprint("\n");
    kprint("  used_slots:        "); itoa((int)g_stats.used_slots,        buf); kprint(buf); kprint("\n");
    kprint("  pages_swapped_out: "); itoa((int)g_stats.pages_swapped_out, buf); kprint(buf); kprint("\n");
    kprint("  pages_swapped_in:  "); itoa((int)g_stats.pages_swapped_in,  buf); kprint(buf); kprint("\n");
    kprint("  swap_failures:     "); itoa((int)g_stats.swap_failures,     buf); kprint(buf); kprint("\n");
}