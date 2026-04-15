use crate::ffi::*;
use crate::pmm::{kalloc, page_ref_inc};

fn zero_page(p: *mut u8) {
    unsafe {
        core::ptr::write_bytes(p, 0, PAGE_SIZE as usize);
    }
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_fork_address_space(src_pd: *mut u32, dst_pd: *mut u32) {
    if src_pd.is_null() || dst_pd.is_null() {
        return;
    }

    let mmap_pde_start = (MMAP_BASE >> 22) as usize;
    let mmap_pde_end = (MMAP_LIMIT >> 22) as usize;

    for i in 32..768 {
        // Skip mmap region — mmap_table_clone handles these PDEs
        if i >= mmap_pde_start && i < mmap_pde_end {
            continue;
        }

        if *src_pd.add(i) & PAGE_PRESENT == 0 {
            *dst_pd.add(i) = 0;
            continue;
        }

        let src_pt = (*src_pd.add(i) & !0xFFFu32) as *mut u32;
        let dst_pt = kalloc() as *mut u32;
        if dst_pt.is_null() {
            continue;
        }
        zero_page(dst_pt as *mut u8);

        for j in 0..1024usize {
            let pte = *src_pt.add(j);
            if pte & PAGE_PRESENT == 0 {
                *dst_pt.add(j) = pte;
                continue;
            }

            let phys = pte & !0xFFFu32;
            let flags = pte & 0xFFFu32;

            let cow_flags = (flags & !PAGE_RW) | PAGE_COW;
            *src_pt.add(j) = phys | cow_flags;
            *dst_pt.add(j) = phys | cow_flags;

            page_ref_inc(phys as *const u8);
        }

        *dst_pd.add(i) = (dst_pt as u32 & !0xFFFu32) | (*src_pd.add(i) & 0xFFFu32);
    }

    tlb_flush_all();
}
