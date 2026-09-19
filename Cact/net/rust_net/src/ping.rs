//! ICMP echo (ping) helper: validates the destination and forwards to the stack layer.

use core::ffi::c_int;

pub fn send_echo_request_host(dst_ip_host: u32, id: u16, seq: u16) -> c_int {
    if dst_ip_host == 0 || dst_ip_host == 0xFFFF_FFFF {
        return -1;
    }
    if crate::stack::icmp_echo_request_host(dst_ip_host, id, seq) {
        0
    } else {
        -1
    }
}

/// Send one echo request and wait for its reply.
///
/// Returns the round-trip time in microseconds, or -1 on timeout/failure.  On
/// success `src_ip_out` receives the responder's address (host order) and
/// `bytes_out` the ICMP message length.
///
/// The wait is driven from here — this runs in the calling task's context — so
/// ping works even if the background poll task is busy elsewhere.  It spins
/// briefly to keep sub-millisecond RTT resolution, then falls back to a tick
/// sleep so a lost probe does not burn a whole timeout in the CPU.
pub fn ping_wait_host(
    dst_ip_host: u32,
    id: u16,
    seq: u16,
    timeout_ms: u32,
    src_ip_out: *mut u32,
    bytes_out: *mut u32,
) -> c_int {
    if send_echo_request_host(dst_ip_host, id, seq) != 0 {
        return -1;
    }
    let timeout_us = (timeout_ms as u64).saturating_mul(1000);
    unsafe {
        let t0 = crate::ffi_kernel::ktime_get_usec();
        loop {
            crate::stack::stack_poll();
            if let Some((src, bytes)) = crate::stack::icmp_try_recv_reply(id, seq) {
                let rtt = crate::ffi_kernel::ktime_get_usec().saturating_sub(t0);
                if !src_ip_out.is_null() {
                    *src_ip_out = src;
                }
                if !bytes_out.is_null() {
                    *bytes_out = bytes as u32;
                }
                return rtt.min(i32::MAX as u64) as c_int;
            }
            let elapsed = crate::ffi_kernel::ktime_get_usec().saturating_sub(t0);
            if elapsed >= timeout_us {
                return -1;
            }
            if elapsed < 250_000 {
                // Poll every millisecond while the reply is still plausible:
                // a 10 ms tick sleep would inflate every WAN RTT by that much.
                crate::ffi_kernel::ktime_busy_wait_us(1000);
            } else {
                crate::ffi_kernel::sched_sleep_ticks(1);
            }
        }
    }
}
