//! Socket buffer (`Skb`) helpers: allocate/free and push/put data for the C networking path.


use crate::ffi_kernel;
use crate::types::{Skb, SKB_MAX_SIZE};

#[no_mangle]
pub extern "C" fn skb_alloc() -> *mut Skb {
    let ptr = ffi_kernel::kmalloc(core::mem::size_of::<Skb>() as u32) as *mut Skb;
    if ptr.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: `kmalloc` returned `size_of::<Skb>()` writable bytes and the null
    // check above guarantees `ptr` is valid, so this zeroes exactly the
    // freshly-allocated `Skb`.
    unsafe { core::ptr::write_bytes(ptr.cast::<u8>(), 0, core::mem::size_of::<Skb>()) };
    // SAFETY: as above — `ptr` points at a live, zeroed `Skb` that is not yet
    // published, so no other context can reach it.  `data_offset` is a `u16`
    // field of that object, so the store is in bounds and aligned.
    unsafe { (*ptr).data_offset = (SKB_MAX_SIZE / 2) as u16 };
    ptr
}

#[no_mangle]
pub extern "C" fn kfree_skb(skb: *mut Skb) {
    if skb.is_null() {
        return;
    }
    // SAFETY: skb was allocated via kmalloc.
    unsafe {
        ffi_kernel::kfree(skb as *mut u8);
    }
}

/// # Safety
///
/// `skb` must be non-null and point to a live [`Skb`] allocated by [`skb_alloc`]
/// (or an equivalent kernel allocation cleared the same way) that the caller may
/// access for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn skb_push(skb: *mut Skb, len: u16) -> *mut u8 {
    if skb.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: the caller contract (see # Safety) makes `skb` a valid pointer to
    // a live `Skb` that this call borrows exclusively and does not let escape
    // (no other call in this function touches it), so `s` is the only reference
    // to it.  `data_offset` is checked to be >= `len` below, so the returned
    // pointer stays inside the inline `data` array, whose producer keeps
    // `data_offset < SKB_MAX_SIZE`.
    let s = unsafe { &mut *skb };
    if s.data_offset < len {
        return core::ptr::null_mut();
    }
    s.data_offset -= len;
    s.total_len = s.total_len.wrapping_add(len);
    // SAFETY: `data_offset >= len` was checked above and producers keep it
    // below `SKB_MAX_SIZE`, so the resulting pointer stays inside `s.data`.
    unsafe { s.data.as_mut_ptr().add(s.data_offset as usize) }
}

/// # Safety
///
/// `skb` must be non-null and point to a live [`Skb`] allocated by [`skb_alloc`]
/// that the caller may access for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn skb_put(skb: *mut Skb, len: u16) -> *mut u8 {
    if skb.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: the caller contract (see # Safety) makes `skb` a valid pointer to
    // a live `Skb` that this call borrows exclusively and does not let escape
    // (no other call in this function touches it), so `s` is the only reference
    // to it.
    let s = unsafe { &mut *skb };
    let offset = s.data_offset as usize;
    let cur_len = s.total_len as usize;
    let new_total = offset + cur_len + len as usize;
    if new_total > SKB_MAX_SIZE {
        return core::ptr::null_mut();
    }
    // SAFETY: `new_total <= SKB_MAX_SIZE` was checked above, so `offset +
    // cur_len` is within `s.data` and the pointer stays inside it.
    let ptr = unsafe { s.data.as_mut_ptr().add(offset + cur_len) };
    // SAFETY: `new_total <= SKB_MAX_SIZE` and `ptr` is `data` advanced by
    // `offset + cur_len`, so the `len`-byte range zeroed here lies inside the
    // inline `data` array (which is the object `ptr` was derived from).
    unsafe { core::ptr::write_bytes(ptr, 0, len as usize) };
    s.total_len = s.total_len.wrapping_add(len);
    ptr
}

/// # Safety
///
/// `skb` must be non-null and point to a live, initialised [`Skb`] that the
/// caller may access for the duration of the call, with `data_offset` kept
/// within `SKB_MAX_SIZE`.
#[no_mangle]
pub unsafe extern "C" fn skb_data(skb: *mut Skb) -> *mut u8 {
    if skb.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: the caller contract (see # Safety) makes `skb` a valid pointer to
    // a live `Skb` that this call borrows and does not let escape; `data_offset`
    // is kept inside the inline `data` array, so the resulting pointer is in
    // bounds.
    let s = unsafe { &mut *skb };
    // SAFETY: `data_offset` is kept inside the inline `data` array by the
    // caller contract, so the resulting pointer is in bounds.
    unsafe { s.data.as_mut_ptr().add(s.data_offset as usize) }
}

/// # Safety
///
/// `skb` must be non-null and point to a live, initialised [`Skb`] that the
/// caller may access for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn skb_len(skb: *mut Skb) -> u16 {
    if skb.is_null() {
        return 0;
    }
    // SAFETY: the caller contract (see # Safety) makes `skb` a valid pointer to
    // a live `Skb`, so the `total_len` read is in bounds and initialised.
    unsafe { (*skb).total_len }
}
