//! Runtime IPv4 settings in **host** byte order (mutable globals updated from C).
//!
//! Defaults are compile-time placeholders; `rust_net_set_ipv4_config` overwrites them
//! and, when the stack is up, pushes the new addresses into smoltcp.
//!
//! The kernel keeps no DHCP client: whatever address/gateway/DNS is stored here
//! was chosen by userspace (static configuration or a userspace DHCP daemon).

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
    // All values are trusted from the (root-only) netcfg path.  Zeros are
    // meaningful: ip==0 clears the interface address, gw==0 clears the default
    // route, dns==0 clears the resolver server.
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
    0
}

/// Snapshot of the current IPv4 link configuration (host byte order).
/// All output pointers may be NULL.
#[no_mangle]
pub extern "C" fn rust_net_get_ipv4_config(
    ip_h: *mut u32,
    mask_h: *mut u32,
    gw_h: *mut u32,
    dns_h: *mut u32,
) -> i32 {
    unsafe {
        if !ip_h.is_null() {
            *ip_h = NET_IP_HOST;
        }
        if !mask_h.is_null() {
            *mask_h = NET_MASK_HOST;
        }
        if !gw_h.is_null() {
            *gw_h = NET_GATEWAY_HOST;
        }
        if !dns_h.is_null() {
            *dns_h = NET_DNS_HOST;
        }
    }
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
