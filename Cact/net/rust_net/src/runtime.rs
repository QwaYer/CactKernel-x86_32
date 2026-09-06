//! Driver registration, background `net_poll_task` poll thread, and RX entry from the NIC driver.
//!
//! `net_sema` serializes access between the interrupt/RX path and `net_poll_task`.

use crate::ffi_kernel;
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
            ffi_kernel::down(core::ptr::addr_of_mut!(net_sema));
        }
        net_poll();
    }
}

/// Periodic timer kick.  smoltcp timers (TCP retransmission, connection timeouts,
/// UDP/ARP caches) only advance while `stack_poll` runs, so a task wakes the poll
/// loop on a fixed cadence even when no NIC IRQ has fired.  The kernel DHCP client
/// that used to drive this wake-up is gone; the poll loop itself stays.
extern "C" fn net_timer_task() {
    loop {
        // SAFETY: sleep is a plain kernel service; semaphore is kernel-lifetime.
        unsafe {
            ffi_kernel::sched_sleep_ticks(NET_POLL_PERIOD_TICKS);
            ffi_kernel::up(core::ptr::addr_of_mut!(net_sema));
        }
    }
}

/// Poll cadence for the timer task (ticks, 10 ms/tick => 100 ms).
const NET_POLL_PERIOD_TICKS: u32 = 10;

#[no_mangle]
pub extern "C" fn register_netdev(drv: *mut NetDriver) {
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
pub extern "C" fn unregister_netdev(drv: *mut NetDriver) {
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

/// Link state for userspace network managers: 1 while a NIC driver is registered.
#[no_mangle]
pub extern "C" fn rust_net_link_is_up() -> i32 {
    if unsafe { active_nic.is_null() } {
        0
    } else {
        1
    }
}

/// Copy the registered NIC's MAC address into `out[6]`.  Returns 0 on success,
/// -1 when no NIC is registered or `out` is NULL.
#[no_mangle]
pub extern "C" fn rust_net_get_mac(out: *mut u8) -> i32 {
    if out.is_null() {
        return -1;
    }
    unsafe {
        if active_nic.is_null() {
            return -1;
        }
        core::ptr::copy_nonoverlapping(my_mac.b.as_ptr(), out, 6);
    }
    0
}

#[no_mangle]
pub extern "C" fn net_init() {
    // SAFETY: globals are static and valid.
    unsafe {
        ffi_kernel::sema_init(core::ptr::addr_of_mut!(net_sema), 0);
        let _ = ffi_kernel::create_task(net_poll_task);
        let _ = ffi_kernel::create_task(net_timer_task);
        ffi_kernel::klog_static(
            ffi_kernel::LOG_OK,
            b"  net         : ready (net_poll_task, RX semaphore, timer kick)\0",
        );
    }
}

#[no_mangle]
pub extern "C" fn netif_rx(skb: *mut Skb) {
    stack::stack_enqueue_rx(skb);
}

/// Stable alias exported for loadable NIC driver modules (see `net_shim.c`).
#[no_mangle]
pub extern "C" fn net_receive_packet(skb: *mut Skb) {
    netif_rx(skb);
}

/// Wake `net_poll_task` after a NIC RX IRQ (stable alias for driver modules).
#[no_mangle]
pub extern "C" fn net_driver_irq_wake() {
    // SAFETY: net_sema is a kernel-lifetime static.
    unsafe {
        ffi_kernel::up(core::ptr::addr_of_mut!(net_sema));
    }
}

#[no_mangle]
pub extern "C" fn net_poll() {
    stack::stack_poll();
}
