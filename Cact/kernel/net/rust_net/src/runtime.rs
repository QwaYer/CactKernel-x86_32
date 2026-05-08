use crate::ethernet;
use crate::ffi_kernel;
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
    }
}

#[no_mangle]
pub extern "C" fn net_init() {
    ffi_kernel::c_kprint(b"[RUST-NET] init\n\0");
    // SAFETY: globals are static and valid.
    unsafe {
        ffi_kernel::sema_init(core::ptr::addr_of_mut!(net_sema), 0);
        let _ = ffi_kernel::create_task(knetd);
        ffi_kernel::virtio_net_init();
    }
}

#[no_mangle]
pub extern "C" fn net_receive(skb: *mut Skb) {
    if skb.is_null() {
        return;
    }
    ethernet::ethernet_input(skb);
}

#[no_mangle]
pub extern "C" fn net_poll() {
    // SAFETY: active_nic is optional global.
    unsafe {
        if !active_nic.is_null() {
            if let Some(poll) = (*active_nic).poll {
                poll();
            }
        }
    }
}
