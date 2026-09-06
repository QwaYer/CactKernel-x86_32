//! `mmap` / `munmap` implementation: private PDEs, file-backed mappings, and COW with the VMM.

use crate::ffi::*;
use crate::pmm::{kalloc, free_page, page_ref_inc};
use crate::vmm::paging::{vmm_map, PD_KERNEL_ENTRIES};
use crate::fault::page_fault::vmm_map_zero;
use crate::process::memfd::{memfd_get_page, memfd_grow_to, memfd_map_dec, memfd_map_inc};
use crate::safe::{zero_page, flush_tlb, kprint_str};

fn fd_to_node(fd: i32) -> *mut VfsNode {
    let t = unsafe { *current_task.get() };
    if t.is_null() {
        return core::ptr::null_mut();
    }
    if fd < 0 || fd as usize >= MAX_FD {
        return core::ptr::null_mut();
    }
    unsafe {
        if (*t).proc.is_null() { return core::ptr::null_mut(); }
        let fds = (*(*t).proc).fds;
        if fds.is_null() { return core::ptr::null_mut(); }
        (*fds).fd_table[fd as usize]
    }
}

#[derive(Copy, Clone)]
pub(crate) enum EnsurePteTable {
    Absent,
    Oom,
    /// PCI hole / MMIO PDEs must stay shared with the kernel template — never COW.
    KernelMmio,
}

/// Before changing PTEs in `pd`, the page table for `pdi` must be private
/// (`PDE_PRIVATE`). Otherwise `do_munmap` / `mmap_table_free` / etc. would
/// mutate or `kfree` the global kernel page tables copied into every process.
pub(crate) unsafe fn ensure_pde_private(
    pd: *mut u32,
    pdi: usize,
) -> Result<*mut u32, EnsurePteTable> {
    if pdi >= PD_KERNEL_ENTRIES {
        return Err(EnsurePteTable::KernelMmio);
    }
    let pde = &mut *pd.add(pdi);
    if *pde & PAGE_PRESENT == 0 {
        return Err(EnsurePteTable::Absent);
    }
    if *pde & PDE_PRIVATE != 0 {
        return Ok((*pde & !0xFFF) as *mut u32);
    }
    let shared = (*pde & !0xFFF) as *const u32;
    let priv_pt = kalloc() as *mut u32;
    if priv_pt.is_null() {
        return Err(EnsurePteTable::Oom);
    }
    core::ptr::copy_nonoverlapping(shared, priv_pt, 1024);
    let old_flags = *pde & 0xFFF;
    *pde = (priv_pt as u32 & !0xFFF)
        | (old_flags | PAGE_USER | PAGE_RW | PDE_PRIVATE);
    Ok(priv_pt)
}

fn prot_to_page_flags(prot: i32, user: bool) -> i32 {
    let mut f = PAGE_PRESENT as i32;
    if prot & PROT_WRITE != 0 {
        f |= PAGE_RW as i32;
    }
    if user {
        f |= PAGE_USER as i32;
    }
    f
}

fn find_free_va(tbl: *mut MmapTable, length: u32) -> u32 {
    // SAFETY: tbl is a valid MmapTable (checked by callers).
    let mut candidate = unsafe { (*tbl).next_base };
    candidate = (candidate + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);

    loop {
        if candidate.wrapping_add(length) > MMAP_LIMIT
            || candidate.wrapping_add(length) < candidate
        {
            return 0;
        }

        let mut clash = false;
        // SAFETY: tbl is valid, regions array is within bounds.
        for i in 0..MMAP_MAX_REGIONS {
            let r = unsafe { &(*tbl).regions[i] };
            if r.is_used == 0 {
                continue;
            }
            let r_end = r.base + r.length;
            let c_end = candidate + length;
            if candidate < r_end && c_end > r.base {
                candidate = r_end;
                candidate = (candidate + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
                clash = true;
                break;
            }
        }
        if !clash {
            return candidate;
        }
    }
}

fn alloc_region_slot(tbl: *mut MmapTable) -> *mut MmapRegion {
    // SAFETY: tbl is valid.
    for i in 0..MMAP_MAX_REGIONS {
        let r = unsafe { &mut (*tbl).regions[i] };
        if r.is_used == 0 {
            return r;
        }
    }
    core::ptr::null_mut()
}

/// Present physical frame backing `va` in `pd`, or 0 if absent.
fn pte_phys(pd: *mut u32, va: u32) -> u32 {
    // SAFETY: pd is valid.
    let pde = unsafe { *pd.add(pd_index(va) as usize) };
    if pde & PAGE_PRESENT == 0 {
        return 0;
    }
    let pt = (pde & !0xFFF) as *const u32;
    // SAFETY: pt is a valid page table.
    let pte = unsafe { *pt.add(pt_index(va) as usize) };
    if pte & PAGE_PRESENT == 0 {
        return 0;
    }
    pte & !0xFFF
}

/// Drop the PTE for a user virtual address, releasing its frame reference.
fn clear_user_pte(pd: *mut u32, va: u32) {
    // SAFETY: pd is valid.
    let pde = unsafe { *pd.add(pd_index(va) as usize) };
    if pde & PAGE_PRESENT == 0 {
        return;
    }
    let pt = (pde & !0xFFF) as *mut u32;
    // SAFETY: pt is a valid page table.
    let pte = unsafe { *pt.add(pt_index(va) as usize) };
    if pte & PAGE_PRESENT != 0 && pte & PAGE_USER != 0 {
        free_page((pte & !0xFFF) as *mut u8);
    }
    // SAFETY: pt is a valid page table.
    unsafe { *pt.add(pt_index(va) as usize) = 0; }
    flush_tlb(va);
}

#[unsafe(no_mangle)]
pub extern "C" fn mmap_table_init(tbl: *mut MmapTable) {
    if tbl.is_null() {
        return;
    }
    // SAFETY: tbl is valid.
    unsafe {
        for i in 0..MMAP_MAX_REGIONS {
            (*tbl).regions[i].is_used = 0;
            (*tbl).regions[i].fd = -1;
            (*tbl).regions[i].shobj = 0;
        }
        (*tbl).next_base = MMAP_BASE;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn mmap_find_region(tbl: *mut MmapTable, addr: u32) -> *mut MmapRegion {
    if tbl.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: tbl is valid.
    for i in 0..MMAP_MAX_REGIONS {
        let r = unsafe { &mut (*tbl).regions[i] };
        if r.is_used == 0 {
            continue;
        }
        if addr >= r.base && addr < r.base + r.length {
            return r;
        }
    }
    core::ptr::null_mut()
}

#[unsafe(no_mangle)]
pub extern "C" fn do_mmap(
    pd: *mut u32,
    tbl: *mut MmapTable,
    hint: u32,
    mut length: u32,
    prot: i32,
    flags: i32,
    fd: i32,
    offset: u32,
) -> *mut u8 {
    if pd.is_null() || tbl.is_null() || length == 0 {
        return MAP_FAILED as *mut u8;
    }
    if (flags & MAP_SHARED == 0) && (flags & MAP_PRIVATE == 0) {
        return MAP_FAILED as *mut u8;
    }

    length = (length + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);

    let va;
    if (flags & MAP_FIXED != 0) && hint != 0 {
        if hint % PAGE_SIZE != 0 {
            return MAP_FAILED as *mut u8;
        }
        if hint >= USER_STACK_TOP || hint.saturating_add(length) > USER_STACK_TOP {
            return MAP_FAILED as *mut u8;
        }
        va = hint;
    } else {
        va = find_free_va(tbl, length);
        if va == 0 {
            kprint_str(b"[MMAP] do_mmap: no free virtual address space\n\0".as_ptr());
            return MAP_FAILED as *mut u8;
        }
    }

    let region = alloc_region_slot(tbl);
    if region.is_null() {
        kprint_str(b"[MMAP] do_mmap: region table full\n\0".as_ptr());
        return MAP_FAILED as *mut u8;
    }

    let page_flags = prot_to_page_flags(prot, true);
    let pages = length / PAGE_SIZE;

    // memfd backing handle for the mapped fd (0 = not a memfd mapping).
    let mut shobj: i32 = 0;

    if flags & MAP_ANON != 0 {
        if vmm_map_zero(pd, va, length, page_flags) != 0 {
            kprint_str(b"[MMAP] do_mmap: vmm_map_zero failed\n\0".as_ptr());
            return MAP_FAILED as *mut u8;
        }
    } else {
        if fd < 0 {
            return MAP_FAILED as *mut u8;
        }
        // SAFETY: memfd_fd_handle is a C helper that only inspects the fd table.
        if flags & MAP_SHARED != 0 {
            shobj = unsafe { memfd_fd_handle(fd) };
        }

        if shobj > 0 && flags & MAP_SHARED != 0 {
            // Shared memfd mapping: map the object's own frames so that fd I/O,
            // truncate, fork, and other MAP_SHARED mappings see one storage.
            if offset % PAGE_SIZE != 0 {
                return MAP_FAILED as *mut u8;
            }
            if memfd_map_inc(shobj) != 0 {
                return MAP_FAILED as *mut u8;
            }
            let first_page = offset / PAGE_SIZE;
            if memfd_grow_to(shobj, offset + length) != 0 {
                memfd_map_dec(shobj);
                return MAP_FAILED as *mut u8;
            }
            let mut installed = 0u32;
            while installed < pages {
                let page = memfd_get_page(shobj, first_page + installed);
                if page.is_null() {
                    break;
                }
                let va_i = va + installed * PAGE_SIZE;
                vmm_map(pd, va_i, page as u32, page_flags);
                if pte_phys(pd, va_i) != page as u32 {
                    break;
                }
                page_ref_inc(page);
                installed += 1;
            }
            if installed < pages {
                for m in 0..installed {
                    clear_user_pte(pd, va + m * PAGE_SIZE);
                }
                memfd_map_dec(shobj);
                kprint_str(b"[MMAP] do_mmap: memfd shared map failed\n\0".as_ptr());
                return MAP_FAILED as *mut u8;
            }
        } else if shobj > 0 {
            // MAP_PRIVATE over a memfd: take a private snapshot copy.
            if offset % PAGE_SIZE != 0 {
                return MAP_FAILED as *mut u8;
            }
            for i in 0..pages {
                let phys = kalloc();
                if phys.is_null() {
                    for m in 0..i {
                        clear_user_pte(pd, va + m * PAGE_SIZE);
                    }
                    return MAP_FAILED as *mut u8;
                }
                zero_page(phys);
                let src = memfd_get_page(shobj, offset / PAGE_SIZE + i);
                if !src.is_null() {
                    // SAFETY: both page pointers are valid 4 KiB frames.
                    unsafe { core::ptr::copy_nonoverlapping(src, phys, PAGE_SIZE as usize); }
                }
                vmm_map(pd, va + i * PAGE_SIZE, phys as u32, page_flags);
            }
        } else {
            let node = fd_to_node(fd);
            let mut file_off = offset;

            for i in 0..pages {
                let phys = kalloc();
                if phys.is_null() {
                    do_munmap(pd, tbl, va, i * PAGE_SIZE);
                    return MAP_FAILED as *mut u8;
                }
                zero_page(phys);
                if !node.is_null() {
                    // SAFETY: node is a valid VfsNode.
                    unsafe { read_vfs(node, file_off, PAGE_SIZE, phys); }
                }
                vmm_map(pd, va + i * PAGE_SIZE, phys as u32, page_flags);
                file_off += PAGE_SIZE;
            }
        }
    }

    let shared_backed = shobj > 0 && flags & MAP_SHARED != 0;

    // SAFETY: region is a valid slot we just allocated.
    unsafe {
        (*region).base = va;
        (*region).length = length;
        (*region).flags = flags as u32;
        (*region).prot = prot as u32;
        (*region).fd = if flags & MAP_ANON != 0 { -1 } else { fd };
        (*region).file_off = offset;
        (*region).is_used = 1;
        (*region).shobj = if shared_backed { shobj } else { 0 };
    }

    // SAFETY: tbl is valid.
    if va + length > unsafe { (*tbl).next_base } {
        unsafe { (*tbl).next_base = va + length; }
    }

    va as *mut u8
}

#[unsafe(no_mangle)]
pub extern "C" fn do_munmap(
    pd: *mut u32,
    tbl: *mut MmapTable,
    addr: u32,
    mut length: u32,
) -> i32 {
    if pd.is_null() || tbl.is_null() || length == 0 {
        return -1;
    }
    if addr % PAGE_SIZE != 0 {
        return -1;
    }

    length = (length + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);

    let mut region: *mut MmapRegion = core::ptr::null_mut();
    // SAFETY: tbl is valid.
    for i in 0..MMAP_MAX_REGIONS {
        let r = unsafe { &mut (*tbl).regions[i] };
        if r.is_used == 0 {
            continue;
        }
        if r.base == addr && r.length == length {
            region = r;
            break;
        }
        if r.base <= addr && addr + length <= r.base + r.length {
            region = r;
            break;
        }
    }

    if region.is_null() {
        return -1;
    }

    let pages = length / PAGE_SIZE;
    for i in 0..pages {
        let va = addr + i * PAGE_SIZE;
        let pdi = pd_index(va) as usize;
        let pde_val = unsafe { *pd.add(pdi) };
        if pde_val & PAGE_PRESENT == 0 {
            continue;
        }
        let pt = match unsafe { ensure_pde_private(pd, pdi) } {
            Ok(p) => p,
            Err(EnsurePteTable::Absent) | Err(EnsurePteTable::KernelMmio) => continue,
            Err(EnsurePteTable::Oom) => return -1,
        };
        // SAFETY: pt is a valid private page table.
        let pte = unsafe { *pt.add(pt_index(va) as usize) };

        // Only release frames that were allocated for user mappings. Supervisor
        // identity PTEs (present, no PAGE_USER) must not be passed to free_page.
        if pte & PAGE_PRESENT != 0 && pte & PAGE_USER != 0 {
            free_page((pte & !0xFFF) as *mut u8);
        }
        unsafe { *pt.add(pt_index(va) as usize) = 0; }
        flush_tlb(va);
    }

    // SAFETY: region is valid.
    unsafe {
        if addr == (*region).base && length >= (*region).length {
            let obj = (*region).shobj;
            (*region).is_used = 0;
            (*region).fd = -1;
            (*region).shobj = 0;
            if obj > 0 {
                memfd_map_dec(obj);
            }
        } else if addr == (*region).base {
            (*region).base += length;
            (*region).length -= length;
            (*region).file_off += length;
        } else {
            (*region).length = addr - (*region).base;
        }
    }

    0
}

#[unsafe(no_mangle)]
pub extern "C" fn do_mprotect(
    pd: *mut u32,
    tbl: *mut MmapTable,
    addr: u32,
    mut length: u32,
    prot: i32,
    brk_start: u32,
    brk_end: u32,
) -> i32 {
    if pd.is_null() || tbl.is_null() || length == 0 {
        return -1;
    }
    if addr % PAGE_SIZE != 0 {
        return -1;
    }

    length = (length + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
    let in_brk = addr >= brk_start && addr < brk_end;

    let region = mmap_find_region(tbl, addr);
    if region.is_null() && !in_brk {
        return -1;
    }

    let page_flags = prot_to_page_flags(prot, true) as u32;
    let pages = length / PAGE_SIZE;

    for i in 0..pages {
        let va = addr + i * PAGE_SIZE;
        let pdi = pd_index(va) as usize;
        let pde_val = unsafe { *pd.add(pdi) };
        if pde_val & PAGE_PRESENT == 0 {
            continue;
        }
        let pt = match unsafe { ensure_pde_private(pd, pdi) } {
            Ok(p) => p,
            Err(EnsurePteTable::Absent) | Err(EnsurePteTable::KernelMmio) => continue,
            Err(EnsurePteTable::Oom) => return -1,
        };
        // SAFETY: pt is valid.
        let pte = unsafe { *pt.add(pt_index(va) as usize) };
        if pte & PAGE_PRESENT == 0 {
            continue;
        }
        unsafe { *pt.add(pt_index(va) as usize) = (pte & !0xFFF) | page_flags; }
        flush_tlb(va);
    }

    if !in_brk {
        // SAFETY: region is valid (non-brk path).
        unsafe { (*region).prot = prot as u32; }
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn mmap_handle_fault(
    pd: *mut u32,
    tbl: *mut MmapTable,
    fault_addr: u32,
) -> i32 {
    if pd.is_null() || tbl.is_null() {
        return -1;
    }

    let region = mmap_find_region(tbl, fault_addr);
    if region.is_null() {
        return -1;
    }

    let page_va = fault_addr & !0xFFF;
    let pdi = pd_index(page_va) as usize;
    let pti = pt_index(page_va) as usize;

    let pde_val = unsafe { *pd.add(pdi) };
    if pde_val & PAGE_PRESENT == 0 {
        return -1;
    }
    let pt = match unsafe { ensure_pde_private(pd, pdi) } {
        Ok(p) => p,
        Err(EnsurePteTable::Absent) | Err(EnsurePteTable::KernelMmio) => return -1,
        Err(EnsurePteTable::Oom) => return -1,
    };

    // SAFETY: pt is valid.
    let pte = unsafe { *pt.add(pti) };
    if pte & PAGE_PRESENT != 0 {
        return -1;
    }

    let phys = kalloc();
    if phys.is_null() {
        return -1;
    }
    zero_page(phys);

    // SAFETY: region is valid.
    let fd = unsafe { (*region).fd };
    if fd >= 0 {
        let page_offset = page_va - unsafe { (*region).base };
        let file_off = unsafe { (*region).file_off } + page_offset;
        let node = fd_to_node(fd);
        if !node.is_null() {
            // SAFETY: node is a valid VfsNode.
            unsafe { read_vfs(node, file_off, PAGE_SIZE, phys); }
        }
    }

    let page_flags = prot_to_page_flags(unsafe { (*region).prot as i32 }, true) as u32;
    // SAFETY: pt is valid.
    unsafe { *pt.add(pti) = (phys as u32 & !0xFFF) | page_flags; }
    flush_tlb(page_va);
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn mmap_print_regions(tbl: *const MmapTable) {
    if tbl.is_null() {
        return;
    }
    let mut buf = [0u8; 16];
    kprint_str(b"[MMAP] === Memory Regions ===\n\0".as_ptr());
    // SAFETY: tbl is valid.
    for i in 0..MMAP_MAX_REGIONS {
        let r = unsafe { &(*tbl).regions[i] };
        if r.is_used == 0 {
            continue;
        }
        kprint_str(b"  [\0".as_ptr());
        // SAFETY: itoa/hex_to_ascii require a valid buffer.
        unsafe {
            itoa(i as i32, buf.as_mut_ptr());
            printk(buf.as_ptr());
        }
        kprint_str(b"] base=0x\0".as_ptr());
        unsafe {
            hex_to_ascii(r.base, buf.as_mut_ptr());
            printk(buf.as_ptr());
        }
        kprint_str(b" len=0x\0".as_ptr());
        unsafe {
            hex_to_ascii(r.length, buf.as_mut_ptr());
            printk(buf.as_ptr());
        }
        kprint_str(b" prot=\0".as_ptr());
        unsafe {
            itoa(r.prot as i32, buf.as_mut_ptr());
            printk(buf.as_ptr());
        }
        kprint_str(b" flags=\0".as_ptr());
        unsafe {
            itoa(r.flags as i32, buf.as_mut_ptr());
            printk(buf.as_ptr());
        }
        kprint_str(b" fd=\0".as_ptr());
        unsafe {
            itoa(r.fd, buf.as_mut_ptr());
            printk(buf.as_ptr());
        }
        kprint_str(b"\n\0".as_ptr());
    }
}
#[path = "mmap_clone.rs"]
mod mmap_clone;
pub use mmap_clone::*;
