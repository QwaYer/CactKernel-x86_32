//! `fork` support: duplicate a page directory with copy-on-write for user-writable mappings.
//!
//! Skips the mmap PDE range (handled separately) and shares kernel identity-map PTEs read-only.

use crate::ffi::*;
use crate::pmm::{kalloc, page_ref_inc};
use crate::safe::{zero_page, flush_tlb_all};
use crate::vmm::paging::PD_KERNEL_ENTRIES;

/// # Safety
///
/// `src_pd` and `dst_pd` must be null or valid, distinct page directories
/// owned by the caller, and neither may be mutated concurrently for the
/// duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_fork_address_space(src_pd: *mut u32, dst_pd: *mut u32) {
    if src_pd.is_null() || dst_pd.is_null() {
        return;
    }

    let mmap_pde_start = (MMAP_BASE >> 22) as usize;
    let mmap_pde_end = (MMAP_LIMIT >> 22) as usize;

    // SAFETY: `src_pd`/`dst_pd` are the caller's valid, distinct page directories
    // (1024 `u32` entries, one page each), so these slices span exactly one
    // directory each and do not overlap; `i` is bounded by `PD_KERNEL_ENTRIES`,
    // which is <= 1024.
    let src_pd = unsafe { core::slice::from_raw_parts(src_pd, 1024) };
    // SAFETY: as above, for the child directory, which this call writes.
    let dst_pd = unsafe { core::slice::from_raw_parts_mut(dst_pd, 1024) };

    for i in 0..PD_KERNEL_ENTRIES {
        // Skip mmap region — mmap_table_clone handles these PDEs.
        if i >= mmap_pde_start && i < mmap_pde_end {
            continue;
        }

        let src_pde = src_pd[i];

        if src_pde & PDE_PRIVATE == 0 {
            // Shared kernel entry: copy the PDE reference as-is so both
            // parent and child point to the same kernel page table.
            dst_pd[i] = src_pde;
            continue;
        }

        // Private page table: COW-fork it for the child.
        if src_pde & PAGE_PRESENT == 0 {
            continue;
        }

        let src_pt = (src_pde & !0xFFFu32) as *mut u32;
        let dst_pt = kalloc() as *mut u32;
        if dst_pt.is_null() {
            continue;
        }
        zero_page(dst_pt as *mut u8);

        for j in 0..1024usize {
            // SAFETY: `src_pt` is the live 1024-entry page table named by the
            // present private source PDE, and `j < 1024`, so this entry pointer is
            // in bounds.
            let src_entry = unsafe { src_pt.add(j) };
            // SAFETY: `src_entry` points at one initialised PTE.
            let pte = unsafe { *src_entry };

            if pte & PAGE_PRESENT == 0 {
                // Copy demand/swap PTEs verbatim (no physical page yet).
                // SAFETY: `dst_pt` is a valid 1024-entry page table and `j < 1024`.
                let dst_entry = unsafe { dst_pt.add(j) };
                // SAFETY: `dst_entry` points at one PTE.
                unsafe { *dst_entry = pte };
                continue;
            }

            if pte & PAGE_USER == 0 {
                // Kernel identity-map PTE (no PAGE_USER): share it read-only
                // without incrementing the refcount — the kernel PMM owns it.
                // SAFETY: `dst_pt` is a valid 1024-entry page table and `j < 1024`.
                let dst_entry = unsafe { dst_pt.add(j) };
                // SAFETY: `dst_entry` points at one PTE, sharing the kernel
                // identity PTE with the child.
                unsafe { *dst_entry = pte };
                continue;
            }

            let va = ((i as u32) << 22) | ((j as u32) << 12);
            if (USER_STACK_LIMIT..USER_STACK_TOP).contains(&va) {
                // Keep user stack pages writable right after fork.
                // Copy-on-write for the stack tends to fault immediately on the
                // first function prologue/store after returning to user mode,
                // so we eagerly clone these pages for stability.
                let new_page = kalloc();
                if !new_page.is_null() {
                    let phys = pte & !0xFFFu32;
                    let mut flags = pte & 0xFFFu32;
                    flags &= !PAGE_COW;
                    flags |= PAGE_RW;
                    // SAFETY: `phys` is a page-aligned frame from the parent PTE
                    // and `new_page` a fresh `kalloc` page, so the 4 KiB copy is in
                    // bounds.
                    unsafe {
                        core::ptr::copy_nonoverlapping(
                            phys as *const u8,
                            new_page,
                            PAGE_SIZE as usize,
                        );
                    }
                    // SAFETY: `dst_pt` is a valid 1024-entry page table and `j < 1024`.
                    let dst_entry = unsafe { dst_pt.add(j) };
                    // SAFETY: `dst_entry` points at one PTE, which receives the
                    // eagerly-cloned child frame.
                    unsafe { *dst_entry = (new_page as u32 & !0xFFFu32) | flags };
                    continue;
                }
                // OOM fallback: keep previous COW behavior below.
            }

            // User page: mark both parent and child COW.
            let phys = pte & !0xFFFu32;
            let flags = pte & 0xFFFu32;
            let cow_flags = (flags & !PAGE_RW) | PAGE_COW;

            // SAFETY: `dst_pt` is a valid 1024-entry page table and `j < 1024`, so
            // this entry pointer is in bounds.
            let dst_entry = unsafe { dst_pt.add(j) };
            // SAFETY: `src_entry` points at one PTE, which receives the COW flags.
            unsafe { *src_entry = phys | cow_flags };
            // SAFETY: `dst_entry` points at one PTE, which receives the same flags.
            unsafe { *dst_entry = phys | cow_flags };
            page_ref_inc(phys as *const u8);
        }

        // SAFETY: `dst_pd` is the child page directory and `i < PD_KERNEL_ENTRIES`,
        // so this writes an in-bounds PD entry naming the new COW page table.
        dst_pd[i] = (dst_pt as u32 & !0xFFFu32) | (src_pde & 0xFFFu32);
    }

    flush_tlb_all();
}
