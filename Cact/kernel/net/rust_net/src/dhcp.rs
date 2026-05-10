//! Lease metadata for C ABI. DHCP is implemented by smoltcp (`dhcpv4::Socket`).

use crate::ffi_kernel;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DhcpLeaseCfg {
    pub ip_host: u32,
    pub netmask_host: u32,
    pub gateway_host: u32,
    pub dns_host: u32,
    pub server_host: u32,
    pub lease_s: u32,
    pub t1_s: u32,
    pub t2_s: u32,
}

static mut LEASE: DhcpLeaseCfg = DhcpLeaseCfg {
    ip_host: 0,
    netmask_host: 0,
    gateway_host: 0,
    dns_host: 0,
    server_host: 0,
    lease_s: 0,
    t1_s: 0,
    t2_s: 0,
};
/// Store DHCP metadata after smoltcp has already applied addresses via `rust_net_set_ipv4_config`.
pub fn record_smoltcp_lease(cfg: DhcpLeaseCfg) {
    unsafe {
        LEASE = cfg;
    }
    ffi_kernel::c_kprint(b"[RUST-NET][DHCP] lease recorded (smoltcp)\n\0");
}

pub fn clear_smoltcp_lease() {
    ffi_kernel::c_kprint(b"[RUST-NET][DHCP] lease cleared (smoltcp)\n\0");
}

extern "C" fn dhcp_wakeup_daemon() {
    loop {
        unsafe {
            if crate::stack::STACK_READY {
                ffi_kernel::sema_up(core::ptr::addr_of_mut!(crate::runtime::net_sema));
            }
            ffi_kernel::sched_sleep_ticks(50);
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_net_dhcp_set_lease(cfg: *const DhcpLeaseCfg) -> i32 {
    if cfg.is_null() {
        return -1;
    }
    unsafe {
        let mut c = *cfg;
        if c.lease_s == 0 {
            c.lease_s = 3600;
        }
        if c.t1_s == 0 {
            c.t1_s = c.lease_s / 2;
        }
        if c.t2_s == 0 {
            c.t2_s = (c.lease_s * 7) / 8;
        }
        let _ = crate::config::rust_net_set_ipv4_config(
            c.ip_host,
            c.netmask_host,
            c.gateway_host,
            c.dns_host,
        );
        record_smoltcp_lease(c);
    }
    0
}

#[no_mangle]
pub extern "C" fn rust_net_dhcp_start_daemon() {
    unsafe {
        let _ = ffi_kernel::create_task(dhcp_wakeup_daemon);
    }
}
