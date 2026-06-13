//! Small C-callable helpers: dotted IPv4 parsing and ping dispatch.
//!
//! String inputs are expected to be NUL-terminated C strings owned by the caller.

use core::ffi::{c_char, c_int};

use crate::ping;

fn parse_ipv4_host(bytes: &[u8]) -> Option<u32> {
    let s = core::str::from_utf8(bytes).ok()?;
    let mut parts = s.split('.');
    let a: u32 = parts.next()?.parse().ok()?;
    let b: u32 = parts.next()?.parse().ok()?;
    let c: u32 = parts.next()?.parse().ok()?;
    let d: u32 = parts.next()?.parse().ok()?;
    if parts.next().is_some() {
        return None;
    }
    if a > 255 || b > 255 || c > 255 || d > 255 {
        return None;
    }
    Some((a << 24) | (b << 16) | (c << 8) | d)
}

fn cstr_len(ptr: *const c_char) -> usize {
    let mut len = 0usize;
    // SAFETY: caller owns `ptr`; this scans until '\0'.
    unsafe {
        while *ptr.add(len) != 0 {
            len += 1;
        }
    }
    len
}

#[no_mangle]
pub extern "C" fn rust_net_parse_ipv4(input: *const c_char, out_host_ip: *mut u32) -> c_int {
    if input.is_null() || out_host_ip.is_null() {
        return -1;
    }

    let len = cstr_len(input);
    // SAFETY: validated non-null; len computed from NUL-terminated string.
    let bytes = unsafe { core::slice::from_raw_parts(input.cast::<u8>(), len) };

    match parse_ipv4_host(bytes) {
        Some(ip) => {
            // SAFETY: pointer was validated by caller contract above.
            unsafe { *out_host_ip = ip; }
            0
        }
        None => -1,
    }
}

#[no_mangle]
pub extern "C" fn rust_net_ping_echo_host(dst_ip_host: u32, id: u16, seq: u16) -> c_int {
    ping::send_echo_request_host(dst_ip_host, id, seq)
}
