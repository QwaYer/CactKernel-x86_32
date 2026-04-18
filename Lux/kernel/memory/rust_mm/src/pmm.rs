use crate::ffi::*;
use crate::safe::{KStatic, lock_acquire, lock_release, kprint_str, kprint_int, kprint_hex, klog_msg};

static MEMORY_BITMAP: KStatic<[u8; BITMAP_SIZE as usize]> = KStatic::new([0u8; BITMAP_SIZE as usize]);
static PAGE_REFCOUNTS: KStatic<[u16; TOTAL_PAGES as usize]> = KStatic::new([0u16; TOTAL_PAGES as usize]);
static FIRST_AVAILABLE_PAGE: KStatic<i32> = KStatic::new(0);
static PAGE_LOCK: KStatic<IrqSpinlock> = KStatic::new(IrqSpinlock { spin_locked: 0, saved_flags: 0 });

fn bitmap_set(idx: u32) {
    let bm = MEMORY_BITMAP.get_mut();
    bm[(idx / 8) as usize] |= 1 << (idx % 8);
}

fn bitmap_clear(idx: u32) {
    let bm = MEMORY_BITMAP.get_mut();
    bm[(idx / 8) as usize] &= !(1 << (idx % 8));
}

fn bitmap_test(idx: u32) -> bool {
    let bm = MEMORY_BITMAP.get_mut();
    bm[(idx / 8) as usize] & (1 << (idx % 8)) != 0
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn init_memory_manager() {
    kprint_str(b"[PMM] bitmap at 0x\0".as_ptr());
    kprint_hex(MEMORY_BITMAP.as_ptr() as u32);
    kprint_str(b"  size=\0".as_ptr());
    kprint_int(BITMAP_SIZE as i32);
    kprint_str(b" B  total_pages=\0".as_ptr());
    kprint_int(TOTAL_PAGES as i32);
    kprint_str(b"\n\0".as_ptr());

    // SAFETY: init is called once during boot, no concurrent access.
    unsafe { irq_spinlock_init(PAGE_LOCK.as_ptr() as *mut IrqSpinlock) };
    {
        let bm = MEMORY_BITMAP.get_mut();
        for i in 0..BITMAP_SIZE as usize {
            bm[i] = 0;
        }
    }

    let reserved_end = HEAP_START + HEAP_SIZE;
    let reserved_pages = (reserved_end - MEM_START + PAGE_SIZE - 1) / PAGE_SIZE;

    kprint_str(b"[PMM] reserving pages 0..\0".as_ptr());
    kprint_int((reserved_pages - 1) as i32);
    kprint_str(b"  (kernel + heap region ends at 0x\0".as_ptr());
    kprint_hex(reserved_end);
    kprint_str(b")\n\0".as_ptr());

    for i in 0..reserved_pages {
        bitmap_set(i);
    }
    *FIRST_AVAILABLE_PAGE.get_mut() = reserved_pages as i32;

    kprint_str(b"[PMM] free pages available from #\0".as_ptr());
    kprint_int(reserved_pages as i32);
    kprint_str(b"  (\0".as_ptr());
    kprint_int(((TOTAL_PAGES - reserved_pages) * 4) as i32);
    kprint_str(b" KB free)\n\0".as_ptr());
    klog_msg(LOG_OK, b"PMM ready\0".as_ptr());
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn kalloc() -> *mut u8 {
    lock_acquire(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);

    let mut i = *FIRST_AVAILABLE_PAGE.get_mut() as u32;
    while i < TOTAL_PAGES {
        if !bitmap_test(i) {
            bitmap_set(i);
            PAGE_REFCOUNTS.get_mut()[i as usize] = 1;
            *FIRST_AVAILABLE_PAGE.get_mut() = (i + 1) as i32;
            lock_release(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
            return (MEM_START + i * PAGE_SIZE) as *mut u8;
        }
        i += 1;
    }
    lock_release(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);

    if crate::fault::swap::swap_is_enabled() {
        let pd = crate::safe::current_page_dir();
        if !pd.is_null() && crate::fault::swap::swap_evict_page(pd) == 0 {
            return kalloc();
        }
    }

    if crate::fault::oom::oom_kill() == 0 {
        return kalloc();
    }

    kprint_str(b"[PMM] kalloc: OUT OF MEMORY\n\0".as_ptr());
    core::ptr::null_mut()
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn kfree_page(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    let addr = ptr as u32;
    if addr < MEM_START {
        return;
    }
    let page_idx = (addr - MEM_START) / PAGE_SIZE;
    if page_idx >= TOTAL_PAGES {
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
    let first = FIRST_AVAILABLE_PAGE.get_mut();
    if (page_idx as i32) < *first {
        *first = page_idx as i32;
    }
    lock_release(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
}

pub fn page_ref_inc(phys: *const u8) {
    let addr = phys as u32;
    if addr < MEM_START {
        return;
    }
    let idx = (addr - MEM_START) / PAGE_SIZE;
    if idx >= TOTAL_PAGES {
        return;
    }
    lock_acquire(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
    PAGE_REFCOUNTS.get_mut()[idx as usize] =
        PAGE_REFCOUNTS.get_mut()[idx as usize].saturating_add(1);
    lock_release(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
}

pub fn page_ref_get(phys: *const u8) -> u16 {
    let addr = phys as u32;
    if addr < MEM_START {
        return 0;
    }
    let idx = (addr - MEM_START) / PAGE_SIZE;
    if idx >= TOTAL_PAGES {
        return 0;
    }
    PAGE_REFCOUNTS.get_mut()[idx as usize]
}
