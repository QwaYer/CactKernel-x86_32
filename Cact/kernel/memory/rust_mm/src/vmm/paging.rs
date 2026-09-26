//! Kernel page directory bootstrap, linear map of RAM, per-process maps, and C-exported VMM helpers.

use crate::ffi::*;
use crate::safe::kprint_str;
use crate::pmm::{kalloc, free_page};

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
    // SAFETY: `addr_of_mut!` projects the address of the first PDE without ever
    // forming a reference to the `static mut`; `page_directory` is a
    // kernel-lifetime global initialised during boot and never moved.
    unsafe { core::ptr::addr_of_mut!(page_directory.0).cast::<u32>() }
}

/// # Safety
///
/// `pd` must be null or a valid page directory that the caller may mutate and
/// that no other thread modifies concurrently.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_sync_kernel_mmio_mappings(pd: *mut u32) {
    if pd.is_null() {
        return;
    }
    // SAFETY: `addr_of!` forms a pointer to `page_directory.0` (a live,
    // 4 KiB-aligned `[u32; PD_TOTAL]`) without creating a reference.
    let src_ptr = unsafe { core::ptr::addr_of!(page_directory.0).cast::<u32>() };
    if core::ptr::eq(pd as *const u32, src_ptr) {
        // Syncing the kernel page directory onto itself is a no-op (and the two
        // slices below would then alias).
        return;
    }
    // SAFETY: `pd` is a valid page directory (checked non-null, and distinct from
    // the kernel template by the test above) spanning `PD_TOTAL` entries, so this
    // exclusive slice is valid for the call.
    let dst = unsafe { core::slice::from_raw_parts_mut(pd, PD_TOTAL) };
    // SAFETY: `src_ptr` points at the live kernel page directory (`PD_TOTAL`
    // entries); the shared slice does not overlap `dst`.
    let src = unsafe { core::slice::from_raw_parts(src_ptr, PD_TOTAL) };
    // Keep high MMIO/PCI-hole PDEs consistent in every process PD so interrupt
    // handlers can touch device registers under any CR3.
    dst[PD_KERNEL_ENTRIES..].copy_from_slice(&src[PD_KERNEL_ENTRIES..]);
}

#[unsafe(no_mangle)]
pub extern "C" fn init_paging() {
    // `init_paging` runs once at boot before scheduling; `PAGE_TABLES` and
    // `page_directory` are distinct 4 KiB-aligned globals spanning `PD_TOTAL`
    // entries each.
    // SAFETY: `addr_of_mut!` forms a pointer to `PAGE_TABLES.0` without creating
    // a reference; this is the only accessor during single-threaded boot.
    let tables_ptr = unsafe { core::ptr::addr_of_mut!(PAGE_TABLES.0) };
    // SAFETY: `tables_ptr` addresses the live `PAGE_TABLES` global, so this
    // exclusive borrow of the whole table array is valid.
    let tables = unsafe { &mut *tables_ptr };
    // SAFETY: `addr_of_mut!` forms a pointer to `page_directory.0` without
    // creating a reference.
    let pd_ptr = unsafe { core::ptr::addr_of_mut!(page_directory.0) };
    // SAFETY: `pd_ptr` addresses the live kernel page directory, so this exclusive
    // borrow is valid and does not overlap `tables`.
    let pd = unsafe { &mut *pd_ptr };

    for (pt_idx, pt) in tables.iter_mut().enumerate() {
        for (page, slot) in pt.iter_mut().enumerate() {
            let phys: u32 = ((pt_idx * 1024 + page) as u32).wrapping_mul(PAGE_SIZE);

            // Choose cache policy based on physical address.
            let cache_flags: u32 = if phys >= PCI_HOLE_START {
                // MMIO / PCI hole: uncacheable, write-through.
                PAGE_PCD | PAGE_PWT
            } else {
                // Normal RAM: default write-back caching.
                0
            };

            *slot = phys | PAGE_PRESENT | PAGE_RW | cache_flags;
        }
        // Point PD entry at this page table.
        // PD entries themselves do NOT need PCD/PWT — only PTEs do.
        pd[pt_idx] = (pt.as_ptr() as u32) | PAGE_PRESENT | PAGE_RW;
    }

    // SAFETY: `addr_of_mut!` forms a pointer to the live kernel page directory
    // without creating a reference.
    let pd = unsafe { core::ptr::addr_of_mut!(page_directory.0).cast::<u32>() };
    // SAFETY: `pd` is the freshly built kernel page directory; loading it here is
    // the one-time, single-threaded boot sequence.
    unsafe { load_page_directory(pd) };
    // SAFETY: as above — paging is enabled once, immediately after the PD is
    // loaded.
    unsafe { enable_paging() };
}


/// COW a shared kernel page table into a fresh private copy.
///
/// # Safety
/// `shared_pt` must point to a live, fully populated 1024-entry page table
/// (typically a shared kernel PDE target) that stays valid for the copy.
unsafe fn cow_page_table(shared_pt: *const u32) -> *mut u32 {
    let priv_pt = kalloc() as *mut u32;
    if priv_pt.is_null() { return core::ptr::null_mut(); }
    // SAFETY: per the contract above `shared_pt` is a live 1024-entry table and
    // `priv_pt` is a fresh 4 KiB kalloc block, so the copy is in bounds.
    unsafe { core::ptr::copy_nonoverlapping(shared_pt, priv_pt, 1024); }
    priv_pt
}

// Public API: C-exported VMM mapping helpers.

/// # Safety
///
/// `pd` must be null or a valid page directory that the caller owns and may
/// mutate; the caller must serialise concurrent mapping changes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_map(pd: *mut u32,
                           virtual_addr: u32,
                           physical_addr: u32,
                           flags: i32)
{
    if pd.is_null() { return; }

    if !virtual_addr.is_multiple_of(PAGE_SIZE) || !physical_addr.is_multiple_of(PAGE_SIZE) {
        kprint_str(c"[ERR] vmm_map: addresses not page-aligned\n".as_ptr() as *const u8);
        return;
    }

    let pdi = pd_index(virtual_addr) as usize;
    let pti = pt_index(virtual_addr) as usize;
    let mut flags = flags as u32;
    let caller_uncached = (flags & (PAGE_PCD | PAGE_PWT)) != 0;
    if physical_addr >= PCI_HOLE_START {
        flags |= PAGE_PCD | PAGE_PWT;
    }
    // User stack lives in [USER_STACK_LIMIT, USER_STACK_TOP). A present+user page
    // without R/W faults with #PF err=0x07 on the first push/store (W/R=1, U/S=1).
    if (USER_STACK_LIMIT..USER_STACK_TOP).contains(&virtual_addr)
        && flags & PAGE_USER != 0
    {
        flags |= PAGE_RW;
    }

    // Upper half (PCI hole / MMIO) uses the same page tables as the kernel
    // identity map in every address space.  Never COW those PDEs for a user
    // PD: drivers may call vmm_map(get_current_pd(), bar_va, ...) and a
    // private copy would diverge from the kernel template, break framebuffer
    // under process CR3, and leak (vmm_free_address_space skips i >= PD_KERNEL_ENTRIES).
    //
    // An uncacheable mapping is device MMIO wherever it lives: firmware may
    // place a BAR *below* PCI_HOLE_START, inside what this kernel treats as the
    // RAM window (the xHCI on this HP sits at 0xa1200000).  Such a mapping must
    // be global too, otherwise it ends up in a per-process page table, and an
    // interrupt handler running under a user CR3 faults on the next register
    // access.
    let is_kernel_mmio = virtual_addr >= PCI_HOLE_START || caller_uncached;

    // SAFETY: `pd` is a valid page directory (checked non-null); `pd_index`/
    // `pt_index` mask to 10 bits and the slice spans `PD_TOTAL` entries, so every
    // PDE index below is in bounds.
    let pd_slice = unsafe { core::slice::from_raw_parts_mut(pd, PD_TOTAL) };
    {
        let pde = &mut pd_slice[pdi];

        if *pde & PAGE_PRESENT == 0 {
            // PDE absent — allocate a fresh private page table.
            let pt = kalloc() as *mut u32;
            if pt.is_null() {
                kprint_str(c"[ERR] vmm_map: kalloc failed for PT\n".as_ptr() as *const u8);
                return;
            }
            // SAFETY: `pt` is the fresh `kalloc` page table just checked non-null;
            // the slice spans its 1024 entries and is exclusive.
            let pt_slice = unsafe { core::slice::from_raw_parts_mut(pt, 1024) };
            pt_slice.fill(0);
            if is_kernel_mmio {
                // `pde` is the in-bounds PDE slot for this mapping; `pt` is the
                // fresh page table recorded in it.
                *pde = (pt as u32) | flags | PAGE_PRESENT;
            } else {
                // As above, additionally marking the table private.
                *pde = (pt as u32) | flags | PAGE_PRESENT | PDE_PRIVATE;
            }
        } else if *pde & PDE_PRIVATE == 0 {
            if is_kernel_mmio {
                // Kernel MMIO mappings must stay globally shared across all
                // process PDs; do not COW these page tables into private ones.
                *pde |= flags & (PAGE_USER | PAGE_RW);
                let pt = (*pde & !0xFFF) as *mut u32;
                // SAFETY: `pt` is the live page table named by the present PDE and
                // `pti < 1024`, so this entry pointer is in bounds.
                let old_entry = unsafe { pt.add(pti) };
                // SAFETY: `old_entry` points at one initialised PTE.
                let old_pte = unsafe { *old_entry };
                if old_pte & PAGE_PRESENT != 0
                    && old_pte & PAGE_COW  != 0
                    && (old_pte & !0xFFF) != (physical_addr & !0xFFF)
                {
                    // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
                    unsafe { free_page((old_pte & !0xFFF) as *mut u8) };
                }
                // SAFETY: `old_entry` points at one PTE, which receives the mapping.
                unsafe { *old_entry = (physical_addr & !0xFFF) | flags | PAGE_PRESENT };
            } else {
                // Shared kernel page table — COW it into a private copy so we
                // never mutate the global kernel PT.
                let shared = (*pde & !0xFFF) as *const u32;
                // SAFETY: `shared` is the live shared kernel page table named by the
                // present PDE, which `cow_page_table`'s contract requires.
                let priv_pt = unsafe { cow_page_table(shared) };
                if priv_pt.is_null() {
                    kprint_str(c"[ERR] vmm_map: COW PT alloc failed\n".as_ptr() as *const u8);
                    return;
                }
                let old_flags = *pde & 0xFFF;
                *pde = (priv_pt as u32 & !0xFFF)
                    | (old_flags | (flags & (PAGE_USER | PAGE_RW)) | PDE_PRIVATE);
            }
        } else {
            // Already a private page table — just propagate permission bits.
            *pde |= flags & (PAGE_USER | PAGE_RW);
        }

        let pt = (*pde & !0xFFF) as *mut u32;
        // SAFETY: `pt` is the live page table named by the present PDE and
        // `pti < 1024`, so this entry pointer is in bounds.
        let entry = unsafe { pt.add(pti) };
        // SAFETY: `entry` points at one initialised PTE.
        let old_pte = unsafe { *entry };

        // Release the old COW frame if we're replacing it with a different one.
        if old_pte & PAGE_PRESENT != 0
            && old_pte & PAGE_COW  != 0
            && (old_pte & !0xFFF) != (physical_addr & !0xFFF)
        {
            // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
            unsafe { free_page((old_pte & !0xFFF) as *mut u8) };
        }

        // SAFETY: `entry` points at one PTE, which receives the mapping.
        unsafe { *entry = (physical_addr & !0xFFF) | flags | PAGE_PRESENT };
    }

    // If a new kernel/MMIO mapping is added to the kernel template, mirror it
    // into the currently active PD as well so already-running user tasks do
    // not fault inside IRQ context before the next scheduler switch.
    if pd == get_kernel_pd() && virtual_addr >= PCI_HOLE_START {
        let active = crate::safe::current_page_dir();
        if !active.is_null() && active != pd {
            // SAFETY: `active` is the page directory currently loaded in CR3,
            // which this CPU owns at this moment.
            unsafe { vmm_sync_kernel_mmio_mappings(active); }
        }
    }
}

/// Translate a virtual address to physical using `pd` (page directory).
/// If `pd` is null, the kernel page directory is used.
/// Returns 0 if the mapping is not present. Page offset bits are preserved.
#[unsafe(no_mangle)]
pub extern "C" fn vmm_get_phys(pd: *mut u32, virtual_addr: u32) -> u32 {
    let pd = if pd.is_null() {
        get_kernel_pd()
    } else {
        pd
    };
    // SAFETY: `pd` is a valid page directory (defaulted to the kernel PD) spanning
    // `PD_TOTAL` entries, and `pd_index` masks to 10 bits, so this shared slice is
    // in bounds for the whole translation.
    let pd = unsafe { core::slice::from_raw_parts(pd, PD_TOTAL) };
    let pdi = pd_index(virtual_addr) as usize;
    let pti = pt_index(virtual_addr) as usize;
    let pde = pd[pdi];
    if pde & PAGE_PRESENT == 0 {
        return 0;
    }
    let pt = (pde & !0xFFF) as *const u32;
    // SAFETY: `pt` is the live page table named by the present PDE and
    // `pt_index` masks to 10 bits, so this entry pointer is in bounds.
    let pte_entry = unsafe { pt.add(pti) };
    // SAFETY: `pte_entry` points at one initialised PTE.
    let pte = unsafe { *pte_entry };
    if pte & PAGE_PRESENT == 0 {
        return 0;
    }
    (pte & !0xFFF) | (virtual_addr & 0xFFF)
}

#[unsafe(no_mangle)]
pub extern "C" fn vmm_create_address_space() -> *mut u32 {
    let pd = kalloc() as *mut u32;
    if pd.is_null() { return core::ptr::null_mut(); }

    // SAFETY: `addr_of!` forms a pointer to `page_directory.0` (a live,
    // 4 KiB-aligned `[u32; PD_TOTAL]`) without creating a reference.
    let src_ptr = unsafe { core::ptr::addr_of!(page_directory.0).cast::<u32>() };
    // SAFETY: `pd` is a freshly kalloc'd, page-aligned, non-null directory
    // spanning `PD_TOTAL` entries; this slice is exclusive and cannot overlap the
    // kernel template (a distinct global).
    let dst = unsafe { core::slice::from_raw_parts_mut(pd, PD_TOTAL) };
    // SAFETY: `src_ptr` points at the live kernel page directory (`PD_TOTAL`
    // entries); the shared slice does not overlap `dst`.
    let src = unsafe { core::slice::from_raw_parts(src_ptr, PD_TOTAL) };
    // Copy the entire kernel PD as a template.
    // All kernel identity mappings (0 → PCI_HOLE_START) and MMIO entries are
    // inherited as *shared* page tables (no PDE_PRIVATE).  The kernel can
    // therefore always see its own heap and stacks regardless of which process PD
    // is loaded in CR3.
    dst.copy_from_slice(src);
    pd
}

/// # Safety
///
/// `pd` must be null or a valid page directory previously returned by
/// `vmm_create_address_space`; it must not be used again after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_free_address_space(pd: *mut u32) {
    if pd.is_null() { return; }

    // SAFETY: `pd` is a valid page directory owned by the caller spanning
    // `PD_TOTAL` entries, and `i` stays below `PD_KERNEL_ENTRIES`, so this shared
    // slice is in bounds for the whole teardown.
    let pd_slice = unsafe { core::slice::from_raw_parts(pd, PD_TOTAL) };
    // Free only user-space PDEs; kernel/MMIO half is globally shared.
    for slot in &pd_slice[..PD_KERNEL_ENTRIES] {
        let pde = *slot;
        // Only free page tables that were privately allocated for this process.
        // Shared kernel PTs must never be touched here.
        if pde & PDE_PRIVATE == 0 { continue; }
        if pde & PAGE_PRESENT == 0 { continue; }

        let pt = (pde & !0xFFF) as *mut u32;
        for j in 0..1024usize {
            // SAFETY: `pt` is a private page table of this directory and `j < 1024`,
            // so this entry pointer is in bounds.
            let pte_entry = unsafe { pt.add(j) };
            // SAFETY: `pte_entry` points at one initialised PTE.
            let pte = unsafe { *pte_entry };
            // Free only user pages; kernel identity-mapped frames are managed by
            // the PMM and must not be double-freed.
            if pte & PAGE_PRESENT != 0 && pte & PAGE_USER != 0 {
                // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
                unsafe { free_page((pte & !0xFFF) as *mut u8) };
            }
        }
        // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
        unsafe { free_page(pt as *mut u8) };
    }
    // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
    unsafe { free_page(pd as *mut u8) };
}

/// # Safety
///
/// `src_pd` and `dst_pd` must be null or valid page directories owned by the
/// caller, and neither may be mutated concurrently for the duration of the
/// call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_copy_address_space(src_pd: *mut u32, dst_pd: *mut u32) {
    if src_pd.is_null() || dst_pd.is_null() { return; }

    // SAFETY: `src_pd`/`dst_pd` are valid, distinct page directories (checked
    // non-null) spanning `PD_TOTAL` entries each, so these slices are in bounds
    // and do not overlap.
    let src_pd = unsafe { core::slice::from_raw_parts(src_pd, PD_TOTAL) };
    // SAFETY: as above, for the destination directory, which this call writes.
    let dst_pd = unsafe { core::slice::from_raw_parts_mut(dst_pd, PD_TOTAL) };
    for i in 0..PD_TOTAL {
        let src_pde = src_pd[i];

        if src_pde & PDE_PRIVATE == 0 {
            // Shared kernel entry — copy the PDE reference, not the data.
            dst_pd[i] = src_pde;
            continue;
        }
        if src_pde & PAGE_PRESENT == 0 { continue; }

        let src_pt = (src_pde & !0xFFF) as *const u32;
        let dst_pt = kalloc() as *mut u32;
        if dst_pt.is_null() { continue; }

        // SAFETY: `dst_pt` is the fresh `kalloc` page table just checked
        // non-null; the slice spans its 1024 entries and is exclusive.
        let dst_pt_slice = unsafe { core::slice::from_raw_parts_mut(dst_pt, 1024) };
        dst_pt_slice.fill(0);

        for (j, dst_entry) in dst_pt_slice.iter_mut().enumerate() {
            // SAFETY: `src_pt` is the live page table of the present private source
            // PDE and `j < 1024`, so this entry pointer is in bounds.
            let src_pte_entry = unsafe { src_pt.add(j) };
            // SAFETY: `src_pte_entry` points at one initialised PTE.
            let src_pte = unsafe { *src_pte_entry };
            // Only deep-copy present user pages.
            if src_pte & PAGE_PRESENT == 0 || src_pte & PAGE_USER == 0 { continue; }

            let new_page = kalloc();
            if new_page.is_null() { continue; }

            // SAFETY: `src_pte` names a present frame and `new_page` is a fresh
            // `kalloc` page, so the 4 KiB copy is in bounds.
            unsafe {
                core::ptr::copy_nonoverlapping(
                    (src_pte & !0xFFF) as *const u8,
                    new_page,
                    PAGE_SIZE as usize,
                );
            }
            *dst_entry = (new_page as u32 & !0xFFF) | (src_pte & 0xFFF);
        }

        dst_pd[i] = (dst_pt as u32 & !0xFFF) | (src_pde & 0xFFF);
    }
}
