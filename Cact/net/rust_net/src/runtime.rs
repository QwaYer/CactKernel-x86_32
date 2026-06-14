//! Driver registration, background `net_poll_task` poll thread, and RX entry from the NIC driver.
//!
//! `net_sema` serializes access between the interrupt/RX path and `net_poll_task`.

use crate::ffi_kernel;
use crate::dhcp;
use crate::stack;
use crate::types::{MacAddr, NetDriver, Semaphore, Skb};

#[no_mangle]
pub static mut active_nic: *mut NetDriver = core::ptr::null_mut();
#[no_mangle]
pub static mut my_mac: MacAddr = MacAddr { b: [0; 6] };
#[no_mangle]
pub static mut net_sema: Semaphore = Semaphore {
    guard: crate::types::Spinlock { locked: 0 },
    waiters: [core::ptr::null_mut(); 64],
    waiter_count: 0,
};

extern "C" fn net_poll_task() {
    loop {
        // SAFETY: semaphore lives for kernel lifetime.
        unsafe {
            ffi_kernel::sema_down(core::ptr::addr_of_mut!(net_sema));
        }
        net_poll();
    }
}

#[no_mangle]
pub extern "C" fn net_register_driver(drv: *mut NetDriver) {
    if drv.is_null() {
        return;
    }
    // SAFETY: driver pointer provided by C virtio driver.
    unsafe {
        active_nic = drv;
        if let Some(get_mac) = (*drv).get_mac {
            get_mac(core::ptr::addr_of_mut!(my_mac));
        }
        my_mac = (*drv).mac;
        stack::stack_init();
        ffi_kernel::klog_static(
            ffi_kernel::LOG_OK,
            b"NIC driver registered; L3 stack initialized\0",
        );
    }
}

/// Clear `active_nic` only if it still points at `drv` (symmetric to registration).
#[no_mangle]
pub extern "C" fn net_unregister_driver(drv: *mut NetDriver) {
    if drv.is_null() {
        return;
    }
    unsafe {
        if active_nic == drv {
            active_nic = core::ptr::null_mut();
            my_mac = MacAddr { b: [0; 6] };
            stack::stack_teardown();
        }
    }
}

#[no_mangle]
pub extern "C" fn net_init() {
    // SAFETY: globals are static and valid.
    unsafe {
        ffi_kernel::sema_init(core::ptr::addr_of_mut!(net_sema), 0);
        let _ = ffi_kernel::create_task(net_poll_task);
        dhcp::rust_net_dhcp_start_daemon();
        ffi_kernel::klog_static(
            ffi_kernel::LOG_OK,
            b"Network subsystem ready (net_poll_task, RX semaphore, DHCP)\0",
        );
    }
}

#[no_mangle]
pub extern "C" fn net_receive(skb: *mut Skb) {
    stack::stack_enqueue_rx(skb);
}

#[no_mangle]
pub extern "C" fn net_poll() {
    stack::stack_poll();
}
