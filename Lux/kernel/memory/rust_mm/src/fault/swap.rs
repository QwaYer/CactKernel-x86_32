use crate::ffi::*;
use crate::pmm::{kalloc, kfree_page};

pub type SwapSlot = u32;
pub type SwapReadFn = Option<unsafe extern "C" fn(u32, *mut u8, u32) -> i32>;
pub type SwapWriteFn = Option<unsafe extern "C" fn(u32, *const u8, u32) -> i32>;

static mut G_READ: SwapReadFn = None;
static mut G_WRITE: SwapWriteFn = None;
static mut G_BITMAP: [u8; SWAP_BITMAP_SIZE as usize] = [0u8; SWAP_BITMAP_SIZE as usize];
static mut G_TOTAL_SLOTS: u32 = 0;
static mut G_ENABLED: i32 = 0;
static mut G_STATS: SwapStats = SwapStats {
    total_slots: 0,
    used_slots: 0,
    pages_swapped_out: 0,
    pages_swapped_in: 0,
    swap_failures: 0,
};
static mut G_SWAP_LOCK: IrqSpinlock = IrqSpinlock { spin_locked: 0, saved_flags: 0 };
static mut G_CLOCK_PDI: u32 = 32;
static mut G_CLOCK_PTJ: u32 = 0;

unsafe fn bitmap_alloc() -> SwapSlot {
    for i in 0..G_TOTAL_SLOTS {
        if G_BITMAP[(i / 8) as usize] & (1u8 << (i % 8)) == 0 {
            G_BITMAP[(i / 8) as usize] |= 1u8 << (i % 8);
            G_STATS.used_slots += 1;
            return i;
        }
    }
    u32::MAX
}

unsafe fn bitmap_free(slot: SwapSlot) {
    if slot >= G_TOTAL_SLOTS {
        return;
    }
    if G_BITMAP[(slot / 8) as usize] & (1u8 << (slot % 8)) != 0 {
        G_BITMAP[(slot / 8) as usize] &= !(1u8 << (slot % 8));
        if G_STATS.used_slots > 0 {
            G_STATS.used_slots -= 1;
        }
    }
}

fn slot_to_lba(slot: SwapSlot) -> u32 {
    SWAP_DATA_START_LBA + slot * (PAGE_SIZE / 512)
}

fn swap_encode_pte(slot: SwapSlot) -> u32 {
    ((slot << 12) & 0xFFFFF000) | PAGE_SWAPPED
}

fn swap_decode_pte(pte: u32) -> SwapSlot {
    (pte & 0xFFFFF000) >> 12
}

pub fn swap_pte_is_swapped(pte: u32) -> bool {
    (pte & PAGE_PRESENT == 0) && (pte & PAGE_SWAPPED != 0)
}

pub unsafe fn swap_is_enabled_internal() -> bool {
    G_ENABLED != 0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swap_init(
    read_fn: unsafe extern "C" fn(u32, *mut u8, u32) -> i32,
    write_fn: unsafe extern "C" fn(u32, *const u8, u32) -> i32,
    slots: u32,
) -> i32 {
    G_READ = Some(read_fn);
    G_WRITE = Some(write_fn);
    G_TOTAL_SLOTS = if slots == 0 || slots > SWAP_MAX_SLOTS {
        SWAP_MAX_SLOTS
    } else {
        slots
    };

    let mut buf = [0u8; 12];
    kprint(b"[SWAP] slots=\0".as_ptr());
    itoa(G_TOTAL_SLOTS as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"  space=\0".as_ptr());
    itoa((G_TOTAL_SLOTS * PAGE_SIZE / 1024 / 1024) as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b" MB  bitmap=\0".as_ptr());
    itoa(SWAP_BITMAP_SIZE as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b" B\n\0".as_ptr());

    for i in 0..SWAP_BITMAP_SIZE as usize {
        G_BITMAP[i] = 0;
    }
    core::ptr::write_bytes(&raw mut G_STATS as *mut u8, 0, core::mem::size_of::<SwapStats>());
    G_STATS.total_slots = G_TOTAL_SLOTS;

    irq_spinlock_init(&raw mut G_SWAP_LOCK);
    G_ENABLED = 1;
    kprint(b"[SWAP] clock-hand eviction  start_lba=\0".as_ptr());
    itoa(SWAP_DATA_START_LBA as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"\n\0".as_ptr());
    klog(LOG_OK, b"swap ready\0".as_ptr());
    0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swap_is_enabled() -> i32 {
    G_ENABLED
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swap_out_page(phys_addr: u32, out_slot: *mut SwapSlot) -> i32 {
    if G_ENABLED == 0 {
        return -1;
    }
    if phys_addr % PAGE_SIZE != 0 {
        return -1;
    }

    irq_spinlock_acquire(&raw mut G_SWAP_LOCK);
    let slot = bitmap_alloc();
    irq_spinlock_release(&raw mut G_SWAP_LOCK);

    if slot == u32::MAX {
        G_STATS.swap_failures += 1;
        kprint(b"[SWAP] swap_out_page: no free slots!\n\0".as_ptr());
        return -1;
    }

    let lba = slot_to_lba(slot);
    let write_fn = G_WRITE.unwrap();
    let rc = write_fn(lba, phys_addr as *const u8, PAGE_SIZE / 512);
    if rc != 0 {
        irq_spinlock_acquire(&raw mut G_SWAP_LOCK);
        bitmap_free(slot);
        irq_spinlock_release(&raw mut G_SWAP_LOCK);
        G_STATS.swap_failures += 1;
        return -1;
    }

    G_STATS.pages_swapped_out += 1;
    *out_slot = slot;
    0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swap_in_page(slot: SwapSlot, phys_addr: u32) -> i32 {
    if G_ENABLED == 0 {
        return -1;
    }
    if slot >= G_TOTAL_SLOTS {
        return -1;
    }
    if phys_addr % PAGE_SIZE != 0 {
        return -1;
    }

    let lba = slot_to_lba(slot);
    let read_fn = G_READ.unwrap();
    let rc = read_fn(lba, phys_addr as *mut u8, PAGE_SIZE / 512);
    if rc != 0 {
        G_STATS.swap_failures += 1;
        return -1;
    }

    irq_spinlock_acquire(&raw mut G_SWAP_LOCK);
    bitmap_free(slot);
    irq_spinlock_release(&raw mut G_SWAP_LOCK);

    G_STATS.pages_swapped_in += 1;
    0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swap_free_slot(slot: SwapSlot) {
    if G_ENABLED == 0 {
        return;
    }
    irq_spinlock_acquire(&raw mut G_SWAP_LOCK);
    bitmap_free(slot);
    irq_spinlock_release(&raw mut G_SWAP_LOCK);
}

pub unsafe fn swap_evict_page_internal(pd: *mut u32) -> i32 {
    swap_evict_page(pd)
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swap_evict_page(pd: *mut u32) -> i32 {
    if G_ENABLED == 0 || pd.is_null() {
        return -1;
    }

    let mut iterations: u32 = 0;
    let max_iter: u32 = 2 * 1024 * 992;

    let mut pdi = G_CLOCK_PDI;
    let mut ptj = G_CLOCK_PTJ;

    while iterations < max_iter {
        iterations += 1;
        if pdi >= 1024 {
            pdi = 32;
            ptj = 0;
        }

        if *pd.add(pdi as usize) & PAGE_PRESENT == 0 {
            pdi += 1;
            ptj = 0;
            continue;
        }

        let pt = (*pd.add(pdi as usize) & !0xFFF) as *mut u32;
        let pte = *pt.add(ptj as usize);

        if pte & PAGE_PRESENT == 0 || swap_pte_is_swapped(pte) {
            ptj += 1;
            if ptj >= 1024 {
                ptj = 0;
                pdi += 1;
            }
            continue;
        }

        if pte & PTE_ACCESSED != 0 {
            *pt.add(ptj as usize) = pte & !PTE_ACCESSED;
            let vaddr = (pdi << 22) | (ptj << 12);
            tlb_flush(vaddr);
            ptj += 1;
            if ptj >= 1024 {
                ptj = 0;
                pdi += 1;
            }
            continue;
        }

        let phys = pte & !0xFFF;
        let mut slot: SwapSlot = 0;
        if swap_out_page(phys, &mut slot) != 0 {
            return -1;
        }

        let mut new_pte = swap_encode_pte(slot);
        new_pte |= (pte & (PAGE_RW | PAGE_USER)) & !PAGE_PRESENT;
        *pt.add(ptj as usize) = new_pte;

        let vaddr = (pdi << 22) | (ptj << 12);
        tlb_flush(vaddr);

        kfree_page(phys as *mut u8);

        ptj += 1;
        if ptj >= 1024 {
            ptj = 0;
            pdi += 1;
        }
        G_CLOCK_PDI = pdi;
        G_CLOCK_PTJ = ptj;
        return 0;
    }

    kprint(b"[SWAP] evict: no evictable page found\n\0".as_ptr());
    -1
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swap_handle_fault(pd: *mut u32, fault_addr: u32) -> i32 {
    if pd.is_null() {
        return -1;
    }

    let page_va = fault_addr & !0xFFF;
    let pdi = pd_index(page_va) as usize;
    let pti = pt_index(page_va) as usize;

    if *pd.add(pdi) & PAGE_PRESENT == 0 {
        return -1;
    }

    let pt = (*pd.add(pdi) & !0xFFF) as *mut u32;
    let pte = *pt.add(pti);

    if !swap_pte_is_swapped(pte) {
        return -1;
    }

    let slot = swap_decode_pte(pte);

    let mut phys = kalloc();
    if phys.is_null() {
        if swap_evict_page(pd) != 0 {
            return -1;
        }
        phys = kalloc();
        if phys.is_null() {
            return -1;
        }
    }

    if swap_in_page(slot, phys as u32) != 0 {
        kfree_page(phys);
        return -1;
    }

    let old_flags = pte & (PAGE_RW | PAGE_USER);
    *pt.add(pti) = (phys as u32 & !0xFFF) | old_flags | PAGE_PRESENT;
    tlb_flush(page_va);
    0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swap_get_stats() -> SwapStats {
    G_STATS
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swap_print_stats() {
    let mut buf = [0u8; 16];
    kprint(b"[SWAP] === Swap Statistics ===\n\0".as_ptr());
    kprint(b"  total_slots:       \0".as_ptr());
    itoa(G_STATS.total_slots as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"\n\0".as_ptr());
    kprint(b"  used_slots:        \0".as_ptr());
    itoa(G_STATS.used_slots as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"\n\0".as_ptr());
    kprint(b"  pages_swapped_out: \0".as_ptr());
    itoa(G_STATS.pages_swapped_out as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"\n\0".as_ptr());
    kprint(b"  pages_swapped_in:  \0".as_ptr());
    itoa(G_STATS.pages_swapped_in as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"\n\0".as_ptr());
    kprint(b"  swap_failures:     \0".as_ptr());
    itoa(G_STATS.swap_failures as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"\n\0".as_ptr());
}