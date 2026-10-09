//! Per-interface enumeration for userspace (`ip`, `sysinfo`).
//!
//! Before loopback existed the stack had exactly one interface, so userspace
//! read it through the single-NIC `CACT_NETCTL_IFNAME` / `CACT_NETCTL_NETCFG_GET`
//! ioctls.  Those are kept for compatibility; this module adds an indexed view so
//! a tool can list both the NIC and `lo`.

use crate::config;
use crate::runtime;
use crate::stack;

/// Interface is the loopback interface (`lo`).  Must match
/// `CACT_IFACE_FLAG_LOOPBACK` in `ioctl_abi.h`.
pub(crate) const IFACE_FLAG_LOOPBACK: u32 = 0x1;

/// Must match `cact_iface_info_t` in `ioctl_abi.h` byte for byte.
#[repr(C)]
pub struct CactIfaceInfo {
    /// NUL-terminated within `CACT_IFNAME_MAX` (16) bytes.
    pub name: [u8; 16],
    pub ip_host: u32,
    pub netmask_host: u32,
    pub gateway_host: u32,
    pub dns_host: u32,
    pub mac: [u8; 6],
    pub link_up: u32,
    pub flags: u32,
}

/// Number of interfaces currently present: the NIC (when registered) plus
/// loopback, or 0 when the stack is not up.  With no NIC the interface is
/// loopback-only, so `lo` is index 0 and is alone.
#[no_mangle]
pub extern "C" fn rust_net_iface_count() -> i32 {
    // SAFETY: `STACK_READY` is a kernel-lifetime `static mut bool` set by
    // `stack_init` and cleared by `stack_teardown`; a byte load cannot tear and
    // reading it as false simply reports "no interfaces".
    if !unsafe { stack::STACK_READY } {
        0
    } else if crate::runtime::rust_net_link_is_up() > 0 {
        2
    } else {
        1
    }
}

/// Fill `out` with interface `idx`.  Index 0 is the NIC and 1 is `lo` while a NIC
/// is registered; with no NIC, `lo` is the only interface and lives at index 0.
/// Returns 0 on success, -1 when `out` is NULL, the stack is down, or `idx` is out
/// of range.
///
/// # Safety
///
/// `out` may be null (the call then returns -1), but if non-null it must point to
/// a writable, properly aligned [`CactIfaceInfo`] that stays live for the call.
#[no_mangle]
pub unsafe extern "C" fn rust_net_iface_get(idx: u32, out: *mut CactIfaceInfo) -> i32 {
    if out.is_null() {
        return -1;
    }
    // SAFETY: `STACK_READY` is a kernel-lifetime `static mut bool`; a byte load
    // cannot tear.
    if !unsafe { stack::STACK_READY } {
        return -1;
    }
    let has_nic = crate::runtime::rust_net_link_is_up() > 0;
    let count = if has_nic { 2 } else { 1 };
    if idx >= count {
        return -1;
    }
    // SAFETY: the caller contract (see # Safety) makes `out` a live, aligned
    // `CactIfaceInfo`, so this borrow is exclusive for the call and the byte
    // write below stays within the struct.
    let info = unsafe { &mut *out };
    // Zero first so an entry that is only partly filled never leaks whatever was
    // in the caller's buffer.
    // SAFETY: `info` points at one fully initialised `CactIfaceInfo`, so a
    // one-element byte fill is in bounds.
    unsafe { core::ptr::write_bytes(info as *mut CactIfaceInfo, 0, 1) };
    if has_nic && idx == 0 {
        fill_nic(info);
    } else {
        fill_loopback(info);
    }
    0
}

fn fill_nic(info: &mut CactIfaceInfo) {
    // SAFETY: `info.name` is a live 16-byte buffer and `runtime::rust_net_get_ifname`
    // copies at most `cap` bytes, stopping at the terminator; it only fails
    // (returns -1) when there is no NIC, which leaves the name empty.
    unsafe { runtime::rust_net_get_ifname(info.name.as_mut_ptr(), info.name.len() as u32) };
    // SAFETY: as above, for the 6-byte MAC buffer.
    unsafe { runtime::rust_net_get_mac(info.mac.as_mut_ptr()) };
    info.ip_host = config::ip_host();
    info.netmask_host = config::netmask_host();
    info.gateway_host = config::gateway_host();
    info.dns_host = config::dns_host();
    info.link_up = 1;
    info.flags = 0;
}

fn fill_loopback(info: &mut CactIfaceInfo) {
    let name = stack::LOOPBACK_IFNAME;
    info.name[..name.len()].copy_from_slice(name);
    info.ip_host = stack::LOOPBACK_IP_HOST;
    info.netmask_host = stack::LOOPBACK_MASK_HOST;
    info.gateway_host = 0;
    info.dns_host = 0;
    info.link_up = 1;
    info.flags = IFACE_FLAG_LOOPBACK;
}
