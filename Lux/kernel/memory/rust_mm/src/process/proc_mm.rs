use crate::ffi::*;
use crate::pmm::kfree_page;
use crate::alloc::heap::{kmalloc, kfree_heap};
use crate::vmm::paging::vmm_free_address_space;
use crate::safe::kprint_str;

//public api
#[unsafe(no_mangle)]
pub extern "C" fn proc_tracker_add(t: *mut ProcPageTracker, phys: *mut u8) -> i32 {
    if t.is_null() || phys.is_null() {
        return -1;
    }

    // SAFETY: t is a valid ProcPageTracker.
    let pages_ptr = unsafe { (*t).pages };
    if pages_ptr.is_null() {
        let new_pages = kmalloc(PROC_INITIAL_PAGES * core::mem::size_of::<*mut u8>() as u32)
            as *mut *mut u8;
        if new_pages.is_null() {
            kprint_str(b"[PROC_MM] ERR: initial alloc failed\n\0".as_ptr());
            return -1;
        }
        for i in 0..PROC_INITIAL_PAGES as usize {
            // SAFETY: new_pages was just allocated with enough capacity.
            unsafe { *new_pages.add(i) = core::ptr::null_mut(); }
        }
        unsafe {
            (*t).pages = new_pages;
            (*t).capacity = PROC_INITIAL_PAGES;
        }
    }

    let count = unsafe { (*t).count };
    let capacity = unsafe { (*t).capacity };

    if count >= capacity {
        let new_cap = capacity + PROC_GROW_STEP;
        let new_arr = kmalloc(new_cap * core::mem::size_of::<*mut u8>() as u32) as *mut *mut u8;
        if new_arr.is_null() {
            kprint_str(b"[PROC_MM] ERR: grow alloc failed\n\0".as_ptr());
            return -1;
        }
        // SAFETY: copy old entries to new array.
        let old_pages = unsafe { (*t).pages };
        for i in 0..count as usize {
            unsafe { *new_arr.add(i) = *old_pages.add(i); }
        }
        for i in count as usize..new_cap as usize {
            unsafe { *new_arr.add(i) = core::ptr::null_mut(); }
        }
        kfree_heap(old_pages as *mut u8);
        unsafe {
            (*t).pages = new_arr;
            (*t).capacity = new_cap;
        }
    }

    // SAFETY: t is valid and count < capacity.
    unsafe {
        *(*t).pages.add(count as usize) = phys;
        (*t).count += 1;
    }
    0
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn proc_free_pages(t: *mut ProcPageTracker) {
    if t.is_null() {
        return;
    }

    // SAFETY: t is a valid ProcPageTracker.
    let pages = unsafe { (*t).pages };
    if !pages.is_null() {
        let count = unsafe { (*t).count };
        for i in 0..count as usize {
            // SAFETY: pages array is valid up to count.
            let p = unsafe { *pages.add(i) };
            if !p.is_null() {
                kfree_page(p);
                unsafe { *pages.add(i) = core::ptr::null_mut(); }
            }
        }
        kfree_heap(pages as *mut u8);
    }

    // SAFETY: t is valid.
    unsafe {
        (*t).pages = core::ptr::null_mut();
        (*t).count = 0;
        (*t).capacity = 0;

        let pd = (*t).page_dir;
        if !pd.is_null() {
            vmm_free_address_space(pd);
            (*t).page_dir = core::ptr::null_mut();
        }
    }
}
