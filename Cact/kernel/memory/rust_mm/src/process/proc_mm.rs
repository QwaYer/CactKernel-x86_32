//! Growable array of per-process physical pages (`ProcPageTracker`) and teardown helpers.

use crate::ffi::*;
use crate::alloc::heap::{kmalloc, kfree};
use crate::vmm::paging::vmm_free_address_space;

/// # Safety
///
/// `t` must be null or point to a live `ProcPageTracker` owned by the caller
/// whose `pages` array is either null or a `kmalloc`'d array of `capacity`
/// slots; `phys` must be a caller-owned physical page pointer or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn proc_tracker_add(t: *mut ProcPageTracker, phys: *mut u8) -> i32 {
    if t.is_null() || phys.is_null() {
        return -1;
    }

    // SAFETY: `t` is non-null (checked above) and points to a valid
    // `ProcPageTracker` owned by the caller; this borrow is exclusive for the
    // whole call, and the allocator/logging calls in between never touch it.
    let t = unsafe { &mut *t };
    if t.pages.is_null() {
        t.pages = kmalloc(PROC_INITIAL_PAGES * core::mem::size_of::<*mut u8>() as u32)
            as *mut *mut u8;
        if t.pages.is_null() {
            // SAFETY: `printk` expects a NUL-terminated static string; the
            // literal below is one.
            unsafe { printk(c"[PROC_MM] ERR: initial alloc failed\n".as_ptr() as *const u8) };
            return -1;
        }
        // SAFETY: `t.pages` is the fresh `kmalloc` block of `PROC_INITIAL_PAGES`
        // slots (just checked non-null); the slice spans exactly that allocation
        // and this borrow is exclusive.
        let slots =
            unsafe { core::slice::from_raw_parts_mut(t.pages, PROC_INITIAL_PAGES as usize) };
        slots.fill(core::ptr::null_mut());
        t.capacity = PROC_INITIAL_PAGES;
    }

    if t.count >= t.capacity {
        let new_cap = t.capacity + PROC_GROW_STEP;
        let new_arr = kmalloc(new_cap * core::mem::size_of::<*mut u8>() as u32) as *mut *mut u8;
        if new_arr.is_null() {
            // SAFETY: NUL-terminated static string, as above.
            unsafe { printk(c"[PROC_MM] ERR: grow alloc failed\n".as_ptr() as *const u8) };
            return -1;
        }
        // SAFETY: `new_arr` is the fresh `kmalloc` block of `new_cap` slots (just
        // checked non-null); the slice spans exactly that allocation and this
        // borrow is exclusive for the rest of the branch.
        let new_slots = unsafe { core::slice::from_raw_parts_mut(new_arr, new_cap as usize) };
        // SAFETY: `t.pages` holds `t.count` valid slots and `t.count <= new_cap`,
        // so copying that many elements into the new block is in bounds; the two
        // allocations are distinct.
        unsafe { core::ptr::copy_nonoverlapping(t.pages, new_slots.as_mut_ptr(), t.count as usize) };
        new_slots[t.count as usize..].fill(core::ptr::null_mut());
        // SAFETY: `t.pages` is the old `kmalloc`'d array that this call owns and
        // is replacing, so it is freed exactly once here.
        unsafe { kfree(t.pages as *mut u8) };
        t.pages = new_arr;
        t.capacity = new_cap;
    }

    // SAFETY: `t.count < t.capacity` holds (the branch above guarantees it) and
    // `t.pages` is a valid `t.capacity`-slot array; the slice spans exactly that
    // allocation and this borrow is exclusive.
    let slots = unsafe { core::slice::from_raw_parts_mut(t.pages, t.capacity as usize) };
    slots[t.count as usize] = phys;
    t.count += 1;
    0
}

/// # Safety
///
/// `t` must be null or point to a live `ProcPageTracker` whose `pages` and
/// `page_dir` are owned allocations this call may release; the tracker must
/// not be used again afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn proc_free_pages(t: *mut ProcPageTracker) {
    if t.is_null() {
        return;
    }

    // SAFETY: `t` is non-null (checked above) and points to a valid
    // `ProcPageTracker`; this borrow is exclusive for the whole call, and the
    // release helpers below only touch the allocations its fields point at.
    let t = unsafe { &mut *t };
    if !t.pages.is_null() {
        // SAFETY: `t.pages` is an owned `kmalloc`'d allocation (per the caller
        // contract) that this destructor is allowed to release.
        unsafe { kfree(t.pages as *mut u8) };
    }

    t.pages = core::ptr::null_mut();
    t.count = 0;
    t.capacity = 0;

    if !t.page_dir.is_null() {
        // SAFETY: `t.page_dir` is the process's page directory, which this
        // teardown is allowed to free; the borrow of `t` ends at this statement.
        unsafe { vmm_free_address_space(t.page_dir) };
        t.page_dir = core::ptr::null_mut();
    }
}
