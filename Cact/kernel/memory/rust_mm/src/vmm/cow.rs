use crate::ffi::*;
use crate::pmm::{kalloc, page_ref_inc};
use crate::safe::{zero_page, flush_tlb_all};
use crate::vmm::paging::PD_KERNEL_ENTRIES;

//public api
#[unsafe(no_mangle)]
pub extern "C" fn vmm_fork_address_space(src_pd: *mut u32, dst_pd: *mut u32) {
    if src_pd.is_null() || dst_pd.is_null() {
        return;
    }

    let mmap_pde_start = (MMAP_BASE >> 22) as usize;
    let mmap_pde_end = (MMAP_LIMIT >> 22) as usize;

    for i in 0..PD_KERNEL_ENTRIES as usize {
        // Skip mmap region — mmap_table_clone handles these PDEs.
        if i >= mmap_pde_start && i < mmap_pde_end {
            continue;
        }

        // SAFETY: src_pd is a valid page directory.
        let src_pde = unsafe { *src_pd.add(i) };

        if src_pde & PDE_PRIVATE == 0 {
            // Shared kernel entry: copy the PDE reference as-is so both
            // parent and child point to the same kernel page table.
            unsafe { *dst_pd.add(i) = src_pde; }
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
            // SAFETY: src_pt is a valid page table within src_pd.
            let pte = unsafe { *src_pt.add(j) };

            if pte & PAGE_PRESENT == 0 {
                // Copy demand/swap PTEs verbatim (no physical page yet).
                unsafe { *dst_pt.add(j) = pte; }
                continue;
            }

            if pte & PAGE_USER == 0 {
                // Kernel identity-map PTE (no PAGE_USER): share it read-only
                // without incrementing the refcount — the kernel PMM owns it.
                unsafe { *dst_pt.add(j) = pte; }
                continue;
            }

            // User page: mark both parent and child COW.
            let phys = pte & !0xFFFu32;
            let flags = pte & 0xFFFu32;
            let cow_flags = (flags & !PAGE_RW) | PAGE_COW;

            // SAFETY: writing COW PTEs into both page tables.
            unsafe {
                *src_pt.add(j) = phys | cow_flags;
                *dst_pt.add(j) = phys | cow_flags;
            }
            page_ref_inc(phys as *const u8);
        }

        // SAFETY: writing the PDE into dst_pd.
        unsafe {
            *dst_pd.add(i) = (dst_pt as u32 & !0xFFFu32) | (src_pde & 0xFFFu32);
        }
    }

    flush_tlb_all();
}
