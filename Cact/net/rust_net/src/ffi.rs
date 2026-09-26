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
    loop {
        // SAFETY: the caller owns `ptr` and guarantees it is NUL-terminated, so
        // every offset below the terminator is a readable byte of that string.
        let p = unsafe { ptr.add(len) };
        // SAFETY: `p` is a byte of the NUL-terminated string above; the scan
        // stops as soon as it is the terminator.
        if unsafe { *p } == 0 {
            break;
        }
        len += 1;
    }
    len
}

/// # Safety
///
/// `input` must be a NUL-terminated string readable for its whole length, and
/// `out_host_ip` must point to a writable, properly aligned `u32`.  Both may be
/// null, in which case the call returns -1 without touching memory.
#[no_mangle]
pub unsafe extern "C" fn rust_net_parse_ipv4(input: *const c_char, out_host_ip: *mut u32) -> c_int {
    if input.is_null() || out_host_ip.is_null() {
        return -1;
    }

    let len = cstr_len(input);
    // SAFETY: the caller contract (see # Safety) makes `input` a NUL-terminated
    // string, so `cstr_len` counted exactly the bytes before the terminator and
    // all `len` of them are readable.
    let bytes = unsafe { core::slice::from_raw_parts(input.cast::<u8>(), len) };

    match parse_ipv4_host(bytes) {
        Some(ip) => {
            // SAFETY: the caller contract (see # Safety) makes `out_host_ip` a
            // writable, aligned `u32`; the null check above excluded null.
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

/// Send one echo request and block until the matching reply arrives.
/// Returns the round-trip time in microseconds, or -1 on timeout.
///
/// # Safety
///
/// `src_ip_out` and `bytes_out` may be null (that result is then dropped), but
/// each non-null one must point to a writable, properly aligned `u32` that
/// stays live for the call.
#[no_mangle]
pub unsafe extern "C" fn rust_net_ping_wait(
    dst_ip_host: u32,
    id: u16,
    seq: u16,
    timeout_ms: u32,
    src_ip_out: *mut u32,
    bytes_out: *mut u32,
) -> c_int {
    // SAFETY: the caller contract above makes each non-null output pointer a
    // writable `u32`; `ping_wait_host` only stores through them after its own
    // `is_null` checks, and it blocks no longer than `timeout_ms`.
    unsafe { ping::ping_wait_host(dst_ip_host, id, seq, timeout_ms, src_ip_out, bytes_out) }
}
