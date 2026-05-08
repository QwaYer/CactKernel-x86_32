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
pub extern "C" fn skb_free(skb: *mut Skb) {
    if skb.is_null() {
        return;
    }
    // SAFETY: skb was allocated via kmalloc.
    unsafe {
        ffi_kernel::kfree_heap(skb as *mut c_void);
    }
}

#[no_mangle]
pub extern "C" fn skb_push(skb: *mut Skb, len: u16) -> *mut u8 {
    if skb.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: caller ensures len fits skb headroom.
    unsafe {
        (*skb).data_offset = (*skb).data_offset.wrapping_sub(len);
        (*skb).total_len = (*skb).total_len.wrapping_add(len);
        (*skb).data.as_mut_ptr().add((*skb).data_offset as usize)
    }
}

#[no_mangle]
pub extern "C" fn skb_put(skb: *mut Skb, len: u16) -> *mut u8 {
    if skb.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: caller ensures tailroom.
    unsafe {
        let ptr = (*skb)
            .data
            .as_mut_ptr()
            .add((*skb).data_offset as usize + (*skb).total_len as usize);
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
