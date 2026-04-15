use crate::ffi::*;
use crate::pmm::{kalloc, kfree_page};
use crate::vmm::paging::vmm_map;
use crate::fault::page_fault::vmm_map_zero;

unsafe fn fd_to_node(fd: i32) -> *mut VfsNode {
    if current_task.is_null() {
        return core::ptr::null_mut();
    }
    if fd < 0 || fd as usize >= MAX_FD {
        return core::ptr::null_mut();
    }
    (*current_task).fd_table[fd as usize]
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

unsafe fn find_free_va(tbl: *mut MmapTable, length: u32) -> u32 {
    let mut candidate = (*tbl).next_base;
    candidate = (candidate + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);

    loop {
        if candidate.wrapping_add(length) > MMAP_LIMIT
            || candidate.wrapping_add(length) < candidate
        {
            return 0;
        }

        let mut clash = false;
        for i in 0..MMAP_MAX_REGIONS {
            let r = &(*tbl).regions[i];
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

unsafe fn alloc_region_slot(tbl: *mut MmapTable) -> *mut MmapRegion {
    for i in 0..MMAP_MAX_REGIONS {
        if (*tbl).regions[i].is_used == 0 {
            return &mut (*tbl).regions[i];
        }
    }
    core::ptr::null_mut()
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mmap_table_init(tbl: *mut MmapTable) {
    if tbl.is_null() {
        return;
    }
    for i in 0..MMAP_MAX_REGIONS {
        (*tbl).regions[i].is_used = 0;
        (*tbl).regions[i].fd = -1;
    }
    (*tbl).next_base = MMAP_BASE;
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mmap_find_region(tbl: *mut MmapTable, addr: u32) -> *mut MmapRegion {
    if tbl.is_null() {
        return core::ptr::null_mut();
    }
    for i in 0..MMAP_MAX_REGIONS {
        let r = &mut (*tbl).regions[i];
        if r.is_used == 0 {
            continue;
        }
        if addr >= r.base && addr < r.base + r.length {
            return r;
        }
    }
    core::ptr::null_mut()
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn do_mmap(
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
        va = hint;
    } else {
        va = find_free_va(tbl, length);
        if va == 0 {
            kprint(b"[MMAP] do_mmap: no free virtual address space\n\0".as_ptr());
            return MAP_FAILED as *mut u8;
        }
    }

    let region = alloc_region_slot(tbl);
    if region.is_null() {
        kprint(b"[MMAP] do_mmap: region table full\n\0".as_ptr());
        return MAP_FAILED as *mut u8;
    }

    let page_flags = prot_to_page_flags(prot, true);
    let pages = length / PAGE_SIZE;

    if flags & MAP_ANON != 0 {
        if vmm_map_zero(pd, va, length, page_flags) != 0 {
            kprint(b"[MMAP] do_mmap: vmm_map_zero failed\n\0".as_ptr());
            return MAP_FAILED as *mut u8;
        }
    } else {
        if fd < 0 {
            return MAP_FAILED as *mut u8;
        }
        let node = fd_to_node(fd);
        let mut file_off = offset;

        for i in 0..pages {
            let phys = kalloc();
            if phys.is_null() {
                do_munmap(pd, tbl, va, i * PAGE_SIZE);
                return MAP_FAILED as *mut u8;
            }
            core::ptr::write_bytes(phys, 0, PAGE_SIZE as usize);
            if !node.is_null() {
                read_vfs(node, file_off, PAGE_SIZE, phys);
            }
            vmm_map(pd, va + i * PAGE_SIZE, phys as u32, page_flags);
            file_off += PAGE_SIZE;
        }
    }

    (*region).base = va;
    (*region).length = length;
    (*region).flags = flags as u32;
    (*region).prot = prot as u32;
    (*region).fd = if flags & MAP_ANON != 0 { -1 } else { fd };
    (*region).file_off = offset;
    (*region).is_used = 1;

    if va + length > (*tbl).next_base {
        (*tbl).next_base = va + length;
    }

    va as *mut u8
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn do_munmap(
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
    for i in 0..MMAP_MAX_REGIONS {
        let r = &mut (*tbl).regions[i];
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
        if *pd.add(pdi) & PAGE_PRESENT == 0 {
            continue;
        }
        let pt = (*pd.add(pdi) & !0xFFF) as *mut u32;
        let pte = *pt.add(pt_index(va) as usize);

        if pte & PAGE_PRESENT != 0 {
            kfree_page((pte & !0xFFF) as *mut u8);
        }
        *pt.add(pt_index(va) as usize) = 0;
        tlb_flush(va);
    }

    if addr == (*region).base && length >= (*region).length {
        (*region).is_used = 0;
        (*region).fd = -1;
    } else if addr == (*region).base {
        (*region).base += length;
        (*region).length -= length;
        (*region).file_off += length;
    } else {
        (*region).length = addr - (*region).base;
    }

    0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn do_mprotect(
    pd: *mut u32,
    tbl: *mut MmapTable,
    addr: u32,
    mut length: u32,
    prot: i32,
) -> i32 {
    if pd.is_null() || tbl.is_null() || length == 0 {
        return -1;
    }
    if addr % PAGE_SIZE != 0 {
        return -1;
    }

    length = (length + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);

    let region = mmap_find_region(tbl, addr);
    if region.is_null() {
        return -1;
    }

    let page_flags = prot_to_page_flags(prot, true) as u32;
    let pages = length / PAGE_SIZE;

    for i in 0..pages {
        let va = addr + i * PAGE_SIZE;
        let pdi = pd_index(va) as usize;
        if *pd.add(pdi) & PAGE_PRESENT == 0 {
            continue;
        }
        let pt = (*pd.add(pdi) & !0xFFF) as *mut u32;
        let pte = *pt.add(pt_index(va) as usize);
        if pte & PAGE_PRESENT == 0 {
            continue;
        }
        *pt.add(pt_index(va) as usize) = (pte & !0xFFF) | page_flags;
        tlb_flush(va);
    }

    (*region).prot = prot as u32;
    0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mmap_handle_fault(
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

    if *pd.add(pdi) & PAGE_PRESENT == 0 {
        return -1;
    }
    let pt = (*pd.add(pdi) & !0xFFF) as *mut u32;

    if *pt.add(pti) & PAGE_PRESENT != 0 {
        return -1;
    }

    let phys = kalloc();
    if phys.is_null() {
        return -1;
    }
    core::ptr::write_bytes(phys, 0, PAGE_SIZE as usize);

    if (*region).fd >= 0 {
        let page_offset = page_va - (*region).base;
        let file_off = (*region).file_off + page_offset;
        let node = fd_to_node((*region).fd);
        if !node.is_null() {
            read_vfs(node, file_off, PAGE_SIZE, phys);
        }
    }

    let page_flags = prot_to_page_flags((*region).prot as i32, true) as u32;
    *pt.add(pti) = (phys as u32 & !0xFFF) | page_flags;
    tlb_flush(page_va);
    0
}

//public api
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

    (*dst).next_base = (*src).next_base;

    for i in 0..MMAP_MAX_REGIONS {
        let sr = &(*src).regions[i];
        let dr = &mut (*dst).regions[i];

        if sr.is_used == 0 {
            dr.is_used = 0;
            continue;
        }

        *dr = core::ptr::read(sr);

        let pages = sr.length / PAGE_SIZE;

        if sr.flags & MAP_SHARED as u32 != 0 {
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
                    core::ptr::write_bytes(new_pt, 0, 1024);
                    *dst_pd.add(pdi) =
                        (new_pt as u32 & !0xFFF) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
                }
                let dst_pt = (*dst_pd.add(pdi) & !0xFFF) as *mut u32;
                *dst_pt.add(pti) = pte;
            }
        } else {
            for p in 0..pages {
                let va = sr.base + p * PAGE_SIZE;
                let pdi = pd_index(va) as usize;
                let pti = pt_index(va) as usize;

                if *src_pd.add(pdi) & PAGE_PRESENT == 0 {
                    continue;
                }
                let src_pt = (*src_pd.add(pdi) & !0xFFF) as *mut u32;
                let pte = *src_pt.add(pti);

                if *dst_pd.add(pdi) & PAGE_PRESENT == 0 {
                    let new_pt = kalloc() as *mut u32;
                    if new_pt.is_null() {
                        continue;
                    }
                    core::ptr::write_bytes(new_pt, 0, 1024);
                    *dst_pd.add(pdi) =
                        (new_pt as u32 & !0xFFF) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
                }
                let dst_pt = (*dst_pd.add(pdi) & !0xFFF) as *mut u32;

                if pte & PAGE_PRESENT == 0 {
                    *dst_pt.add(pti) = pte;
                    continue;
                }

                let cow_pte = (pte & !PAGE_RW) | PAGE_COW;
                *src_pt.add(pti) = cow_pte;
                *dst_pt.add(pti) = cow_pte;
            }

            tlb_flush_all();
        }
    }
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mmap_table_free(tbl: *mut MmapTable, pd: *mut u32) {
    if tbl.is_null() || pd.is_null() {
        return;
    }
    for i in 0..MMAP_MAX_REGIONS {
        let r = &(*tbl).regions[i];
        if r.is_used == 0 {
            continue;
        }
        do_munmap(pd, tbl, r.base, r.length);
    }
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mmap_print_regions(tbl: *const MmapTable) {
    if tbl.is_null() {
        return;
    }
    let mut buf = [0u8; 16];
    kprint(b"[MMAP] === Memory Regions ===\n\0".as_ptr());
    for i in 0..MMAP_MAX_REGIONS {
        let r = &(*tbl).regions[i];
        if r.is_used == 0 {
            continue;
        }
        kprint(b"  [\0".as_ptr());
        itoa(i as i32, buf.as_mut_ptr());
        kprint(buf.as_ptr());
        kprint(b"] base=0x\0".as_ptr());
        hex_to_ascii(r.base, buf.as_mut_ptr());
        kprint(buf.as_ptr());
        kprint(b" len=0x\0".as_ptr());
        hex_to_ascii(r.length, buf.as_mut_ptr());
        kprint(buf.as_ptr());
        kprint(b" prot=\0".as_ptr());
        itoa(r.prot as i32, buf.as_mut_ptr());
        kprint(buf.as_ptr());
        kprint(b" flags=\0".as_ptr());
        itoa(r.flags as i32, buf.as_mut_ptr());
        kprint(buf.as_ptr());
        kprint(b" fd=\0".as_ptr());
        itoa(r.fd, buf.as_mut_ptr());
        kprint(buf.as_ptr());
        kprint(b"\n\0".as_ptr());
    }
}