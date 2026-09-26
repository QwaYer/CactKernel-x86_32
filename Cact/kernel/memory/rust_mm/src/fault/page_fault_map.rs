//! Demand/zero/COW mapping helpers exposed to C. Split out of `page_fault.rs`.

use crate::ffi::*;
use crate::pmm::{kalloc, free_page, page_ref_get_locked, PAGE_LOCK};
use crate::vmm::paging::vmm_map;
use crate::fault::page_fault::{ensure_private_pt, pte_get};
use crate::safe::{zero_page, flush_tlb, lock_acquire, lock_release};

/// # Safety
///
/// `pd` must be null or a valid page directory that the caller owns and may
/// mutate; the caller must serialise concurrent page-table changes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_map_demand(
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

        // SAFETY: `pd` is the caller's valid page directory and `pdi` came from
        // `pd_index`, so the entry is in bounds; the caller serialises changes.
        let pt = unsafe { ensure_private_pt(pd, pdi, flags) };
        if pt.is_null() {
            return -1;
        }
        // SAFETY: `pt` is the live 1024-entry page table just returned and `va`
        // was produced by the same indexing, so this entry pointer is in bounds.
        let entry = unsafe { pt.add(pt_index(va) as usize) };
        // SAFETY: `entry` points at one PTE.
        unsafe { *entry = PAGE_DEMAND | (flags & PAGE_USER) };
        va += PAGE_SIZE;
    }
    0
}

/// # Safety
///
/// `pd` must be null or a valid page directory that the caller owns and may
/// mutate; the caller must serialise concurrent page-table changes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_map_zero(
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

        // SAFETY: `pd` is the caller's valid page directory and `pdi` came from
        // `pd_index`, so the entry is in bounds; the caller serialises changes.
        let pt = unsafe { ensure_private_pt(pd, pdi, flags) };
        if pt.is_null() {
            return -1;
        }
        // SAFETY: `pt` is the live 1024-entry page table just returned and `va`
        // was produced by the same indexing, so this entry pointer is in bounds.
        let entry = unsafe { pt.add(pt_index(va) as usize) };
        // SAFETY: `entry` points at one PTE.
        unsafe { *entry = PAGE_DEMAND | PAGE_ZERO | (flags & PAGE_USER) };
        va += PAGE_SIZE;
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn vmm_map_cow(pd: *mut u32, virtual_addr: u32) -> i32 {
    let pte = pte_get(pd, virtual_addr);
    // SAFETY: `pte` is the result of `pte_get` and is non-null on the left of `||`,
    // so the present-bit test reads a valid PTE.
    if pte.is_null() || unsafe { *pte & PAGE_PRESENT == 0 } {
        return -1;
    }
    // SAFETY: `pte` is the non-null live PTE returned by `pte_get`.
    let val = unsafe { *pte };
    // SAFETY: `pte` is non-null (checked above), so storing the COW-marked value
    // back is in bounds.
    unsafe { *pte = (val & !PAGE_RW) | PAGE_COW };
    flush_tlb(virtual_addr & !0xFFF);
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn vmm_is_cow_page(pd: *mut u32, virtual_addr: u32) -> i32 {
    let pte = pte_get(pd, virtual_addr);
    // SAFETY: `pte` is non-null on the left of `&&`; its COW bit is read from a live
    // PTE.
    if !pte.is_null() && unsafe { *pte & PAGE_COW != 0 } {
        1
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn vmm_handle_cow(pd: *mut u32, virtual_addr: u32) -> i32 {
    let pte = pte_get(pd, virtual_addr);
    // SAFETY: `pte` is non-null on the left of `||`; a live PTE's COW bit is read.
    if pte.is_null() || unsafe { *pte & PAGE_COW == 0 } {
        return -1;
    }

    // SAFETY: pte is valid and has COW flag.
    let old_phys = unsafe { *pte & !0xFFF } as *mut u8;
    // SAFETY: `pte` is non-null and COW-flagged; reading its value is valid.
    let pte_val = unsafe { *pte };
    let flags = ((pte_val & 0xFFF) & !PAGE_COW) | PAGE_RW | PAGE_PRESENT;

    // ── Sole-owner fast path ─────────────────────────────────────────────────
    // Hold PAGE_LOCK for the entire check + PTE-update so that no other CPU
    // can observe rc == 1 and also promote its own PTE to RW for the same
    // physical frame (C-01: two processes writing to one physical frame).
    lock_acquire(PAGE_LOCK.as_ptr());
    if page_ref_get_locked(old_phys) == 1 {
        // SAFETY: writing the private copy's frame into the live COW PTE addressed by
        // `pte`.
        unsafe { *pte = (old_phys as u32 & !0xFFF) | flags; }
        lock_release(PAGE_LOCK.as_ptr());
        flush_tlb(virtual_addr & !0xFFF);
        return 0;
    }
    lock_release(PAGE_LOCK.as_ptr());

    // ── Multi-owner copy path ────────────────────────────────────────────────
    // Allocate the new frame outside the lock (kalloc takes PAGE_LOCK itself).
    let new_phys = kalloc();
    if new_phys.is_null() {
        return -1;
    }

    // SAFETY: `old_phys` is the shared frame named by the live COW PTE and
    // `new_phys` a fresh `kalloc` page, so the 4 KiB copy is in bounds.
    unsafe { core::ptr::copy_nonoverlapping(old_phys as *const u8, new_phys, PAGE_SIZE as usize) };
    // SAFETY: `pte` is non-null (checked above), so storing the private copy's
    // frame back is in bounds.
    unsafe { *pte = (new_phys as u32 & !0xFFF) | flags };

    // Release our reference to the shared frame.
    // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
    unsafe { free_page(old_phys) };

    flush_tlb(virtual_addr & !0xFFF);
    0
}

/// # Safety
///
/// `pd` must be a valid page directory for the current address space; the
/// function writes PTEs into it and allocates frames for the user stack window.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_setup_user_stack(pd: *mut u32, initial_size: u32) -> u32 {
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
        // SAFETY: `pd` is valid (checked at entry), `va` lies in the user stack
        // window and `phys` is the zeroed frame just allocated for it.
        unsafe {
            vmm_map(
                pd,
                va,
                phys as u32,
                (PAGE_PRESENT | PAGE_RW | PAGE_USER) as i32,
            );
        }
        va += PAGE_SIZE;
    }

    USER_STACK_TOP
}
