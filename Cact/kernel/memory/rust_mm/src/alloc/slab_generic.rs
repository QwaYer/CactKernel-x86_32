//! Generic (kmalloc-style) size caches on top of the slab allocator.

use crate::ffi::*;
use crate::safe::{kprint_str, klog_msg};
use crate::alloc::heap::{kmalloc, kfree};
use crate::alloc::slab::{
    slab_alloc, slab_cache_create, slab_free, G_CACHE_LOCK, G_GENERIC_CACHES, Slab,
    GENERIC_CACHE_COUNT,
};

const GENERIC_SIZES: [u32; GENERIC_CACHE_COUNT] = [8, 16, 32, 64, 128, 256, 512, 1024, 2048];

#[unsafe(no_mangle)]
pub extern "C" fn slab_init() {
    // SAFETY: boot-time init, single-threaded.
    unsafe { irq_spinlock_init(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock) };

    for i in 0..GENERIC_CACHE_COUNT {
        let mut name = [0u8; SLAB_NAME_LEN];
        let prefix = b"kmalloc-";
        name[..prefix.len()].copy_from_slice(prefix);

        let mut num_buf = [0u8; 8];
        let mut sz = GENERIC_SIZES[i];
        let mut p: usize = 6;
        num_buf[7] = 0;
        loop {
            num_buf[p] = b'0' + (sz % 10) as u8;
            sz /= 10;
            if sz == 0 {
                break;
            }
            p -= 1;
        }
        let mut j = prefix.len();
        let mut k = p;
        while k < 7 && j < SLAB_NAME_LEN - 1 {
            name[j] = num_buf[k];
            j += 1;
            k += 1;
        }
        name[j] = 0;

        let cache = slab_cache_create(name.as_ptr(), GENERIC_SIZES[i], None, None);
        G_GENERIC_CACHES.get_mut()[i] = cache;
        if cache.is_null() {
            // SAFETY: printk_color is a C function that takes a valid string.
            unsafe {
                printk_color(b"[SLAB] failed to create cache: \0".as_ptr(), COLOR_LIGHT_RED);
                printk_color(name.as_ptr(), COLOR_LIGHT_RED);
            }
            kprint_str(b"\n\0".as_ptr());
            klog_msg(LOG_FAIL, b"slab cache creation failed\0".as_ptr());
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn slab_kmalloc(size: u32) -> *mut u8 {
    if size == 0 {
        return core::ptr::null_mut();
    }
    let caches = G_GENERIC_CACHES.get_mut();
    for i in 0..GENERIC_CACHE_COUNT {
        if size <= GENERIC_SIZES[i] {
            return slab_alloc(caches[i]);
        }
    }
    kmalloc(size)
}

#[unsafe(no_mangle)]
pub extern "C" fn slab_kfree(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    // SAFETY: obj resides in a page whose start is a Slab header.
    let s = (ptr as u32 & !(PAGE_SIZE - 1)) as *mut Slab;
    unsafe {
        if !(*s).cache.is_null() && (*s).capacity > 0 && (*s).capacity <= 512 {
            slab_free((*s).cache, ptr);
            return;
        }
    }
    kfree(ptr);
}
