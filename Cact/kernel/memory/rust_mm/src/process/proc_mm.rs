//! Growable array of per-process physical pages (`ProcPageTracker`) and teardown helpers.

use crate::ffi::*;
use crate::alloc::heap::{kmalloc, kfree_heap};
use crate::vmm::paging::vmm_free_address_space;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn proc_tracker_add(t: *mut ProcPageTracker, phys: *mut u8) -> i32 {
    if t.is_null() || phys.is_null() {
        return -1;
    }

    if (*t).pages.is_null() {
        (*t).pages = kmalloc(PROC_INITIAL_PAGES * core::mem::size_of::<*mut u8>() as u32)
            as *mut *mut u8;
        if (*t).pages.is_null() {
            kprint(b"[PROC_MM] ERR: initial alloc failed\n\0".as_ptr());
            return -1;
        }
        for i in 0..PROC_INITIAL_PAGES as usize {
            *(*t).pages.add(i) = core::ptr::null_mut();
        }
        (*t).capacity = PROC_INITIAL_PAGES;
    }

    if (*t).count >= (*t).capacity {
        let new_cap = (*t).capacity + PROC_GROW_STEP;
        let new_arr = kmalloc(new_cap * core::mem::size_of::<*mut u8>() as u32) as *mut *mut u8;
        if new_arr.is_null() {
            kprint(b"[PROC_MM] ERR: grow alloc failed\n\0".as_ptr());
            return -1;
        }
        for i in 0..(*t).count as usize {
            *new_arr.add(i) = *(*t).pages.add(i);
        }
        for i in (*t).count as usize..new_cap as usize {
            *new_arr.add(i) = core::ptr::null_mut();
        }
        kfree_heap((*t).pages as *mut u8);
        (*t).pages = new_arr;
        (*t).capacity = new_cap;
    }

    *(*t).pages.add((*t).count as usize) = phys;
    (*t).count += 1;
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn proc_free_pages(t: *mut ProcPageTracker) {
    if t.is_null() {
        return;
    }

    if !(*t).pages.is_null() {
        kfree_heap((*t).pages as *mut u8);
    }

    (*t).pages = core::ptr::null_mut();
    (*t).count = 0;
    (*t).capacity = 0;

    if !(*t).page_dir.is_null() {
        vmm_free_address_space((*t).page_dir);
        (*t).page_dir = core::ptr::null_mut();
    }
}
