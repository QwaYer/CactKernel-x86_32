//! Swap backing store: slot bitmap, clock hand, and C callbacks for block read/write.
//!
//! Encodes swapped-out PTEs with `PAGE_SWAPPED`; cooperates with the page-fault path.

use core::sync::atomic::{AtomicU32, Ordering};

use crate::ffi::*;
use crate::pmm::{kalloc, free_page};
use crate::safe::{KStatic, lock_acquire, lock_release, kprint_str, kprint_int, klog_msg, flush_tlb};

pub type SwapSlot = u32;
pub type SwapReadFn = Option<unsafe extern "C" fn(u32, *mut u8, u32) -> i32>;
pub type SwapWriteFn = Option<unsafe extern "C" fn(u32, *const u8, u32) -> i32>;

static G_READ: KStatic<SwapReadFn> = KStatic::new(None);
static G_WRITE: KStatic<SwapWriteFn> = KStatic::new(None);
static G_BITMAP: KStatic<[u8; SWAP_BITMAP_SIZE as usize]> = KStatic::new([0u8; SWAP_BITMAP_SIZE as usize]);
static G_TOTAL_SLOTS: KStatic<u32> = KStatic::new(0);
static G_ENABLED: KStatic<i32> = KStatic::new(0);
static G_STATS: KStatic<SwapStats> = KStatic::new(SwapStats {
    total_slots: 0,
    used_slots: 0,
    pages_swapped_out: 0,
    pages_swapped_in: 0,
    swap_failures: 0,
});
static G_SWAP_LOCK: KStatic<IrqSpinlock> = KStatic::new(IrqSpinlock { spin_locked: 0, saved_flags: 0 });
static G_CLOCK_PDI: KStatic<u32> = KStatic::new(32);
static G_CLOCK_PTJ: KStatic<u32> = KStatic::new(0);

fn bitmap_alloc() -> SwapSlot {
    let total = *G_TOTAL_SLOTS.get_mut();
    let bm = G_BITMAP.get_mut();
    let stats = G_STATS.get_mut();
    for i in 0..total {
        if bm[(i / 8) as usize] & (1u8 << (i % 8)) == 0 {
            bm[(i / 8) as usize] |= 1u8 << (i % 8);
            stats.used_slots += 1;
            return i;
        }
    }
    u32::MAX
}

fn bitmap_free(slot: SwapSlot) {
    let total = *G_TOTAL_SLOTS.get_mut();
    if slot >= total {
        return;
    }
    let bm = G_BITMAP.get_mut();
    let stats = G_STATS.get_mut();
    if bm[(slot / 8) as usize] & (1u8 << (slot % 8)) != 0 {
        bm[(slot / 8) as usize] &= !(1u8 << (slot % 8));
        if stats.used_slots > 0 {
            stats.used_slots -= 1;
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

pub fn swap_is_enabled() -> bool {
    *G_ENABLED.get_mut() != 0
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_init(
    read_fn: unsafe extern "C" fn(u32, *mut u8, u32) -> i32,
    write_fn: unsafe extern "C" fn(u32, *const u8, u32) -> i32,
    slots: u32,
) -> i32 {
    *G_READ.get_mut() = Some(read_fn);
    *G_WRITE.get_mut() = Some(write_fn);
    let total = if slots == 0 || slots > SWAP_MAX_SLOTS {
        SWAP_MAX_SLOTS
    } else {
        slots
    };
    *G_TOTAL_SLOTS.get_mut() = total;

    {
        let bm = G_BITMAP.get_mut();
        for i in 0..SWAP_BITMAP_SIZE as usize {
            bm[i] = 0;
        }
    }
    // SAFETY: zeroing stats at boot time.
    unsafe {
        core::ptr::write_bytes(G_STATS.as_ptr() as *mut u8, 0, core::mem::size_of::<SwapStats>());
    }
    G_STATS.get_mut().total_slots = total;

    // SAFETY: boot-time init.
    unsafe { irq_spinlock_init(G_SWAP_LOCK.as_ptr() as *mut IrqSpinlock) };
    *G_ENABLED.get_mut() = 1;
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_is_enabled_ffi() -> i32 {
    *G_ENABLED.get_mut()
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_out_page(phys_addr: u32, out_slot: *mut SwapSlot) -> i32 {
    if *G_ENABLED.get_mut() == 0 {
        return -1;
    }
    if phys_addr % PAGE_SIZE != 0 {
        return -1;
    }

    lock_acquire(G_SWAP_LOCK.as_ptr() as *mut IrqSpinlock);
    let slot = bitmap_alloc();
    lock_release(G_SWAP_LOCK.as_ptr() as *mut IrqSpinlock);

    if slot == u32::MAX {
        G_STATS.get_mut().swap_failures += 1;
        klog_msg(LOG_WARN, b"swap out failed: no free slots\0".as_ptr());
        return -1;
    }

    let lba = slot_to_lba(slot);
    let write_fn = *G_WRITE.get_mut();
    let write_fn = match write_fn {
        Some(f) => f,
        None => return -1,
    };
    // SAFETY: calling the C-provided write function.
    let rc = unsafe { write_fn(lba, phys_addr as *const u8, PAGE_SIZE / 512) };
    if rc != 0 {
        lock_acquire(G_SWAP_LOCK.as_ptr() as *mut IrqSpinlock);
        bitmap_free(slot);
        lock_release(G_SWAP_LOCK.as_ptr() as *mut IrqSpinlock);
        G_STATS.get_mut().swap_failures += 1;
        return -1;
    }

    G_STATS.get_mut().pages_swapped_out += 1;
    // SAFETY: out_slot is a valid pointer provided by the caller.
    unsafe { *out_slot = slot; }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_in_page(slot: SwapSlot, phys_addr: u32) -> i32 {
    if *G_ENABLED.get_mut() == 0 {
        return -1;
    }
    if slot >= *G_TOTAL_SLOTS.get_mut() {
        return -1;
    }
    if phys_addr % PAGE_SIZE != 0 {
        return -1;
    }

    let lba = slot_to_lba(slot);
    let read_fn = *G_READ.get_mut();
    let read_fn = match read_fn {
        Some(f) => f,
        None => return -1,
    };
    // SAFETY: calling the C-provided read function.
    let rc = unsafe { read_fn(lba, phys_addr as *mut u8, PAGE_SIZE / 512) };
    if rc != 0 {
        G_STATS.get_mut().swap_failures += 1;
        return -1;
    }

    lock_acquire(G_SWAP_LOCK.as_ptr() as *mut IrqSpinlock);
    bitmap_free(slot);
    lock_release(G_SWAP_LOCK.as_ptr() as *mut IrqSpinlock);

    G_STATS.get_mut().pages_swapped_in += 1;
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_free_slot(slot: SwapSlot) {
    if *G_ENABLED.get_mut() == 0 {
        return;
    }
    lock_acquire(G_SWAP_LOCK.as_ptr() as *mut IrqSpinlock);
    bitmap_free(slot);
    lock_release(G_SWAP_LOCK.as_ptr() as *mut IrqSpinlock);
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_evict_page(pd: *mut u32) -> i32 {
    if *G_ENABLED.get_mut() == 0 || pd.is_null() {
        return -1;
    }

    let mut iterations: u32 = 0;
    let max_iter: u32 = 2 * 1024 * 992;

    let mut pdi = *G_CLOCK_PDI.get_mut();
    let mut ptj = *G_CLOCK_PTJ.get_mut();

    while iterations < max_iter {
        iterations += 1;
        if pdi >= 1024 {
            pdi = 32;
            ptj = 0;
        }

        // SAFETY: pd is valid.
        let pde = unsafe { *pd.add(pdi as usize) };
        if pde & PAGE_PRESENT == 0 {
            pdi += 1;
            ptj = 0;
            continue;
        }

        if pde & PDE_PRIVATE == 0 {
            pdi += 1;
            ptj = 0;
            continue;
        }

        let pt_atomic = (pde & !0xFFF) as *mut AtomicU32;
        // SAFETY: pt_atomic is valid — page table mapped in virtual space.
        let pte = unsafe { (*pt_atomic.add(ptj as usize)).load(Ordering::Relaxed) };

        if pte & PAGE_PRESENT == 0 || swap_pte_is_swapped(pte) {
            ptj += 1;
            if ptj >= 1024 {
                ptj = 0;
                pdi += 1;
            }
            continue;
        }
        if pte & PAGE_USER == 0 {
            ptj += 1;
            if ptj >= 1024 {
                ptj = 0;
                pdi += 1;
            }
            continue;
        }

        if pte & PTE_ACCESSED != 0 {
            unsafe { (*pt_atomic.add(ptj as usize)).fetch_and(!PTE_ACCESSED, Ordering::AcqRel); }
            let vaddr = (pdi << 22) | (ptj << 12);
            flush_tlb(vaddr);
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
        unsafe { (*pt_atomic.add(ptj as usize)).store(new_pte, Ordering::Release); }

        let vaddr = (pdi << 22) | (ptj << 12);
        flush_tlb(vaddr);

        free_page(phys as *mut u8);

        ptj += 1;
        if ptj >= 1024 {
            ptj = 0;
            pdi += 1;
        }
        *G_CLOCK_PDI.get_mut() = pdi;
        *G_CLOCK_PTJ.get_mut() = ptj;
        return 0;
    }

    klog_msg(LOG_WARN, b"swap evict failed: no candidate page\0".as_ptr());
    -1
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_handle_fault(pd: *mut u32, fault_addr: u32) -> i32 {
    if pd.is_null() {
        return -1;
    }

    let page_va = fault_addr & !0xFFF;
    let pdi = pd_index(page_va) as usize;
    let pti = pt_index(page_va) as usize;

    // SAFETY: pd is valid.
    let pde = unsafe { *pd.add(pdi) };
    if pde & PAGE_PRESENT == 0 {
        return -1;
    }

    let pt = (pde & !0xFFF) as *mut AtomicU32;
    // SAFETY: pt is valid.
    let pte = unsafe { (*pt.add(pti)).load(Ordering::Acquire) };

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
        free_page(phys);
        return -1;
    }

    let old_flags = pte & (PAGE_RW | PAGE_USER);
    // SAFETY: writing the restored PTE.
    unsafe { (*pt.add(pti)).store((phys as u32 & !0xFFF) | old_flags | PAGE_PRESENT, Ordering::Release); }
    flush_tlb(page_va);
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_get_stats() -> SwapStats {
    *G_STATS.get_mut()
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_print_stats() {
    let stats = *G_STATS.get_mut();
    kprint_str(b"[SWAP] === Swap Statistics ===\n\0".as_ptr());
    kprint_str(b"  total_slots:       \0".as_ptr());
    kprint_int(stats.total_slots as i32);
    kprint_str(b"\n\0".as_ptr());
    kprint_str(b"  used_slots:        \0".as_ptr());
    kprint_int(stats.used_slots as i32);
    kprint_str(b"\n\0".as_ptr());
    kprint_str(b"  pages_swapped_out: \0".as_ptr());
    kprint_int(stats.pages_swapped_out as i32);
    kprint_str(b"\n\0".as_ptr());
    kprint_str(b"  pages_swapped_in:  \0".as_ptr());
    kprint_int(stats.pages_swapped_in as i32);
    kprint_str(b"\n\0".as_ptr());
    kprint_str(b"  swap_failures:     \0".as_ptr());
    kprint_int(stats.swap_failures as i32);
    kprint_str(b"\n\0".as_ptr());
}
