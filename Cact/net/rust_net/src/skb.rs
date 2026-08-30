//! Socket buffer (`Skb`) helpers: allocate/free and push/put data for the C networking path.

use core::ffi::c_void;

use crate::checksum;
use crate::ffi_kernel;
use crate::types::{Skb, SKB_MAX_SIZE};

#[no_mangle]
pub extern "C" fn skb_alloc() -> *mut Skb {
    // SAFETY: kernel allocator returns raw heap memory.
    unsafe {
        let ptr = ffi_kernel::kmalloc(core::mem::size_of::<Skb>()) as *mut Skb;
        if ptr.is_null() {
            return core::ptr::null_mut();
        }
        core::ptr::write_bytes(ptr.cast::<u8>(), 0, core::mem::size_of::<Skb>());
        (*ptr).data_offset = (SKB_MAX_SIZE / 2) as u16;
        ptr
    }
}

#[no_mangle]
pub extern "C" fn kfree_skb(skb: *mut Skb) {
    if skb.is_null() {
        return;
    }
    // SAFETY: skb was allocated via kmalloc.
    unsafe {
        ffi_kernel::kfree(skb as *mut c_void);
    }
}

#[no_mangle]
pub extern "C" fn skb_push(skb: *mut Skb, len: u16) -> *mut u8 {
    if skb.is_null() {
        return core::ptr::null_mut();
    }
    unsafe {
        if (*skb).data_offset < len {
            return core::ptr::null_mut();
        }
        (*skb).data_offset -= len;
        (*skb).total_len = (*skb).total_len.wrapping_add(len);
        (*skb).data.as_mut_ptr().add((*skb).data_offset as usize)
    }
}

#[no_mangle]
pub extern "C" fn skb_put(skb: *mut Skb, len: u16) -> *mut u8 {
    if skb.is_null() {
        return core::ptr::null_mut();
    }
    unsafe {
        let offset = (*skb).data_offset as usize;
        let cur_len = (*skb).total_len as usize;
        let new_total = offset + cur_len + len as usize;
        if new_total > SKB_MAX_SIZE {
            return core::ptr::null_mut();
        }
        let ptr = (*skb).data.as_mut_ptr().add(offset + cur_len);
        core::ptr::write_bytes(ptr, 0, len as usize);
        (*skb).total_len = (*skb).total_len.wrapping_add(len);
        ptr
    }
}

#[no_mangle]
pub extern "C" fn skb_data(skb: *mut Skb) -> *mut u8 {
    if skb.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: valid skb pointer.
    unsafe { (*skb).data.as_mut_ptr().add((*skb).data_offset as usize) }
}

#[no_mangle]
pub extern "C" fn skb_len(skb: *mut Skb) -> u16 {
    if skb.is_null() {
        return 0;
    }
    // SAFETY: valid skb pointer.
    unsafe { (*skb).total_len }
}

#[no_mangle]
pub extern "C" fn inet_checksum(data: *mut c_void, len: u16) -> u16 {
    checksum::inet_checksum(data.cast::<u8>(), len)
}
