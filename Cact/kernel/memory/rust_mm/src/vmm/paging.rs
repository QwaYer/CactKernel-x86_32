use crate::ffi::*;
use crate::safe::{kprint_str, kprint_hex, kprint_int, klog_msg};
use crate::pmm::{kalloc, kfree_page};

pub(crate) const PD_KERNEL_ENTRIES: usize = (PCI_HOLE_START / (PAGE_SIZE * 1024)) as usize;

const PD_TOTAL: usize = 1024;

#[repr(C, align(4096))]
struct Aligned4K<T>(T);

#[no_mangle]
static mut page_directory: Aligned4K<[u32; PD_TOTAL]> =
    Aligned4K([0u32; PD_TOTAL]);

static mut PAGE_TABLES: Aligned4K<[[u32; 1024]; PD_TOTAL]> =
    Aligned4K([[0u32; 1024]; PD_TOTAL]);

pub fn get_kernel_pd() -> *mut u32 {
    unsafe { page_directory.0.as_mut_ptr() }
}

#[unsafe(no_mangle)]
pub extern "C" fn init_paging() {
    kprint_str(b"[PAGING] page_directory at 0x\0".as_ptr());
    let pd_addr = unsafe { page_directory.0.as_ptr() as u32 };
    kprint_hex(pd_addr);
    kprint_str(b"\n[PAGING] PAGE_TABLES  at 0x\0".as_ptr());
    let pt_addr = unsafe { PAGE_TABLES.0.as_ptr() as u32 };
    kprint_hex(pt_addr);
    kprint_str(b"  (4 MB BSS)\n\0".as_ptr());
    kprint_str(b"[PAGING] identity-mapping full 4 GB (1024 page tables x 1024 pages)\n\0".as_ptr());

    unsafe {
        for pt_idx in 0..PD_TOTAL {
            let pt = &mut PAGE_TABLES.0[pt_idx];
            for page in 0..1024usize {
                let phys: u32 = ((pt_idx * 1024 + page) as u32)
                    .wrapping_mul(PAGE_SIZE);

                // Choose cache policy based on physical address.
                let cache_flags: u32 = if phys >= PCI_HOLE_START {
                    // MMIO / PCI hole: uncacheable, write-through.
                    PAGE_PCD | PAGE_PWT
                } else {
                    // Normal RAM: default write-back caching.
                    0
                };

                pt[page] = phys | PAGE_PRESENT | PAGE_RW | cache_flags;
            }
            // Point PD entry at this page table.
            // PD entries themselves do NOT need PCD/PWT — only PTEs do.
            page_directory.0[pt_idx] =
                (pt.as_ptr() as u32) | PAGE_PRESENT | PAGE_RW;
        }
    }

    kprint_str(b"[PAGING] loading CR3=0x\0".as_ptr());
    kprint_hex(unsafe { page_directory.0.as_ptr() as u32 });
    kprint_str(b"  setting CR0.PG\n\0".as_ptr());

    unsafe {
        load_page_directory(page_directory.0.as_mut_ptr());
        enable_paging();
    }

    kprint_str(b"[PAGING] paging enabled - 4 GB identity map active\n\0".as_ptr());
    kprint_str(b"[PAGING] RAM  (P|RW)        entries: \0".as_ptr());
    kprint_int(PD_KERNEL_ENTRIES as i32);
    kprint_str(b" (0x00000000 .. 0x\0".as_ptr());
    kprint_hex(PCI_HOLE_START);
    kprint_str(b")\n\0".as_ptr());
    kprint_str(b"[PAGING] MMIO (P|RW|PCD|PWT) entries: \0".as_ptr());
    kprint_int((PD_TOTAL - PD_KERNEL_ENTRIES) as i32);
    kprint_str(b" (0x\0".as_ptr());
    kprint_hex(PCI_HOLE_START);
    kprint_str(b" .. 0xFFFFFFFF)\n\0".as_ptr());
    klog_msg(LOG_OK, b"paging enabled\0".as_ptr());
}


/// COW a shared kernel page table into a fresh private copy.
unsafe fn cow_page_table(shared_pt: *const u32) -> *mut u32 {
    let priv_pt = kalloc() as *mut u32;
    if priv_pt.is_null() { return core::ptr::null_mut(); }
    core::ptr::copy_nonoverlapping(shared_pt, priv_pt, 1024);
    priv_pt
}

//Public api
#[unsafe(no_mangle)]
pub extern "C" fn vmm_map(pd: *mut u32,
                           virtual_addr: u32,
                           physical_addr: u32,
                           flags: i32)
{
    if pd.is_null() { return; }

    if virtual_addr % PAGE_SIZE != 0 || physical_addr % PAGE_SIZE != 0 {
        kprint_str(b"[ERR] vmm_map: addresses not page-aligned\n\0".as_ptr());
        return;
    }

    let pdi = pd_index(virtual_addr) as usize;
    let pti = pt_index(virtual_addr) as usize;
    let mut flags = flags as u32;
    if physical_addr >= PCI_HOLE_START {
        flags |= PAGE_PCD | PAGE_PWT;
    }
    // User stack lives in [USER_STACK_LIMIT, USER_STACK_TOP). A present+user page
    // without R/W faults with #PF err=0x07 on the first push/store (W/R=1, U/S=1).
    if virtual_addr >= USER_STACK_LIMIT
        && virtual_addr < USER_STACK_TOP
        && flags & PAGE_USER != 0
    {
        flags |= PAGE_RW;
    }

    unsafe {
        let pde = &mut *pd.add(pdi);

        if *pde & PAGE_PRESENT == 0 {
            // PDE absent — allocate a fresh private page table.
            let pt = kalloc() as *mut u32;
            if pt.is_null() {
                kprint_str(b"[ERR] vmm_map: kalloc failed for PT\n\0".as_ptr());
                return;
            }
            for i in 0..1024usize { *pt.add(i) = 0; }
            *pde = (pt as u32) | flags | PAGE_PRESENT | PDE_PRIVATE;
        } else if *pde & PDE_PRIVATE == 0 {
            // Shared kernel page table — COW it into a private copy so we
            // never mutate the global kernel PT.
            let shared = (*pde & !0xFFF) as *const u32;
            let priv_pt = cow_page_table(shared);
            if priv_pt.is_null() {
                kprint_str(b"[ERR] vmm_map: COW PT alloc failed\n\0".as_ptr());
                return;
            }
            let old_flags = *pde & 0xFFF;
            *pde = (priv_pt as u32 & !0xFFF)
                | (old_flags | (flags & (PAGE_USER | PAGE_RW)) | PDE_PRIVATE);
        } else {
            // Already a private page table — just propagate permission bits.
            *pde |= flags & (PAGE_USER | PAGE_RW);
        }

        let pt = (*pde & !0xFFF) as *mut u32;
        let old_pte = *pt.add(pti);

        // Release the old COW frame if we're replacing it with a different one.
        if old_pte & PAGE_PRESENT != 0
            && old_pte & PAGE_COW  != 0
            && (old_pte & !0xFFF) != (physical_addr & !0xFFF)
        {
            kfree_page((old_pte & !0xFFF) as *mut u8);
        }

        *pt.add(pti) = (physical_addr & !0xFFF) | flags | PAGE_PRESENT;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn vmm_create_address_space() -> *mut u32 {
    let pd = kalloc() as *mut u32;
    if pd.is_null() { return core::ptr::null_mut(); }

    unsafe {
        // Copy the entire kernel PD as a template.
        // All kernel identity mappings (0 → PCI_HOLE_START) and MMIO entries
        // are inherited as *shared* page tables (no PDE_PRIVATE).  The kernel
        // can therefore always see its own heap and stacks regardless of which
        // process PD is loaded in CR3.
        for i in 0..PD_TOTAL {
            *pd.add(i) = page_directory.0[i];
        }
    }
    pd
}

#[unsafe(no_mangle)]
pub extern "C" fn vmm_free_address_space(pd: *mut u32) {
    if pd.is_null() { return; }

    unsafe {
        for i in 0..PD_TOTAL {
            let pde = *pd.add(i);
            // Only free page tables that were privately allocated for this
            // process.  Shared kernel PTs must never be touched here.
            if pde & PDE_PRIVATE == 0 { continue; }
            if pde & PAGE_PRESENT == 0 { continue; }

            let pt = (pde & !0xFFF) as *mut u32;
            for j in 0..1024usize {
                let pte = *pt.add(j);
                // Free only user pages; kernel identity-mapped frames are
                // managed by the PMM and must not be double-freed.
                if pte & PAGE_PRESENT != 0 && pte & PAGE_USER != 0 {
                    kfree_page((pte & !0xFFF) as *mut u8);
                }
            }
            kfree_page(pt as *mut u8);
        }
        kfree_page(pd as *mut u8);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn vmm_copy_address_space(src_pd: *mut u32, dst_pd: *mut u32) {
    if src_pd.is_null() || dst_pd.is_null() { return; }

    unsafe {
        for i in 0..PD_TOTAL {
            let src_pde = *src_pd.add(i);

            if src_pde & PDE_PRIVATE == 0 {
                // Shared kernel entry — copy the PDE reference, not the data.
                *dst_pd.add(i) = src_pde;
                continue;
            }
            if src_pde & PAGE_PRESENT == 0 { continue; }

            let src_pt = (src_pde & !0xFFF) as *const u32;
            let dst_pt = kalloc() as *mut u32;
            if dst_pt.is_null() { continue; }

            for j in 0..1024usize { *dst_pt.add(j) = 0; }

            for j in 0..1024usize {
                let src_pte = *src_pt.add(j);
                // Only deep-copy present user pages.
                if src_pte & PAGE_PRESENT == 0 || src_pte & PAGE_USER == 0 { continue; }

                let new_page = kalloc();
                if new_page.is_null() { continue; }

                core::ptr::copy_nonoverlapping(
                    (src_pte & !0xFFF) as *const u8,
                    new_page,
                    PAGE_SIZE as usize,
                );
                *dst_pt.add(j) = (new_page as u32 & !0xFFF) | (src_pte & 0xFFF);
            }

            *dst_pd.add(i) = (dst_pt as u32 & !0xFFF) | (src_pde & 0xFFF);
        }
    }
}
