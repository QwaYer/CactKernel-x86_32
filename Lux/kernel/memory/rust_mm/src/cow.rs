use crate::ffi::*;
use crate::pmm::kalloc;

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

    for i in 32..768 {
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

            let new_page = kalloc();
            if new_page.is_null() {
                *dst_pt.add(j) = 0;
                continue;
            }
            let old_page = (pte & !0xFFFu32) as *const u8;
            core::ptr::copy_nonoverlapping(old_page, new_page, PAGE_SIZE as usize);
            *dst_pt.add(j) = (new_page as u32 & !0xFFFu32) | (pte & 0xFFFu32);
        }

        *dst_pd.add(i) = (dst_pt as u32 & !0xFFFu32) | (*src_pd.add(i) & 0xFFFu32);
    }
}