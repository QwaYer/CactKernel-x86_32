use crate::ffi::*;
use crate::safe::{KStatic, lock_acquire, lock_release,
                  kprint_str, kprint_int, kprint_hex, klog_msg};

static MEMORY_BITMAP: KStatic<[u8; BITMAP_SIZE as usize]> =
    KStatic::new([0u8; BITMAP_SIZE as usize]);

static PAGE_REFCOUNTS: KStatic<[u16; TOTAL_PAGES as usize]> =
    KStatic::new([0u16; TOTAL_PAGES as usize]);

static FIRST_AVAILABLE_PAGE: KStatic<u32> = KStatic::new(0);

pub(crate) static PAGE_LOCK: KStatic<IrqSpinlock> =
    KStatic::new(IrqSpinlock { spin_locked: 0, saved_flags: 0 });

static MMAP_TABLE: KStatic<Mb2MmapTable> = KStatic::new(Mb2MmapTable {
    entries: [Mb2MmapFlat { base: 0, len: 0, ty: 0 }; MB2_MMAP_MAX_ENTRIES],
    count: 0,
});

#[inline(always)]
fn bitmap_set(idx: u32) {
    let bm = MEMORY_BITMAP.get_mut();
    bm[(idx / 8) as usize] |= 1 << (idx % 8);
}

#[inline(always)]
fn bitmap_clear(idx: u32) {
    let bm = MEMORY_BITMAP.get_mut();
    bm[(idx / 8) as usize] &= !(1 << (idx % 8));
}

#[inline(always)]
fn bitmap_test(idx: u32) -> bool {
    let bm = MEMORY_BITMAP.get_mut();
    bm[(idx / 8) as usize] & (1 << (idx % 8)) != 0
}

#[inline(always)]
fn addr_to_page(addr: u32) -> u32 {
    addr / PAGE_SIZE
}

/// Page index → physical address.
#[inline(always)]
fn page_to_addr(idx: u32) -> u32 {
    idx * PAGE_SIZE
}


//Public api
#[unsafe(no_mangle)]
pub extern "C" fn pmm_init_from_mmap(mmap: *const Mb2MmapTable) {
    if mmap.is_null() {
        kprint_str(b"[PMM] pmm_init_from_mmap: NULL pointer - assuming no MMAP\n\0".as_ptr());
        return;
    }
    // SAFETY: pointer is valid C-side static storage.
    let src = unsafe { &*mmap };
    let count = (src.count as usize).min(MB2_MMAP_MAX_ENTRIES);
    let dst = MMAP_TABLE.get_mut();
    dst.count = count as u32;
    for i in 0..count {
        let mut e = src.entries[i];
        if e.len != 0 {
            let end = e.base.saturating_add(e.len);
            if e.base >= PCI_HOLE_START {
                e.len = 0;
            } else if end > PCI_HOLE_START {
                e.len = PCI_HOLE_START - e.base;
            }
        }
        dst.entries[i] = e;
    }
    kprint_str(b"[PMM] mmap_table copied: \0".as_ptr());
    kprint_int(count as i32);
    kprint_str(b" entries\n\0".as_ptr());
}

#[unsafe(no_mangle)]
pub extern "C" fn init_memory_manager() {
    kprint_str(b"[PMM] bitmap at 0x\0".as_ptr());
    kprint_hex(MEMORY_BITMAP.as_ptr() as u32);
    kprint_str(b"  size=\0".as_ptr());
    kprint_int(BITMAP_SIZE as i32);
    kprint_str(b" B  total_pages=\0".as_ptr());
    kprint_int(TOTAL_PAGES as i32);
    kprint_str(b"  managed_range=0..0x\0".as_ptr());
    kprint_hex(PCI_HOLE_START);
    kprint_str(b"\n\0".as_ptr());

    unsafe { irq_spinlock_init(PAGE_LOCK.as_ptr() as *mut IrqSpinlock) };
    {
        let bm = MEMORY_BITMAP.get_mut();
        for b in bm.iter_mut() {
            *b = 0xFF;
        }
    }

    let mmap = MMAP_TABLE.get_mut();
    let have_mmap = mmap.count > 0;

    if have_mmap {
        kprint_str(b"[PMM] applying hardware MMAP (\0".as_ptr());
        kprint_int(mmap.count as i32);
        kprint_str(b" entries):\n\0".as_ptr());

        for i in 0..mmap.count as usize {
            let e = mmap.entries[i];

            kprint_str(b"  [MMAP] base=0x\0".as_ptr());
            kprint_hex(e.base);
            kprint_str(b" len=0x\0".as_ptr());
            kprint_hex(e.len);
            kprint_str(b" type=\0".as_ptr());
            kprint_int(e.ty as i32);
            kprint_str(b"\n\0".as_ptr());

            if e.ty != MB2_MMAP_TYPE_AVAILABLE { continue; }
            if e.len == 0 { continue; }

            let region_end = e.base.saturating_add(e.len);
            let clip_end   = region_end.min(PCI_HOLE_START);
            if e.base >= clip_end { continue; }

            let first_page = addr_to_page((e.base + PAGE_SIZE - 1) & !(PAGE_SIZE - 1));
            let last_page  = addr_to_page(clip_end & !(PAGE_SIZE - 1));
            if first_page >= last_page { continue; }

            for pg in first_page..last_page {
                if pg < TOTAL_PAGES {
                    bitmap_clear(pg);
                }
            }
        }
    } else {
        kprint_str(b"[PMM] no MMAP - assuming all RAM above 16 MB is available\n\0".as_ptr());
        let first_free = addr_to_page(RESERVED_END);
        for pg in first_free..TOTAL_PAGES {
            bitmap_clear(pg);
        }
    }

    let reserved_pages = addr_to_page(RESERVED_END); // = 32 MB / 4096 = 8192
    kprint_str(b"[PMM] hard-reserving pages 0..\0".as_ptr());
    kprint_int(reserved_pages as i32);
    kprint_str(b" (0x00000000..0x\0".as_ptr());
    kprint_hex(RESERVED_END);
    kprint_str(b") - BIOS + kernel + tables + heap\n\0".as_ptr());

    for pg in 0..reserved_pages {
        bitmap_set(pg);
    }

    // Keep the dedicated heap window out of PMM allocations even when
    // RESERVED_END == HEAP_START (C/Rust layout sync for 4 GB mode).
    let heap_start_page = addr_to_page(HEAP_START);
    let heap_end_addr = HEAP_START.saturating_add(HEAP_SIZE).min(PCI_HOLE_START);
    let heap_end_page = addr_to_page(heap_end_addr);
    if heap_start_page < heap_end_page {
        kprint_str(b"[PMM] reserving heap pages \0".as_ptr());
        kprint_int(heap_start_page as i32);
        kprint_str(b"..\0".as_ptr());
        kprint_int((heap_end_page - 1) as i32);
        kprint_str(b" (0x\0".as_ptr());
        kprint_hex(HEAP_START);
        kprint_str(b"..0x\0".as_ptr());
        kprint_hex(heap_end_addr);
        kprint_str(b")\n\0".as_ptr());
        for pg in heap_start_page..heap_end_page {
            bitmap_set(pg);
        }
    }

    // Initialise the first-fit search hint.
    let initial_hint = if reserved_pages > heap_start_page {
        reserved_pages
    } else {
        heap_end_page
    };
    *FIRST_AVAILABLE_PAGE.get_mut() = initial_hint;

    let mut free_count: u32 = 0;
    {
        let bm = MEMORY_BITMAP.get_mut();
        for pg in 0..TOTAL_PAGES as usize {
            if bm[pg / 8] & (1 << (pg % 8)) == 0 {
                free_count += 1;
            }
        }
    }

    kprint_str(b"[PMM] free pages: \0".as_ptr());
    kprint_int(free_count as i32);
    kprint_str(b"  (\0".as_ptr());
    kprint_int((free_count / 256) as i32); // free_count * 4096 / 1024 / 1024
    kprint_str(b" MB)\n\0".as_ptr());
    klog_msg(LOG_OK, b"PMM ready\0".as_ptr());
}

#[unsafe(no_mangle)]
pub extern "C" fn kalloc() -> *mut u8 {
    const MAX_RECLAIM_ROUNDS: u32 = 4;

    let mut round: u32 = 0;
    loop {
        lock_acquire(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
        let hint = *FIRST_AVAILABLE_PAGE.get_mut();
        let mut i = hint;
        while i < TOTAL_PAGES {
            if !bitmap_test(i) {
                bitmap_set(i);
                PAGE_REFCOUNTS.get_mut()[i as usize] = 1;
                *FIRST_AVAILABLE_PAGE.get_mut() = i + 1;
                lock_release(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
                return page_to_addr(i) as *mut u8;
            }
            i += 1;
        }
        lock_release(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);

        if round >= MAX_RECLAIM_ROUNDS {
            break;
        }
        round += 1;

        let reclaimed = if crate::fault::swap::swap_is_enabled() {
            let pd = crate::safe::current_page_dir();
            if !pd.is_null() && crate::fault::swap::swap_evict_page(pd) == 0 {
                true
            } else {
                crate::fault::oom::oom_kill() == 0
            }
        } else {
            crate::fault::oom::oom_kill() == 0
        };

        if !reclaimed {
            break;
        }
    }

    kprint_str(b"[PMM] kalloc: OUT OF MEMORY\n\0".as_ptr());
    core::ptr::null_mut()
}

#[unsafe(no_mangle)]
pub extern "C" fn kfree_page(ptr: *mut u8) {
    if ptr.is_null() { return; }

    let addr = ptr as u32;
    // Must be page-aligned and within the managed range.
    if addr % PAGE_SIZE != 0 { return; }

    let page_idx = addr_to_page(addr);
    if page_idx >= TOTAL_PAGES { return; }

    // Never allow freeing the hard-reserved low 32 MB (below the heap window).
    if addr < RESERVED_END { return; }

    // RESERVED_END == HEAP_START: pages in the dedicated kernel heap *window*
    // must never be returned to the PMM — they are not tracked like kalloc pages.
    let heap_end_addr = HEAP_START.saturating_add(HEAP_SIZE).min(PCI_HOLE_START);
    if addr >= HEAP_START && addr < heap_end_addr {
        return;
    }

    lock_acquire(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);

    let rc = &mut PAGE_REFCOUNTS.get_mut()[page_idx as usize];
    if *rc > 1 {
        *rc -= 1;
        lock_release(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }
    *rc = 0;
    bitmap_clear(page_idx);

    // Update first-fit hint if this page is earlier than the current hint.
    let first = FIRST_AVAILABLE_PAGE.get_mut();
    if page_idx < *first {
        *first = page_idx;
    }

    lock_release(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
}

pub fn page_ref_inc(phys: *const u8) {
    let addr = phys as u32;
    if addr < RESERVED_END { return; }
    let idx = addr_to_page(addr);
    if idx >= TOTAL_PAGES { return; }

    lock_acquire(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
    PAGE_REFCOUNTS.get_mut()[idx as usize] =
        PAGE_REFCOUNTS.get_mut()[idx as usize].saturating_add(1);
    lock_release(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
}

/// Read the reference count of a physical page under `PAGE_LOCK`.
///
/// The lock guarantees that the value is not stale: `page_ref_inc` and
/// `kfree_page` both modify the count under the same lock, so holding it
/// here provides a sequentially-consistent view of the refcount.
pub fn page_ref_get(phys: *const u8) -> u16 {
    let addr = phys as u32;
    if addr < RESERVED_END { return 0; }
    let idx = addr_to_page(addr);
    if idx >= TOTAL_PAGES { return 0; }
    lock_acquire(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
    let rc = PAGE_REFCOUNTS.get_mut()[idx as usize];
    lock_release(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
    rc
}

/// Read the reference count **without** acquiring `PAGE_LOCK`.
///
/// # Safety
/// The caller **must** already hold `PAGE_LOCK`.  Use this inside a critical
/// section where the lock is acquired around a check-then-act sequence (e.g.
/// the COW sole-owner fast path in `vmm_handle_cow`).
pub(crate) fn page_ref_get_locked(phys: *const u8) -> u16 {
    let addr = phys as u32;
    if addr < RESERVED_END { return 0; }
    let idx = addr_to_page(addr);
    if idx >= TOTAL_PAGES { return 0; }
    PAGE_REFCOUNTS.get_mut()[idx as usize]
}
