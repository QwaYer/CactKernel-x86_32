use crate::ffi::*;
use crate::safe::{kprint_str, kprint_hex, klog_msg};
use crate::pmm::{kalloc, kfree_page};

#[repr(C, align(4096))]
struct Aligned4K<T>(T);

#[no_mangle]
static mut page_directory: Aligned4K<[u32; 1024]> = Aligned4K([0u32; 1024]);
static mut PAGE_TABLES: Aligned4K<[[u32; 1024]; 32]> = Aligned4K([[0u32; 1024]; 32]);

pub fn get_kernel_pd() -> *mut u32 {
    unsafe { page_directory.0.as_mut_ptr() }
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn init_paging() {
    kprint_str(b"[PAGING] page_directory at 0x\0".as_ptr());
    let pd_addr = unsafe { page_directory.0.as_ptr() as u32 };
    kprint_hex(pd_addr);
    kprint_str(b"  identity-mapping first 128 MB (32 page tables x 1024 pages)\n\0".as_ptr());

    unsafe {
        for i in 0..1024 {
            if page_directory.0[i] == 0 {
                page_directory.0[i] = 0x00000002;
            }
        }

        for j in 0..32 {
            for i in 0..1024 {
                PAGE_TABLES.0[j][i] = ((j as u32 * 1024 + i as u32) * 4096) | 3;
            }
            page_directory.0[j] = (PAGE_TABLES.0[j].as_ptr() as u32) | 3;
        }
    }

    kprint_str(b"[PAGING] loading CR3=0x\0".as_ptr());
    kprint_hex(unsafe { page_directory.0.as_ptr() as u32 });
    kprint_str(b"  setting CR0.PG\n\0".as_ptr());
    unsafe {
        load_page_directory(page_directory.0.as_mut_ptr());
        enable_paging();
    }
    kprint_str(b"[PAGING] paging enabled \xe2\x80\x94 virtual address space active\n\0".as_ptr());
    klog_msg(LOG_OK, b"paging enabled\0".as_ptr());
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn vmm_map(pd: *mut u32, virtual_addr: u32, physical_addr: u32, flags: i32) {
    if pd.is_null() {
        return;
    }
    if virtual_addr % PAGE_SIZE != 0 || physical_addr % PAGE_SIZE != 0 {
        kprint_str(b"[ERR] vmm_map: addresses must be page-aligned\n\0".as_ptr());
        return;
    }

    let pd_idx = pd_index(virtual_addr) as usize;
    let pt_idx = pt_index(virtual_addr) as usize;
    let flags = flags as u32;

    unsafe {
        if *pd.add(pd_idx) & PAGE_PRESENT == 0 {
            let pt = kalloc() as *mut u32;
            for i in 0..1024 {
                *pt.add(i) = 0;
            }
            *pd.add(pd_idx) = (pt as u32) | flags | PAGE_PRESENT;
        } else {
            *pd.add(pd_idx) |= flags & (PAGE_USER | PAGE_RW);
        }

        let pt = (*pd.add(pd_idx) & !0xFFF) as *mut u32;

        let old_pte = *pt.add(pt_idx);
        if old_pte & PAGE_PRESENT != 0
            && old_pte & PAGE_COW != 0
            && (old_pte & !0xFFF) != (physical_addr & !0xFFF)
        {
            kfree_page((old_pte & !0xFFF) as *mut u8);
        }

        *pt.add(pt_idx) = (physical_addr & !0xFFF) | flags | PAGE_PRESENT;
    }
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn vmm_create_address_space() -> *mut u32 {
    let pd = kalloc() as *mut u32;
    if pd.is_null() {
        return core::ptr::null_mut();
    }

    unsafe {
        for i in 0..32 {
            *pd.add(i) = page_directory.0[i];
        }
        for i in 32..768 {
            *pd.add(i) = 0;
        }
        for i in 768..1024 {
            *pd.add(i) = page_directory.0[i];
        }
    }
    pd
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn vmm_free_address_space(pd: *mut u32) {
    if pd.is_null() {
        return;
    }
    unsafe {
        for i in 32..768 {
            if *pd.add(i) & PAGE_PRESENT != 0 {
                let pt = (*pd.add(i) & !0xFFF) as *mut u32;
                for j in 0..1024 {
                    if *pt.add(j) & PAGE_PRESENT != 0 {
                        kfree_page((*pt.add(j) & !0xFFF) as *mut u8);
                    }
                }
                kfree_page(pt as *mut u8);
            }
        }
        kfree_page(pd as *mut u8);
    }
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn vmm_copy_address_space(src_pd: *mut u32, dst_pd: *mut u32) {
    if src_pd.is_null() || dst_pd.is_null() {
        return;
    }
    unsafe {
        for i in 32..768 {
            if *src_pd.add(i) & PAGE_PRESENT != 0 {
                let src_pt = (*src_pd.add(i) & !0xFFF) as *mut u32;
                let dst_pt = kalloc() as *mut u32;
                if dst_pt.is_null() {
                    continue;
                }
                for j in 0..1024usize {
                    *dst_pt.add(j) = 0;
                }

                for j in 0..1024usize {
                    if *src_pt.add(j) & PAGE_PRESENT != 0 {
                        let new_page = kalloc();
                        if new_page.is_null() {
                            continue;
                        }
                        let src_phys = (*src_pt.add(j) & !0xFFF) as *const u8;
                        core::ptr::copy_nonoverlapping(src_phys, new_page, PAGE_SIZE as usize);
                        *dst_pt.add(j) = (new_page as u32 & !0xFFF) | (*src_pt.add(j) & 0xFFF);
                    }
                }
                *dst_pd.add(i) = (dst_pt as u32) | (*src_pd.add(i) & 0xFFF);
            }
        }
    }
}
