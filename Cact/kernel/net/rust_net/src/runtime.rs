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

extern "C" fn knetd() {
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
        ffi_kernel::c_kprint(b"[RUST-NET] driver registered, mac=\0");
        ffi_kernel::c_kprint_hex((my_mac.b[0] as u32) << 8 | my_mac.b[1] as u32);
        ffi_kernel::c_kprint(b":\0");
        ffi_kernel::c_kprint_hex((my_mac.b[2] as u32) << 8 | my_mac.b[3] as u32);
        ffi_kernel::c_kprint(b":\0");
        ffi_kernel::c_kprint_hex((my_mac.b[4] as u32) << 8 | my_mac.b[5] as u32);
        ffi_kernel::c_kprint(b"\n\0");
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
            ffi_kernel::c_kprint(b"[RUST-NET] driver unregistered\n\0");
        }
    }
}

#[no_mangle]
pub extern "C" fn net_init() {
    ffi_kernel::c_kprint(b"[RUST-NET] init begin\n\0");
    // SAFETY: globals are static and valid.
    unsafe {
        ffi_kernel::sema_init(core::ptr::addr_of_mut!(net_sema), 0);
        ffi_kernel::c_kprint(b"[RUST-NET] net_sema initialized\n\0");
        let _ = ffi_kernel::create_task(knetd);
        ffi_kernel::c_kprint(b"[RUST-NET] knetd task created\n\0");
        dhcp::rust_net_dhcp_start_daemon();
        ffi_kernel::c_kprint(b"[RUST-NET] dhcp daemon created\n\0");
        ffi_kernel::c_kprint(b"[RUST-NET] waiting NIC driver registration via net_register_driver()\n\0");
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
