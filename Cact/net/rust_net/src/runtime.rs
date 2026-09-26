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
    guard: cact_sync::spinlock_t::new(),
    count: core::sync::atomic::AtomicI32::new(0),
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
        // SAFETY: `sched_sleep_ticks` is a kernel service that suspends the
        // calling task for a whole number of ticks; it is safe from task
        // context.
        unsafe { sched::timer_wheel::sched_sleep_ticks(NET_POLL_PERIOD_TICKS) };
        // SAFETY: `net_sema` is a kernel-lifetime semaphore initialised by
        // `net_init`; `up` is the matching kernel C release service.
        unsafe { ffi_kernel::up(core::ptr::addr_of_mut!(net_sema)) };
    }
}

/// Poll cadence for the timer task (ticks, 10 ms/tick => 100 ms).
const NET_POLL_PERIOD_TICKS: u32 = 10;

/// # Safety
///
/// `drv` must be non-null and point to a live [`NetDriver`] describing the NIC
/// that is being registered — the `net_driver_t` the C driver keeps for the
/// lifetime of the device.  Its `mac`, `get_mac` and `send`/`poll` fields must
/// be initialised (function pointers either valid or NULL), because this
/// function reads them and stores `drv` in the crate-wide `active_nic`, which
/// every later TX/RX path dereferences.
#[no_mangle]
pub unsafe extern "C" fn register_netdev(drv: *mut NetDriver) {
    if drv.is_null() {
        return;
    }
    // SAFETY: the caller contract (see # Safety) makes `drv` a valid pointer to
    // a live `NetDriver` whose fields are initialised; this borrow ends before
    // the calls below, so nothing mutates it while it is live.
    let d = unsafe { &*drv };
    let get_mac = d.get_mac;
    let mac = d.mac;
    // SAFETY: `active_nic` is a kernel-lifetime `static mut` pointer written
    // only here and in `unregister_netdev`, on the single-threaded
    // driver-registration path.
    unsafe { active_nic = drv };
    if let Some(get_mac) = get_mac {
        // SAFETY: `get_mac` is the driver's own MAC accessor and `my_mac` is a
        // kernel-lifetime 6-byte static, so the pointer it writes into is live
        // and correctly sized.
        get_mac(core::ptr::addr_of_mut!(my_mac));
    }
    // SAFETY: `my_mac` is a kernel-lifetime static written only here and in
    // `unregister_netdev`, on the same single-threaded path.
    unsafe { my_mac = mac };
    stack::stack_init();
    ffi_kernel::klog_static(
        ffi_kernel::LOG_OK,
        b"NIC driver registered; L3 stack initialized\0",
    );
}

/// Clear `active_nic` only if it still points at `drv` (symmetric to registration).
#[no_mangle]
pub extern "C" fn unregister_netdev(drv: *mut NetDriver) {
    if drv.is_null() {
        return;
    }
    // SAFETY: `active_nic` is a kernel-lifetime `static mut` pointer read on the
    // driver's teardown path, where no other CPU is using the interface; an
    // aligned pointer load cannot tear.
    let registered = unsafe { active_nic == drv };
    if registered {
        // SAFETY: `active_nic` is a kernel-lifetime `static mut` pointer written
        // only here and in `register_netdev`, on the same single-threaded path.
        unsafe { active_nic = core::ptr::null_mut() };
        // SAFETY: as above, for the kernel-lifetime `my_mac` static.
        unsafe { my_mac = MacAddr { b: [0; 6] } };
        // SAFETY: `stack_teardown` tears the L3 stack down; the caller contract
        // (this runs on the single-threaded driver teardown path) makes that the
        // exclusive accessor at this point.
        unsafe { stack::stack_teardown() };
    }
}

/// Link state for userspace network managers: 1 while a NIC driver is registered.
#[no_mangle]
pub extern "C" fn rust_net_link_is_up() -> i32 {
    // SAFETY: `active_nic` is a kernel-lifetime `static mut` pointer; an aligned
    // pointer-sized load cannot tear, and `is_null` only inspects the value.
    if unsafe { active_nic.is_null() } {
        0
    } else {
        1
    }
}

/// Copy the registered NIC's MAC address into `out[6]`.  Returns 0 on success,
/// -1 when no NIC is registered or `out` is NULL.
///
/// # Safety
///
/// `out` may be null (the call then returns -1), but if non-null it must point
/// to at least 6 writable bytes that stay live for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn rust_net_get_mac(out: *mut u8) -> i32 {
    if out.is_null() {
        return -1;
    }
    // SAFETY: `active_nic` is a kernel-lifetime `static mut` pointer read from a
    // syscall path; an aligned pointer load cannot tear.
    if unsafe { active_nic.is_null() } {
        return -1;
    }
    // SAFETY: `addr_of!` forms a pointer to the live `my_mac.b` static field
    // without creating a reference; `my_mac` is a kernel-lifetime static that no
    // other context writes on this path.
    let src = unsafe { core::ptr::addr_of!(my_mac.b).cast::<u8>() };
    // SAFETY: `src` points to the live 6-byte `my_mac.b` array; `out` is the
    // caller's 6-byte writable buffer (the null check above excluded null), and
    // it comes from the caller so it cannot overlap the static.
    unsafe { core::ptr::copy_nonoverlapping(src, out, 6) };
    0
}

#[no_mangle]
pub extern "C" fn net_init() {
    // SAFETY: `net_sema` is a kernel-lifetime semaphore static; `sema_init`
    // initialises it once, before any task can wait on it (this runs from the
    // single-threaded driver-registration path).
    unsafe { ffi_kernel::sema_init(core::ptr::addr_of_mut!(net_sema), 0) };
    // SAFETY: `create_task` spawns a kernel task from the given (valid, `'static`)
    // entry point; it is called here on the same single-threaded path.
    unsafe { let _ = sched::task::create_task(net_poll_task as *const core::ffi::c_void); }
    // SAFETY: as above, for the periodic timer task.
    unsafe { let _ = sched::task::create_task(net_timer_task as *const core::ffi::c_void); }
    ffi_kernel::klog_static(
        ffi_kernel::LOG_OK,
        b"  net         : ready (net_poll_task, RX semaphore, timer kick)\0",
    );
}

/// Hand a received frame to the stack.  Called by NIC drivers (and the
/// `net_receive_packet` alias) from their RX path.
///
/// # Safety
///
/// `skb` must be null or a live, initialised [`Skb`] the caller hands over: the
/// stack either queues or frees it exactly once, so the caller must not touch it
/// afterwards.
#[no_mangle]
pub unsafe extern "C" fn netif_rx(skb: *mut Skb) {
    // SAFETY: this only forwards `skb`; the caller contract above is what makes
    // `stack_enqueue_rx`'s own `# Safety` requirement hold.
    unsafe {
        stack::stack_enqueue_rx(skb);
    }
}

/// Stable alias exported for loadable NIC driver modules (see `net_shim.c`).
///
/// # Safety
///
/// As [`netif_rx`]: `skb` must be null or a live, initialised [`Skb`] that this
/// call takes ownership of.
#[no_mangle]
pub unsafe extern "C" fn net_receive_packet(skb: *mut Skb) {
    // SAFETY: this only forwards `skb`; the caller contract above is what makes
    // `netif_rx`'s own `# Safety` requirement hold.
    unsafe {
        netif_rx(skb);
    }
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
