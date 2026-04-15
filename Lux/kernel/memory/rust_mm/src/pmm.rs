use crate::ffi::*;

static mut MEMORY_BITMAP: [u8; BITMAP_SIZE as usize] = [0u8; BITMAP_SIZE as usize];
static mut PAGE_REFCOUNTS: [u16; TOTAL_PAGES as usize] = [0u16; TOTAL_PAGES as usize];
static mut FIRST_AVAILABLE_PAGE: i32 = 0;
static mut PAGE_LOCK: IrqSpinlock = IrqSpinlock { spin_locked: 0, saved_flags: 0 };

unsafe fn bitmap_set(idx: u32) {
    MEMORY_BITMAP[(idx / 8) as usize] |= 1 << (idx % 8);
}

unsafe fn bitmap_clear(idx: u32) {
    MEMORY_BITMAP[(idx / 8) as usize] &= !(1 << (idx % 8));
}

unsafe fn bitmap_test(idx: u32) -> bool {
    MEMORY_BITMAP[(idx / 8) as usize] & (1 << (idx % 8)) != 0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn init_memory_manager() {
    let mut buf = [0u8; 16];
    kprint(b"[PMM] bitmap at 0x\0".as_ptr());
    hex_to_ascii(MEMORY_BITMAP.as_ptr() as u32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"  size=\0".as_ptr());
    itoa(BITMAP_SIZE as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b" B  total_pages=\0".as_ptr());
    itoa(TOTAL_PAGES as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"\n\0".as_ptr());

    irq_spinlock_init(&raw mut PAGE_LOCK);
    for i in 0..BITMAP_SIZE as usize {
        MEMORY_BITMAP[i] = 0;
    }

    let reserved_end = HEAP_START + HEAP_SIZE;
    let reserved_pages = (reserved_end - MEM_START + PAGE_SIZE - 1) / PAGE_SIZE;

    kprint(b"[PMM] reserving pages 0..\0".as_ptr());
    itoa((reserved_pages - 1) as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"  (kernel + heap region ends at 0x\0".as_ptr());
    hex_to_ascii(reserved_end, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b")\n\0".as_ptr());

    for i in 0..reserved_pages {
        bitmap_set(i);
    }
    FIRST_AVAILABLE_PAGE = reserved_pages as i32;

    kprint(b"[PMM] free pages available from #\0".as_ptr());
    itoa(FIRST_AVAILABLE_PAGE, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"  (\0".as_ptr());
    itoa(((TOTAL_PAGES - reserved_pages) * 4) as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b" KB free)\n\0".as_ptr());
    klog(LOG_OK, b"PMM ready\0".as_ptr());
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kalloc() -> *mut u8 {
    irq_spinlock_acquire(&raw mut PAGE_LOCK);
    let mut i = FIRST_AVAILABLE_PAGE as u32;
    while i < TOTAL_PAGES {
        if !bitmap_test(i) {
            bitmap_set(i);
            PAGE_REFCOUNTS[i as usize] = 1;
            FIRST_AVAILABLE_PAGE = (i + 1) as i32;
            irq_spinlock_release(&raw mut PAGE_LOCK);
            return (MEM_START + i * PAGE_SIZE) as *mut u8;
        }
        i += 1;
    }
    irq_spinlock_release(&raw mut PAGE_LOCK);

    if crate::fault::swap::swap_is_enabled_internal() {
        let pd = get_current_pd();
        if !pd.is_null() && crate::fault::swap::swap_evict_page_internal(pd) == 0 {
            return kalloc();
        }
    }

    if crate::fault::oom::oom_kill_internal() == 0 {
        return kalloc();
    }

    kprint(b"[PMM] kalloc: OUT OF MEMORY\n\0".as_ptr());
    core::ptr::null_mut()
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kfree_page(ptr: *mut u8) {
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
    irq_spinlock_acquire(&raw mut PAGE_LOCK);
    if PAGE_REFCOUNTS[page_idx as usize] > 1 {
        PAGE_REFCOUNTS[page_idx as usize] -= 1;
        irq_spinlock_release(&raw mut PAGE_LOCK);
        return;
    }
    PAGE_REFCOUNTS[page_idx as usize] = 0;
    bitmap_clear(page_idx);
    if (page_idx as i32) < FIRST_AVAILABLE_PAGE {
        FIRST_AVAILABLE_PAGE = page_idx as i32;
    }
    irq_spinlock_release(&raw mut PAGE_LOCK);
}

pub unsafe fn page_ref_inc(phys: *const u8) {
    let addr = phys as u32;
    if addr < MEM_START {
        return;
    }
    let idx = (addr - MEM_START) / PAGE_SIZE;
    if idx >= TOTAL_PAGES {
        return;
    }
    irq_spinlock_acquire(&raw mut PAGE_LOCK);
    PAGE_REFCOUNTS[idx as usize] = PAGE_REFCOUNTS[idx as usize].saturating_add(1);
    irq_spinlock_release(&raw mut PAGE_LOCK);
}

pub unsafe fn page_ref_get(phys: *const u8) -> u16 {
    let addr = phys as u32;
    if addr < MEM_START {
        return 0;
    }
    let idx = (addr - MEM_START) / PAGE_SIZE;
    if idx >= TOTAL_PAGES {
        return 0;
    }
    PAGE_REFCOUNTS[idx as usize]
}