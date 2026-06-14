#![no_std]

use hmac::Mac;
use sha2::Sha256;

const CACT_HMAC_KEY: &[u8] = b"CactKernel-HMAC-Secret-2026-32B!!";

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    let mut r: u8 = 0;
    for i in 0..a.len() {
        r |= a[i] ^ b[i];
    }
    r == 0
}

#[no_mangle]
pub extern "C" fn cact_debug_xor(data: *const u8, data_len: u32) -> u32 {
    if data.is_null() {
        return 0xFFFFFFFF;
    }
    let slice = unsafe { core::slice::from_raw_parts(data, data_len as usize) };
    let mut x: u32 = 0;
    for &b in slice {
        x ^= b as u32;
    }
    x
}

#[no_mangle]
pub extern "C" fn cact_hmac_verify(
    data: *const u8,
    data_len: u32,
    tag: *const u8,
    tag_len: u32,
) -> i32 {
    if data.is_null() || tag.is_null() || tag_len != 32 {
        return -1;
    }
    let data_slice = unsafe { core::slice::from_raw_parts(data, data_len as usize) };
    let tag_slice = unsafe { core::slice::from_raw_parts(tag, tag_len as usize) };

    let mut mac = match hmac::Hmac::<Sha256>::new_from_slice(CACT_HMAC_KEY) {
        Ok(m) => m,
        Err(_) => return -1,
    };
    mac.update(data_slice);

    let computed = mac.finalize().into_bytes();

    if ct_eq(&computed, tag_slice) {
        0
    } else {
        -1
    }
}
