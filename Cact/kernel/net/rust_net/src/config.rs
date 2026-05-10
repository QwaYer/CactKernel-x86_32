use crate::ffi_kernel;

static mut NET_IP_HOST: u32 = (10u32 << 24) | (0 << 16) | (2 << 8) | 15;
static mut NET_MASK_HOST: u32 = (255u32 << 24) | (255 << 16) | (255 << 8) | 0;
static mut NET_GATEWAY_HOST: u32 = (10u32 << 24) | (0 << 16) | (2 << 8) | 2;
static mut NET_DNS_HOST: u32 = (8u32 << 24) | (8 << 16) | (8 << 8) | 8;

#[inline]
pub fn ip_host() -> u32 {
    unsafe { NET_IP_HOST }
}

#[inline]
pub fn netmask_host() -> u32 {
    unsafe { NET_MASK_HOST }
}

#[inline]
pub fn gateway_host() -> u32 {
    unsafe { NET_GATEWAY_HOST }
}

#[inline]
pub fn dns_host() -> u32 {
    unsafe { NET_DNS_HOST }
}

#[no_mangle]
pub extern "C" fn rust_net_set_ipv4_config(ip_h: u32, mask_h: u32, gw_h: u32, dns_h: u32) -> i32 {
    // Basic sanity: non-zero address and mask.
    if ip_h == 0 || mask_h == 0 {
        return -1;
    }
    unsafe {
        NET_IP_HOST = ip_h;
        NET_MASK_HOST = mask_h;
        NET_GATEWAY_HOST = gw_h;
        NET_DNS_HOST = dns_h;
    }
    if unsafe { crate::stack::STACK_READY } {
        let _ = crate::stack::with_iface_sockets(|iface, _socks| {
            crate::stack::sync_iface_ipv4_from_config(iface);
        });
    }
    ffi_kernel::c_kprint(b"[RUST-NET][CFG] ip=\0");
    ffi_kernel::c_kprint_hex(ip_h);
    ffi_kernel::c_kprint(b" mask=\0");
    ffi_kernel::c_kprint_hex(mask_h);
    ffi_kernel::c_kprint(b" gw=\0");
    ffi_kernel::c_kprint_hex(gw_h);
    ffi_kernel::c_kprint(b" dns=\0");
    ffi_kernel::c_kprint_hex(dns_h);
    ffi_kernel::c_kprint(b"\n\0");
    0
}

#[no_mangle]
pub extern "C" fn rust_net_get_dns_host() -> u32 {
    dns_host()
}

#[no_mangle]
pub extern "C" fn rust_net_get_ip_host() -> u32 {
    ip_host()
}
