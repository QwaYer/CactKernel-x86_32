//! Demand/zero/COW mapping helpers exposed to C. Split out of `page_fault.rs`.

use crate::ffi::*;
use crate::pmm::{kalloc, free_page, page_ref_get_locked, PAGE_LOCK};
use crate::vmm::paging::vmm_map;
use crate::fault::page_fault::{ensure_private_pt, pte_get};
use crate::safe::{zero_page, flush_tlb, lock_acquire, lock_release};

#[unsafe(no_mangle)]
pub extern "C" fn vmm_map_demand(
    pd: *mut u32,
    virtual_addr: u32,
    size: u32,
    flags: i32,
) -> i32 {
    if pd.is_null() || size == 0 {
        return -1;
    }
    let flags = flags as u32;
    let start = virtual_addr & !0xFFF;
    let end = virtual_addr.saturating_add(size).saturating_add(0xFFF) & !0xFFF;
    
    if start >= USER_STACK_TOP || end > USER_STACK_TOP {
        return -1;
    }

    let mut va = start;
    while va < end {
        let pdi = pd_index(va) as usize;

        // SAFETY: pd is valid.
        unsafe {
            let pt = ensure_private_pt(pd, pdi, flags);
            if pt.is_null() { return -1; }
            *pt.add(pt_index(va) as usize) = PAGE_DEMAND | (flags & PAGE_USER);
        }
        va += PAGE_SIZE;
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn vmm_map_zero(
    pd: *mut u32,
    virtual_addr: u32,
    size: u32,
    flags: i32,
) -> i32 {
    if pd.is_null() || size == 0 {
        return -1;
    }
    let flags = flags as u32;
    let start = virtual_addr & !0xFFF;
    // Use saturating arithmetic so an overflowing range is caught by the
    // kernel-space boundary check below rather than wrapping to a low address.
    let end = virtual_addr.saturating_add(size).saturating_add(0xFFF) & !0xFFF;
    // C-08: never install demand entries inside kernel space (≥ 0xC000_0000).
    // A saturated end will be ≥ USER_STACK_TOP and is therefore also rejected.
    if start >= USER_STACK_TOP || end > USER_STACK_TOP {
        return -1;
    }

    let mut va = start;
    while va < end {
        let pdi = pd_index(va) as usize;

        // SAFETY: pd is valid.
        unsafe {
            let pt = ensure_private_pt(pd, pdi, flags);
            if pt.is_null() { return -1; }
            *pt.add(pt_index(va) as usize) = PAGE_DEMAND | PAGE_ZERO | (flags & PAGE_USER);
        }
        va += PAGE_SIZE;
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn vmm_map_cow(pd: *mut u32, virtual_addr: u32) -> i32 {
    let pte = pte_get(pd, virtual_addr);
    if pte.is_null() || unsafe { *pte & PAGE_PRESENT == 0 } {
        return -1;
    }
    // SAFETY: pte is valid.
    unsafe { *pte = (*pte & !PAGE_RW) | PAGE_COW; }
    flush_tlb(virtual_addr & !0xFFF);
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn vmm_is_cow_page(pd: *mut u32, virtual_addr: u32) -> i32 {
    let pte = pte_get(pd, virtual_addr);
    if !pte.is_null() && unsafe { *pte & PAGE_COW != 0 } {
        1
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn vmm_handle_cow(pd: *mut u32, virtual_addr: u32) -> i32 {
    let pte = pte_get(pd, virtual_addr);
    if pte.is_null() || unsafe { *pte & PAGE_COW == 0 } {
        return -1;
    }

    // SAFETY: pte is valid and has COW flag.
    let old_phys = unsafe { *pte & !0xFFF } as *mut u8;
    let pte_val = unsafe { *pte };
    let flags = ((pte_val & 0xFFF) & !PAGE_COW) | PAGE_RW | PAGE_PRESENT;

    // ── Sole-owner fast path ─────────────────────────────────────────────────
    // Hold PAGE_LOCK for the entire check + PTE-update so that no other CPU
    // can observe rc == 1 and also promote its own PTE to RW for the same
    // physical frame (C-01: two processes writing to one physical frame).
    lock_acquire(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
    if page_ref_get_locked(old_phys) == 1 {
        unsafe { *pte = (old_phys as u32 & !0xFFF) | flags; }
        lock_release(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);
        flush_tlb(virtual_addr & !0xFFF);
        return 0;
    }
    lock_release(PAGE_LOCK.as_ptr() as *mut IrqSpinlock);

    // ── Multi-owner copy path ────────────────────────────────────────────────
    // Allocate the new frame outside the lock (kalloc takes PAGE_LOCK itself).
    let new_phys = kalloc();
    if new_phys.is_null() {
        return -1;
    }

    // SAFETY: copying page contents.
    unsafe {
        core::ptr::copy_nonoverlapping(old_phys as *const u8, new_phys, PAGE_SIZE as usize);
        *pte = (new_phys as u32 & !0xFFF) | flags;
    }

    // Release our reference to the shared frame.
    free_page(old_phys);

    flush_tlb(virtual_addr & !0xFFF);
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn vmm_setup_user_stack(pd: *mut u32, initial_size: u32) -> u32 {
    if pd.is_null() {
        return 0;
    }

    let mut initial_size = (initial_size + 0xFFF) & !0xFFF;
    if initial_size == 0 {
        initial_size = PAGE_SIZE;
    }

    let bottom = USER_STACK_TOP - initial_size;

    let mut va = bottom;
    while va < USER_STACK_TOP {
        let phys = kalloc();
        if phys.is_null() {
            return 0;
        }
        zero_page(phys);
        vmm_map(
            pd,
            va,
            phys as u32,
            (PAGE_PRESENT | PAGE_RW | PAGE_USER) as i32,
        );
        va += PAGE_SIZE;
    }

    USER_STACK_TOP
}
