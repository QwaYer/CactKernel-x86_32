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
