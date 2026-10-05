#![no_std]
// Safety baseline: unsafe ops inside `unsafe fn` bodies must be explicit, and every
// `unsafe` block needs a SAFETY comment (the latter enforced under clippy).
#![deny(unsafe_op_in_unsafe_fn)]
#![deny(unused_unsafe)]
#![deny(clippy::undocumented_unsafe_blocks)]
#![deny(clippy::missing_safety_doc)]
#![deny(static_mut_refs)]
#![deny(improper_ctypes, improper_ctypes_definitions)]
// Additional safety lints (all verified zero-hit when enabled, 2026-09-26):
// transmute misuse, byte-vs-element count confusion and assumptions that
// uninitialised memory is valid become hard errors instead of silent warnings.
#![deny(clippy::transmute_ptr_to_ref)]
#![deny(clippy::transmute_ptr_to_ptr)]
#![deny(clippy::useless_transmute)]
#![deny(clippy::size_of_in_element_count)]
#![deny(clippy::uninit_assumed_init)]
#![deny(invalid_reference_casting)]
// Maximal-safety: one unsafe operation per `unsafe` block, so each `// SAFETY:`
// justifies exactly one operation.
#![deny(clippy::multiple_unsafe_ops_per_block)]

mod crypt;

// The kernel Rust graph's single `#[panic_handler]` lives in `cact_mm`.

/// # Safety
///
/// `data` must be null or point to at least `data_len` readable bytes that stay
/// valid for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn cact_debug_xor(data: *const u8, data_len: u32) -> u32 {
    if data.is_null() {
        return 0xFFFFFFFF;
    }
    // SAFETY: `data` is non-null (checked above) and the caller guarantees
    // `data_len` readable bytes at it, valid for the duration of the call.
    let slice = unsafe { core::slice::from_raw_parts(data, data_len as usize) };
    let mut x: u32 = 0;
    for &b in slice {
        x ^= b as u32;
    }
    x
}
