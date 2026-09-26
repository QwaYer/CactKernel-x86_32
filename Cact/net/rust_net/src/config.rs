//! Runtime IPv4 settings in **host** byte order (mutable globals updated from C).
//!
//! Defaults are compile-time placeholders; `rust_net_set_ipv4_config` overwrites them
//! and, when the stack is up, pushes the new addresses into smoltcp.
//!
//! The kernel keeps no DHCP client: whatever address/gateway/DNS is stored here
//! was chosen by userspace (static configuration or a userspace DHCP daemon).

/// The four IPv4 values as one object, so a reader/writer needs a single
/// dereference rather than one per field.
struct NetCfg {
    ip: u32,
    mask: u32,
    gateway: u32,
    dns: u32,
}

static mut NET_CFG: NetCfg = NetCfg {
    ip: (10u32 << 24) | (2 << 8) | 15,
    mask: (255u32 << 24) | (255 << 16) | (255 << 8),
    gateway: (10u32 << 24) | (2 << 8) | 2,
    dns: (8u32 << 24) | (8 << 16) | (8 << 8) | 8,
};

#[inline]
pub fn ip_host() -> u32 {
    // SAFETY: `NET_CFG` is written only by `rust_net_set_ipv4_config`, which
    // stores whole aligned 32-bit words, so this word load cannot tear —
    // whatever is read is a complete address the userland manager configured.
    unsafe { NET_CFG.ip }
}

#[inline]
pub fn netmask_host() -> u32 {
    // SAFETY: as `ip_host` — the field is only ever written as a whole aligned
    // 32-bit word, so this load cannot observe a torn value.
    unsafe { NET_CFG.mask }
}

#[inline]
pub fn gateway_host() -> u32 {
    // SAFETY: as `ip_host` — the field is only ever written as a whole aligned
    // 32-bit word, so this load cannot observe a torn value.
    unsafe { NET_CFG.gateway }
}

#[inline]
pub fn dns_host() -> u32 {
    // SAFETY: as `ip_host` — the field is only ever written as a whole aligned
    // 32-bit word, so this load cannot observe a torn value.
    unsafe { NET_CFG.dns }
}

#[no_mangle]
pub extern "C" fn rust_net_set_ipv4_config(ip_h: u32, mask_h: u32, gw_h: u32, dns_h: u32) -> i32 {
    // All values are trusted from the (root-only) netcfg path.  Zeros are
    // meaningful: ip==0 clears the interface address, gw==0 clears the default
    // route, dns==0 clears the resolver server.
    // SAFETY: `NET_CFG` has no other writer (the getters above only read it) and
    // the borrow ends before the `stack` calls below, so the four whole-word
    // stores are the only accesses in flight here.
    let cfg = unsafe { &mut *core::ptr::addr_of_mut!(NET_CFG) };
    cfg.ip = ip_h;
    cfg.mask = mask_h;
    cfg.gateway = gw_h;
    cfg.dns = dns_h;
    // SAFETY: `STACK_READY` is a `static mut bool` set by `stack_init` and
    // cleared by `stack_teardown`; a byte load reads either 0 or 1.
    if unsafe { crate::stack::STACK_READY } {
        let _ = crate::stack::with_iface_sockets(|iface, _socks| {
            crate::stack::sync_iface_ipv4_from_config(iface);
        });
    }
    0
}

/// Snapshot of the current IPv4 link configuration (host byte order).
/// All output pointers may be NULL.
///
/// # Safety
///
/// Each of `ip_h`, `mask_h`, `gw_h` and `dns_h` may be null (it is then
/// skipped), but every non-null one must point to a writable, properly aligned
/// `u32` that stays live for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn rust_net_get_ipv4_config(
    ip_h: *mut u32,
    mask_h: *mut u32,
    gw_h: *mut u32,
    dns_h: *mut u32,
) -> i32 {
    // SAFETY: `NET_CFG` is only written by `rust_net_set_ipv4_config` and the
    // borrow ends before any call below, so this shared read sees a consistent
    // whole-word snapshot.
    let cfg = unsafe { &*core::ptr::addr_of!(NET_CFG) };
    // SAFETY: the caller contract (see # Safety) makes each non-null pointer a
    // writable, aligned `u32` for the call; the store is guarded by its own
    // `is_null` test, so no null is ever written through.
    unsafe {
        if !ip_h.is_null() {
            *ip_h = cfg.ip;
        }
    }
    // SAFETY: as above for `mask_h`.
    unsafe {
        if !mask_h.is_null() {
            *mask_h = cfg.mask;
        }
    }
    // SAFETY: as above for `gw_h`.
    unsafe {
        if !gw_h.is_null() {
            *gw_h = cfg.gateway;
        }
    }
    // SAFETY: as above for `dns_h`.
    unsafe {
        if !dns_h.is_null() {
            *dns_h = cfg.dns;
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
