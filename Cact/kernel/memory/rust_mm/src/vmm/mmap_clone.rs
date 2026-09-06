//! mmap table cloning (fork) and teardown. Split out of `mmap.rs` so the
//! COW/private-PDE machinery stays together with its two main users.

use crate::ffi::*;
use crate::pmm::{kalloc, page_ref_inc};
use crate::process::memfd::memfd_map_inc;
use crate::safe::flush_tlb_all;
use crate::vmm::mmap::{do_munmap, ensure_pde_private, EnsurePteTable};

#[unsafe(no_mangle)]
pub extern "C" fn mmap_table_clone(
    src: *mut MmapTable,
    dst: *mut MmapTable,
    src_pd: *mut u32,
    dst_pd: *mut u32,
) {
    if src.is_null() || dst.is_null() {
        return;
    }

    unsafe {
        (*dst).next_base = (*src).next_base;
    }

    for i in 0..MMAP_MAX_REGIONS {
        let sr = unsafe { &(*src).regions[i] };
        let dr = unsafe { &mut (*dst).regions[i] };

        if sr.is_used == 0 {
            dr.is_used = 0;
            continue;
        }

        unsafe { core::ptr::write(dr, core::ptr::read(sr)); }

        let pages = sr.length / PAGE_SIZE;

        if sr.flags & MAP_SHARED as u32 != 0 {
            unsafe {
                // A memfd-backed region stays alive for the child as well.
                if sr.shobj > 0 {
                    memfd_map_inc(sr.shobj);
                }
                for p in 0..pages {
                    let va = sr.base + p * PAGE_SIZE;
                    let pdi = pd_index(va) as usize;
                    let pti = pt_index(va) as usize;

                    if *src_pd.add(pdi) & PAGE_PRESENT == 0 {
                        continue;
                    }
                    let src_pt = (*src_pd.add(pdi) & !0xFFF) as *mut u32;
                    let pte = *src_pt.add(pti);
                    if pte == 0 {
                        continue;
                    }

                    if *dst_pd.add(pdi) & PAGE_PRESENT == 0 {
                        let new_pt = kalloc() as *mut u32;
                        if new_pt.is_null() {
                            continue;
                        }
                        core::ptr::write_bytes(new_pt as *mut u8, 0, PAGE_SIZE as usize);
                        *dst_pd.add(pdi) =
                            (new_pt as u32 & !0xFFF) | PAGE_PRESENT | PAGE_RW | PAGE_USER | PDE_PRIVATE;
                    } else if matches!(
                        ensure_pde_private(dst_pd, pdi),
                        Err(EnsurePteTable::Oom) | Err(EnsurePteTable::KernelMmio)
                    ) {
                        continue;
                    }
                    let dst_pt = (*dst_pd.add(pdi) & !0xFFF) as *mut u32;
                    *dst_pt.add(pti) = pte;

                    if pte & PAGE_PRESENT != 0 {
                        page_ref_inc((pte & !0xFFF) as *const u8);
                    }
                }
            }
        } else {
            unsafe {
                for p in 0..pages {
                    let va = sr.base + p * PAGE_SIZE;
                    let pdi = pd_index(va) as usize;
                    let pti = pt_index(va) as usize;

                    if *src_pd.add(pdi) & PAGE_PRESENT == 0 {
                        continue;
                    }
                    let src_pt_before = (*src_pd.add(pdi) & !0xFFF) as *mut u32;
                    let pte = *src_pt_before.add(pti);

                    if *dst_pd.add(pdi) & PAGE_PRESENT == 0 {
                        let new_pt = kalloc() as *mut u32;
                        if new_pt.is_null() {
                            continue;
                        }
                        core::ptr::write_bytes(new_pt as *mut u8, 0, PAGE_SIZE as usize);
                        *dst_pd.add(pdi) =
                            (new_pt as u32 & !0xFFF) | PAGE_PRESENT | PAGE_RW | PAGE_USER | PDE_PRIVATE;
                    } else if matches!(
                        ensure_pde_private(dst_pd, pdi),
                        Err(EnsurePteTable::Oom) | Err(EnsurePteTable::KernelMmio)
                    ) {
                        continue;
                    }
                    let dst_pt = (*dst_pd.add(pdi) & !0xFFF) as *mut u32;

                    if pte & PAGE_PRESENT == 0 {
                        *dst_pt.add(pti) = pte;
                        continue;
                    }

                    if matches!(
                        ensure_pde_private(src_pd, pdi),
                        Err(EnsurePteTable::Oom) | Err(EnsurePteTable::KernelMmio)
                    ) {
                        continue;
                    }
                    let src_pt = (*src_pd.add(pdi) & !0xFFF) as *mut u32;

                    let cow_pte = (pte & !PAGE_RW) | PAGE_COW;
                    *src_pt.add(pti) = cow_pte;
                    *dst_pt.add(pti) = cow_pte;

                    page_ref_inc((pte & !0xFFF) as *const u8);
                }
            }

            flush_tlb_all();
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn mmap_table_free(tbl: *mut MmapTable, pd: *mut u32) {
    if tbl.is_null() || pd.is_null() {
        return;
    }
    // SAFETY: tbl is valid.
    for i in 0..MMAP_MAX_REGIONS {
        let r = unsafe { &(*tbl).regions[i] };
        if r.is_used == 0 {
            continue;
        }
        do_munmap(pd, tbl, r.base, r.length);
    }
}
