//! mmap table cloning (fork) and teardown. Split out of `mmap.rs` so the
//! COW/private-PDE machinery stays together with its two main users.

use crate::ffi::*;
use crate::pmm::{kalloc, page_ref_inc};
use crate::process::memfd::memfd_map_inc;
use crate::safe::flush_tlb_all;
use crate::vmm::mmap::{do_munmap, ensure_pde_private, EnsurePteTable};

/// # Safety
///
/// `src` and `dst` must be live, distinct `MmapTable`s and `src_pd`/`dst_pd`
/// valid page directories owned by the caller; none of them may be mutated
/// concurrently for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mmap_table_clone(
    src: *mut MmapTable,
    dst: *mut MmapTable,
    src_pd: *mut u32,
    dst_pd: *mut u32,
) {
    if src.is_null() || dst.is_null() {
        return;
    }

    // SAFETY: `src`/`dst` are valid `MmapTable`s (the caller checked them), so
    // this bump-pointer read is in bounds.
    let next_base = unsafe { (*src).next_base };
    // SAFETY: as above, for the destination table.
    unsafe { (*dst).next_base = next_base };

    for i in 0..MMAP_MAX_REGIONS {
        // SAFETY: `src` is a valid `MmapTable` and `i < MMAP_MAX_REGIONS`, so the region
        // reference is in bounds.
        let sr = unsafe { &(*src).regions[i] };
        // SAFETY: `dst` is a valid `MmapTable`, distinct from `src`, and `i` is in
        // bounds of its region array.
        let dr = unsafe { &mut (*dst).regions[i] };

        if sr.is_used == 0 {
            dr.is_used = 0;
            continue;
        }

        // SAFETY: `sr` is an in-bounds region slot of the source table.
        let src_region = unsafe { core::ptr::read(sr) };
        // SAFETY: `dr` is the corresponding in-bounds slot of the distinct
        // destination table, so the places do not overlap.
        unsafe { core::ptr::write(dr, src_region) };

        let pages = sr.length / PAGE_SIZE;

        if sr.flags & MAP_SHARED as u32 != 0 {
            // SAFETY: `src_pd`/`dst_pd` are the caller's valid, distinct page
            // directories (1024 `u32` entries, one page each), so these slices
            // span exactly one directory each; `pd_index` masks to 10 bits, so
            // every index below is in bounds.
            let src_pd = unsafe { core::slice::from_raw_parts(src_pd, 1024) };
            // SAFETY: as above, for the child directory, which this branch writes.
            let dst_pd = unsafe { core::slice::from_raw_parts_mut(dst_pd, 1024) };
            // A memfd-backed region stays alive for the child as well.
            if sr.shobj > 0 {
                memfd_map_inc(sr.shobj);
            }
            for p in 0..pages {
                let va = sr.base + p * PAGE_SIZE;
                let pdi = pd_index(va) as usize;
                let pti = pt_index(va) as usize;

                if src_pd[pdi] & PAGE_PRESENT == 0 {
                    continue;
                }
                let src_pt = (src_pd[pdi] & !0xFFF) as *const u32;
                // SAFETY: `src_pt` addresses the live source page table and
                // `pti < 1024`, so this entry pointer is in bounds.
                let src_entry = unsafe { src_pt.add(pti) };
                // SAFETY: `src_entry` points at one initialised `u32` entry.
                let pte = unsafe { *src_entry };
                if pte == 0 {
                    continue;
                }

                if dst_pd[pdi] & PAGE_PRESENT == 0 {
                    let new_pt = kalloc() as *mut u32;
                    if new_pt.is_null() {
                        continue;
                    }
                    // SAFETY: `new_pt` is the fresh `kalloc` page just checked
                    // non-null; zeroing `PAGE_SIZE` bytes covers exactly that page.
                    unsafe { core::ptr::write_bytes(new_pt as *mut u8, 0, PAGE_SIZE as usize) };
                    dst_pd[pdi] =
                        (new_pt as u32 & !0xFFF) | PAGE_PRESENT | PAGE_RW | PAGE_USER | PDE_PRIVATE;
                } else if matches!(
                    // SAFETY: `dst_pd` is a valid page directory and `pdi` came
                    // from `pd_index`, so this entry is in bounds.
                    unsafe { ensure_pde_private(dst_pd.as_mut_ptr(), pdi) },
                    Err(EnsurePteTable::Oom) | Err(EnsurePteTable::KernelMmio)
                ) {
                    continue;
                }
                let dst_pt = (dst_pd[pdi] & !0xFFF) as *mut u32;
                // SAFETY: `dst_pt` addresses the live destination page table and
                // `pti < 1024`, so this entry pointer is in bounds.
                let dst_entry = unsafe { dst_pt.add(pti) };
                // SAFETY: `dst_entry` points at one `u32` entry, and `pte` was
                // read from the matching source entry.
                unsafe { *dst_entry = pte };

                if pte & PAGE_PRESENT != 0 {
                    page_ref_inc((pte & !0xFFF) as *const u8);
                }
            }
        } else {
            // SAFETY: `src_pd`/`dst_pd` are the caller's valid, distinct page
            // directories (1024 `u32` entries, one page each), so these slices
            // span exactly one directory each; `pd_index` masks to 10 bits, so
            // every index below is in bounds.
            let src_pd = unsafe { core::slice::from_raw_parts_mut(src_pd, 1024) };
            // SAFETY: as above, for the child directory.
            let dst_pd = unsafe { core::slice::from_raw_parts_mut(dst_pd, 1024) };
            for p in 0..pages {
                let va = sr.base + p * PAGE_SIZE;
                let pdi = pd_index(va) as usize;
                let pti = pt_index(va) as usize;

                if src_pd[pdi] & PAGE_PRESENT == 0 {
                    continue;
                }
                let src_pt_before = (src_pd[pdi] & !0xFFF) as *const u32;
                // SAFETY: `src_pt_before` addresses the live source page table
                // and `pti < 1024`, so this entry pointer is in bounds.
                let src_before_entry = unsafe { src_pt_before.add(pti) };
                // SAFETY: `src_before_entry` points at one initialised `u32`.
                let pte = unsafe { *src_before_entry };

                if dst_pd[pdi] & PAGE_PRESENT == 0 {
                    let new_pt = kalloc() as *mut u32;
                    if new_pt.is_null() {
                        continue;
                    }
                    // SAFETY: `new_pt` is the fresh `kalloc` page just checked
                    // non-null; zeroing `PAGE_SIZE` bytes covers exactly that page.
                    unsafe { core::ptr::write_bytes(new_pt as *mut u8, 0, PAGE_SIZE as usize) };
                    dst_pd[pdi] =
                        (new_pt as u32 & !0xFFF) | PAGE_PRESENT | PAGE_RW | PAGE_USER | PDE_PRIVATE;
                } else if matches!(
                    // SAFETY: `dst_pd` is a valid page directory and `pdi` came
                    // from `pd_index`, so this entry is in bounds.
                    unsafe { ensure_pde_private(dst_pd.as_mut_ptr(), pdi) },
                    Err(EnsurePteTable::Oom) | Err(EnsurePteTable::KernelMmio)
                ) {
                    continue;
                }
                let dst_pt = (dst_pd[pdi] & !0xFFF) as *mut u32;

                if pte & PAGE_PRESENT == 0 {
                    // SAFETY: `dst_pt` addresses the live destination page table
                    // and `pti < 1024`, so this entry pointer is in bounds.
                    let dst_entry = unsafe { dst_pt.add(pti) };
                    // SAFETY: `dst_entry` points at one `u32` entry.
                    unsafe { *dst_entry = pte };
                    continue;
                }

                if matches!(
                    // SAFETY: `src_pd` is a valid page directory and `pdi` came
                    // from `pd_index`, so this entry is in bounds.
                    unsafe { ensure_pde_private(src_pd.as_mut_ptr(), pdi) },
                    Err(EnsurePteTable::Oom) | Err(EnsurePteTable::KernelMmio)
                ) {
                    continue;
                }
                let src_pt = (src_pd[pdi] & !0xFFF) as *mut u32;

                let cow_pte = (pte & !PAGE_RW) | PAGE_COW;
                // SAFETY: `src_pt` addresses the live source page table and
                // `pti < 1024`, so this entry pointer is in bounds.
                let src_entry = unsafe { src_pt.add(pti) };
                // SAFETY: `dst_pt` addresses the live destination page table and
                // `pti < 1024`, so this entry pointer is in bounds.
                let dst_entry = unsafe { dst_pt.add(pti) };
                // SAFETY: `src_entry` points at one `u32` entry, which receives
                // the COW-marked PTE.
                unsafe { *src_entry = cow_pte };
                // SAFETY: `dst_entry` points at one `u32` entry, which receives
                // the same COW-marked PTE.
                unsafe { *dst_entry = cow_pte };

                page_ref_inc((pte & !0xFFF) as *const u8);
            }

            flush_tlb_all();
        }
    }
}

/// # Safety
///
/// `tbl` must be null or a live `MmapTable` whose regions are mapped into `pd`,
/// and `pd` must be a valid page directory owned by the caller.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mmap_table_free(tbl: *mut MmapTable, pd: *mut u32) {
    if tbl.is_null() || pd.is_null() {
        return;
    }
    // SAFETY: tbl is valid.
    for i in 0..MMAP_MAX_REGIONS {
        // SAFETY: `tbl` is valid (checked at entry) and `i < MMAP_MAX_REGIONS`.
        let r = unsafe { &(*tbl).regions[i] };
        if r.is_used == 0 {
            continue;
        }
        // SAFETY: `tbl`/`pd` are valid (checked at entry) and `r.base`/`r.length`
        // describe the live region being torn down.
        unsafe { do_munmap(pd, tbl, r.base, r.length); }
    }
}
