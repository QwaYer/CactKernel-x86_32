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
    // SAFETY: `G_TOTAL_SLOTS` is written once by `swap_init` at boot; `bitmap_alloc`
    // itself is only called while holding `G_SWAP_LOCK`.
    let total = *unsafe { KStatic::get_mut(G_TOTAL_SLOTS.as_ptr()) };
    // SAFETY: `G_BITMAP` slot bitmap, mutated under `G_SWAP_LOCK` — held by every
    // caller of `bitmap_alloc`.
    let bm = unsafe { KStatic::get_mut(G_BITMAP.as_ptr()) };
    // SAFETY: `G_STATS.used_slots` alongside the bitmap update, under `G_SWAP_LOCK`.
    let stats = unsafe { KStatic::get_mut(G_STATS.as_ptr()) };
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
    // SAFETY: `G_TOTAL_SLOTS` read; `bitmap_free` callers hold `G_SWAP_LOCK`.
    let total = *unsafe { KStatic::get_mut(G_TOTAL_SLOTS.as_ptr()) };
    if slot >= total {
        return;
    }
    // SAFETY: `G_BITMAP` mutated under `G_SWAP_LOCK`.
    let bm = unsafe { KStatic::get_mut(G_BITMAP.as_ptr()) };
    // SAFETY: `G_STATS.used_slots` decremented with the bitmap, under `G_SWAP_LOCK`.
    let stats = unsafe { KStatic::get_mut(G_STATS.as_ptr()) };
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
    // SAFETY: `G_ENABLED` is written once by `swap_init` during boot and only read
    // afterwards, so it is effectively immutable.
    *unsafe { KStatic::get_mut(G_ENABLED.as_ptr()) } != 0
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_init(
    read_fn: unsafe extern "C" fn(u32, *mut u8, u32) -> i32,
    write_fn: unsafe extern "C" fn(u32, *const u8, u32) -> i32,
    slots: u32,
) -> i32 {
    // SAFETY: boot-time `swap_init`: single-threaded, no other swap user yet.
    *unsafe { KStatic::get_mut(G_READ.as_ptr()) } = Some(read_fn);
    // SAFETY: same single-threaded boot init of `G_WRITE`.
    *unsafe { KStatic::get_mut(G_WRITE.as_ptr()) } = Some(write_fn);
    let total = if slots == 0 || slots > SWAP_MAX_SLOTS {
        SWAP_MAX_SLOTS
    } else {
        slots
    };
    // SAFETY: `G_TOTAL_SLOTS` set during single-threaded boot init.
    *unsafe { KStatic::get_mut(G_TOTAL_SLOTS.as_ptr()) } = total;

    {
        // SAFETY: zeroing the `G_BITMAP` slot bitmap during single-threaded boot init.
        let bm = unsafe { KStatic::get_mut(G_BITMAP.as_ptr()) };
        bm.fill(0);
    }
    // SAFETY: zeroing stats at boot time.
    unsafe {
        core::ptr::write_bytes(G_STATS.as_ptr() as *mut u8, 0, core::mem::size_of::<SwapStats>());
    }
    // SAFETY: initialising the slot count in `G_STATS` during boot.
    (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).total_slots = total;

    // SAFETY: boot-time init.
    unsafe { irq_spinlock_init(G_SWAP_LOCK.as_ptr()) };
    // SAFETY: enabling swap; set last, so no concurrent reader can see `G_ENABLED`
    // before the tables above are initialised.
    *unsafe { KStatic::get_mut(G_ENABLED.as_ptr()) } = 1;
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_is_enabled_ffi() -> i32 {
    // SAFETY: `G_ENABLED` is immutable after `swap_init`.
    *unsafe { KStatic::get_mut(G_ENABLED.as_ptr()) }
}

/// # Safety
///
/// `out_slot` must be null or point to a writable `SwapSlot` valid for the
/// call, and `phys_addr` must be a page-aligned physical frame address.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swap_out_page(phys_addr: u32, out_slot: *mut SwapSlot) -> i32 {
    // SAFETY: `G_ENABLED` is immutable after `swap_init`.
    if *unsafe { KStatic::get_mut(G_ENABLED.as_ptr()) } == 0 {
        return -1;
    }
    if !phys_addr.is_multiple_of(PAGE_SIZE) {
        return -1;
    }

    lock_acquire(G_SWAP_LOCK.as_ptr());
    let slot = bitmap_alloc();
    lock_release(G_SWAP_LOCK.as_ptr());

    if slot == u32::MAX {
        // SAFETY: `G_STATS.swap_failures` — a diagnostic counter; this increment is not
        // taken under `G_SWAP_LOCK` and is not serialised across CPUs, but a lost count
        // is harmless.
        (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).swap_failures += 1;
        klog_msg(LOG_WARN, c"swap out failed: no free slots".as_ptr() as *const u8);
        return -1;
    }

    let lba = slot_to_lba(slot);
    // SAFETY: `G_WRITE` callback pointer is set once at boot and left unchanged.
    let write_fn = *unsafe { KStatic::get_mut(G_WRITE.as_ptr()) };
    let write_fn = match write_fn {
        Some(f) => f,
        None => return -1,
    };
    // SAFETY: calling the C-provided write function.
    let rc = unsafe { write_fn(lba, phys_addr as *const u8, PAGE_SIZE / 512) };
    if rc != 0 {
        lock_acquire(G_SWAP_LOCK.as_ptr());
        bitmap_free(slot);
        lock_release(G_SWAP_LOCK.as_ptr());
        // SAFETY: `G_STATS.swap_failures` — unlocked diagnostic counter, see above.
        (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).swap_failures += 1;
        return -1;
    }

    // SAFETY: `G_STATS.pages_swapped_out` — unlocked diagnostic counter, see above.
    (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).pages_swapped_out += 1;
    // SAFETY: out_slot is a valid pointer provided by the caller.
    unsafe { *out_slot = slot; }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_in_page(slot: SwapSlot, phys_addr: u32) -> i32 {
    // SAFETY: `G_ENABLED` immutable after boot init.
    if *unsafe { KStatic::get_mut(G_ENABLED.as_ptr()) } == 0 {
        return -1;
    }
    // SAFETY: `G_TOTAL_SLOTS` immutable after boot init; slotted reads are validated
    // against it.
    if slot >= *unsafe { KStatic::get_mut(G_TOTAL_SLOTS.as_ptr()) } {
        return -1;
    }
    if !phys_addr.is_multiple_of(PAGE_SIZE) {
        return -1;
    }

    let lba = slot_to_lba(slot);
    // SAFETY: `G_READ` callback pointer is set once at boot and left unchanged.
    let read_fn = *unsafe { KStatic::get_mut(G_READ.as_ptr()) };
    let read_fn = match read_fn {
        Some(f) => f,
        None => return -1,
    };
    // SAFETY: calling the C-provided read function.
    let rc = unsafe { read_fn(lba, phys_addr as *mut u8, PAGE_SIZE / 512) };
    if rc != 0 {
        // SAFETY: `G_STATS.swap_failures` — unlocked diagnostic counter, see above.
        (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).swap_failures += 1;
        return -1;
    }

    lock_acquire(G_SWAP_LOCK.as_ptr());
    bitmap_free(slot);
    lock_release(G_SWAP_LOCK.as_ptr());

    // SAFETY: `G_STATS.pages_swapped_in` — unlocked diagnostic counter, see above.
    (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).pages_swapped_in += 1;
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_free_slot(slot: SwapSlot) {
    // SAFETY: `G_ENABLED` immutable after boot init.
    if *unsafe { KStatic::get_mut(G_ENABLED.as_ptr()) } == 0 {
        return;
    }
    lock_acquire(G_SWAP_LOCK.as_ptr());
    bitmap_free(slot);
    lock_release(G_SWAP_LOCK.as_ptr());
}

/// # Safety
///
/// `pd` must be null or a valid page directory whose user PTEs the caller is
/// allowed to mutate; the caller must not race another page-table walker.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swap_evict_page(pd: *mut u32) -> i32 {
    // SAFETY: `G_ENABLED` immutable after boot init.
    if *unsafe { KStatic::get_mut(G_ENABLED.as_ptr()) } == 0 || pd.is_null() {
        return -1;
    }

    let mut iterations: u32 = 0;
    let max_iter: u32 = 2 * 1024 * 992;

    // SAFETY: `G_CLOCK_PDI` is the clock-hand page-directory index, only advanced by
    // `swap_evict_page`; a torn read across CPUs merely restarts the scan earlier.
    let mut pdi = *unsafe { KStatic::get_mut(G_CLOCK_PDI.as_ptr()) };
    // SAFETY: `G_CLOCK_PTJ`, the clock hand's page-table index, under the same
    // caveat as `G_CLOCK_PDI`: it is a heuristic scan position, not shared state.
    let mut ptj = *unsafe { KStatic::get_mut(G_CLOCK_PTJ.as_ptr()) };

    while iterations < max_iter {
        iterations += 1;
        if pdi >= 1024 {
            pdi = 32;
            ptj = 0;
        }

        // SAFETY: `pd` is valid and `pdi < 1024`, so this PD entry pointer is in
        // bounds.
        let pde_entry = unsafe { pd.add(pdi as usize) };
        // SAFETY: `pde_entry` points at one initialised PD entry.
        let pde = unsafe { *pde_entry };
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
        // SAFETY: `pt_atomic` is the live page table named by the present private
        // PDE and `ptj < 1024`, so this entry pointer is in bounds.
        let pte_entry = unsafe { pt_atomic.add(ptj as usize) };
        // SAFETY: `pte_entry` points at one live atomic PTE; a relaxed load is a
        // single atomic access, so no torn value can be observed.
        let pte = unsafe { (*pte_entry).load(Ordering::Relaxed) };

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
            // SAFETY: `pt_atomic` is the page table referenced by the present,
            // private PDE just read from `pd` and `ptj < 1024`, so this entry
            // pointer is in bounds.
            let access_entry = unsafe { pt_atomic.add(ptj as usize) };
            // SAFETY: `access_entry` points at one live atomic PTE; the fetch_and is
            // a single atomic RMW, so a concurrent accessor cannot observe a torn
            // value.
            unsafe { (*access_entry).fetch_and(!PTE_ACCESSED, Ordering::AcqRel) };
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
        // SAFETY: `phys` is the page-aligned frame selected by the clock scan and
        // `slot` is a live local receiving the assigned swap slot.
        if unsafe { swap_out_page(phys, &mut slot) } != 0 {
            return -1;
        }

        let mut new_pte = swap_encode_pte(slot);
        new_pte |= (pte & (PAGE_RW | PAGE_USER)) & !PAGE_PRESENT;
        // SAFETY: `pt_atomic` is the same live page table addressed above and
        // `ptj < 1024`, so this entry pointer is in bounds.
        let publish_entry = unsafe { pt_atomic.add(ptj as usize) };
        // SAFETY: `publish_entry` points at one live atomic PTE; the store is a
        // single atomic access, publishing the swapped-out PTE.
        unsafe { (*publish_entry).store(new_pte, Ordering::Release) };

        let vaddr = (pdi << 22) | (ptj << 12);
        flush_tlb(vaddr);

        // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
        unsafe { free_page(phys as *mut u8) };

        ptj += 1;
        if ptj >= 1024 {
            ptj = 0;
            pdi += 1;
        }
        // SAFETY: saving the clock hand after a successful eviction; best-effort scan
        // position shared between CPUs, see above.
        *unsafe { KStatic::get_mut(G_CLOCK_PDI.as_ptr()) } = pdi;
        // SAFETY: saving the clock hand's PT index; best-effort scan position.
        *unsafe { KStatic::get_mut(G_CLOCK_PTJ.as_ptr()) } = ptj;
        return 0;
    }

    klog_msg(LOG_WARN, c"swap evict failed: no candidate page".as_ptr() as *const u8);
    -1
}

/// # Safety
///
/// `pd` must be null or a valid page directory whose user PTEs the caller is
/// allowed to mutate; the caller must not race another page-table walker.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn swap_handle_fault(pd: *mut u32, fault_addr: u32) -> i32 {
    if pd.is_null() {
        return -1;
    }

    let page_va = fault_addr & !0xFFF;
    let pdi = pd_index(page_va) as usize;
    let pti = pt_index(page_va) as usize;

    // SAFETY: `pd` is valid and `pdi` came from `pd_index`, so this PD entry
    // pointer is in bounds.
    let pde_entry = unsafe { pd.add(pdi) };
    // SAFETY: `pde_entry` points at one initialised PD entry.
    let pde = unsafe { *pde_entry };
    if pde & PAGE_PRESENT == 0 {
        return -1;
    }

    let pt = (pde & !0xFFF) as *mut AtomicU32;
    // SAFETY: `pt` is the live page table named by the present PDE and `pti` came
    // from `pt_index`, so this entry pointer is in bounds.
    let pte_entry = unsafe { pt.add(pti) };
    // SAFETY: `pte_entry` points at one live atomic PTE; an acquire load is a
    // single atomic access.
    let pte = unsafe { (*pte_entry).load(Ordering::Acquire) };

    if !swap_pte_is_swapped(pte) {
        return -1;
    }

    let slot = swap_decode_pte(pte);

    let mut phys = kalloc();
    if phys.is_null() {
        // SAFETY: `pd` is a valid page directory (checked at entry); eviction only
        // touches that directory's own user PTEs.
        if unsafe { swap_evict_page(pd) } != 0 {
            return -1;
        }
        phys = kalloc();
        if phys.is_null() {
            return -1;
        }
    }

    if swap_in_page(slot, phys as u32) != 0 {
        // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
        unsafe { free_page(phys) };
        return -1;
    }

    let old_flags = pte & (PAGE_RW | PAGE_USER);
    // SAFETY: `pt` is the live page table named by the present PDE and `pti` came
    // from `pt_index`, so this entry pointer is in bounds.
    let restore_entry = unsafe { pt.add(pti) };
    // SAFETY: `restore_entry` points at one live atomic PTE; the store publishes
    // the restored mapping.
    unsafe { (*restore_entry).store((phys as u32 & !0xFFF) | old_flags | PAGE_PRESENT, Ordering::Release) };
    flush_tlb(page_va);
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_get_stats() -> SwapStats {
    // SAFETY: read-only snapshot of the swap counters for the C caller.
    *unsafe { KStatic::get_mut(G_STATS.as_ptr()) }
}

#[unsafe(no_mangle)]
pub extern "C" fn swap_print_stats() {
    // SAFETY: read-only snapshot of the swap counters for printing.
    let stats = *unsafe { KStatic::get_mut(G_STATS.as_ptr()) };
    kprint_str(c"[SWAP] === Swap Statistics ===\n".as_ptr() as *const u8);
    kprint_str(c"  total_slots:       ".as_ptr() as *const u8);
    kprint_int(stats.total_slots as i32);
    kprint_str(c"\n".as_ptr() as *const u8);
    kprint_str(c"  used_slots:        ".as_ptr() as *const u8);
    kprint_int(stats.used_slots as i32);
    kprint_str(c"\n".as_ptr() as *const u8);
    kprint_str(c"  pages_swapped_out: ".as_ptr() as *const u8);
    kprint_int(stats.pages_swapped_out as i32);
    kprint_str(c"\n".as_ptr() as *const u8);
    kprint_str(c"  pages_swapped_in:  ".as_ptr() as *const u8);
    kprint_int(stats.pages_swapped_in as i32);
    kprint_str(c"\n".as_ptr() as *const u8);
    kprint_str(c"  swap_failures:     ".as_ptr() as *const u8);
    kprint_int(stats.swap_failures as i32);
    kprint_str(c"\n".as_ptr() as *const u8);
}
