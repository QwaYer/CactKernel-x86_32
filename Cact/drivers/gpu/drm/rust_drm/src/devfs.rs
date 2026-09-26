//! `/dev/dri` registration: a devfs directory driver that walks and lists
//! `cardN` / `renderDN` and owns their `vfs_node_t`.
//!
//! Migrated slice 14 — `core/drm_devfs.c`.

use core::ffi::{c_int, c_void};

use crate::device::device_table;
use crate::file::drm_fill_node;
use crate::structs::DrmDevice;
use crate::vfs::{
    cstr_eq, fmt_indexed, register_chrdev, DevfsDriver, VfsDirent, VfsNode, DEVFS_F_DIR,
};

extern "C" {

}

static mut READY: c_int = 0;

/// The `readdir` scratch entry devfs returns a pointer to, as in C.
static mut DE: VfsDirent = VfsDirent {
    name: [0u8; 128],
    inode: 0,
};

extern "C" fn drm_dri_walk(_priv: *mut c_void, name: *const u8) -> *mut VfsNode {
    if name.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: `device_table()` returns this driver's devfs device table, and the
    // devfs walker runs under the devfs lock, so it is safe to read it here.
    let t = unsafe { device_table() };
    let mut card = [0u8; 16];
    let mut render = [0u8; 16];
    for slot in t.iter() {
        let dev = match slot {
            Some(d) => *d,
            None => continue,
        };
        if dev.is_null() {
            continue;
        }
        // SAFETY: `dev` is a live device in the table; this shared borrow is
        // consumed by the field reads below.
        let dev_ref = unsafe { &*dev };
        // SAFETY: `card` is a 16-byte stack array and `fmt_indexed` requires
        // `cap >= 1`, which 16 satisfies.
        unsafe { fmt_indexed(card.as_mut_ptr(), 16, b"card", dev_ref.minor) };
        // SAFETY: as above — `render` is a 16-byte stack array and `cap` is 16.
        unsafe { fmt_indexed(render.as_mut_ptr(), 16, b"renderD", 128 + dev_ref.minor) };
        // SAFETY: `name` is a NUL-terminated string (checked non-null above) and
        // `card` is a NUL-terminated stack buffer.
        if unsafe { cstr_eq(name, card.as_ptr()) } {
            return dev_ref.card_node;
        }
        // SAFETY: as above — `render` is a NUL-terminated stack buffer.
        if dev_ref.have_render_node != 0 && unsafe { cstr_eq(name, render.as_ptr()) } {
            return dev_ref.render_node;
        }
    }
    core::ptr::null_mut()
}

extern "C" fn drm_dri_readdir(_priv: *mut c_void, index: u32) -> *mut VfsDirent {
    // SAFETY: `device_table()` returns this driver's devfs device table, read under
    // the devfs lock.
    let t = unsafe { device_table() };
    // `addr_of_mut!` forms a pointer to the private `DE` scratch entry without
    // creating a reference, which needs no `unsafe` on its own.
    let de_ptr = core::ptr::addr_of_mut!(DE);
    // SAFETY: `de_ptr` addresses the live `DE` global, so this exclusive borrow is
    // valid for the whole walk.
    let de = unsafe { &mut *de_ptr };
    let mut i = 0u32;
    for slot in t.iter() {
        let dev = match slot {
            Some(d) => *d,
            None => continue,
        };
        if dev.is_null() {
            continue;
        }
        // SAFETY: `dev` is a live device in the table; this shared borrow is
        // consumed by the field reads below.
        let dev_ref = unsafe { &*dev };
        if i == index {
            // SAFETY: `de.name` is a 128-byte array and `cap` is 128.
            unsafe { fmt_indexed(de.name.as_mut_ptr(), 128, b"card", dev_ref.minor) };
            de.inode = dev_ref.minor as u32;
            return de as *mut VfsDirent;
        }
        i += 1;
        if dev_ref.have_render_node != 0 {
            if i == index {
                // SAFETY: `de.name` is a 128-byte array and `cap` is 128.
                unsafe { fmt_indexed(de.name.as_mut_ptr(), 128, b"renderD", 128 + dev_ref.minor) };
                de.inode = 128u32 + dev_ref.minor as u32;
                return de as *mut VfsDirent;
            }
            i += 1;
        }
    }
    core::ptr::null_mut()
}

static mut DRIVER: DevfsDriver = DevfsDriver {
    before: [core::ptr::null_mut(); 5],
    walk: Some(drm_dri_walk),
    readdir: Some(drm_dri_readdir),
};

#[no_mangle]
pub extern "C" fn drm_devfs_init() -> c_int {
    // SAFETY: `READY` is this driver's one-time init flag.
    if unsafe { READY } != 0 {
        return 0;
    }
    // `addr_of_mut!` forms a pointer to the static `DRIVER` table without creating
    // a reference, which needs no `unsafe` on its own.
    let driver = core::ptr::addr_of_mut!(DRIVER);
    // SAFETY: `register_chrdev` is the devfs C service that records the driver
    // table; `driver` is the live static table and `devfs_lock` serialises this
    // one-time registration.
    if unsafe {
        register_chrdev(
            c"dri".as_ptr() as *const u8,
            DEVFS_F_DIR,
            driver,
            core::ptr::null_mut(),
        )
    }
    .is_null()
    {
        // SAFETY: static NUL-terminated string.
        unsafe {
            crate::ffi::printk(
                c"\x013  drm         : cannot register /dev/dri\n".as_ptr() as *const u8,
            );
        }
        return -1;
    }
    // SAFETY: as above — publishing the one-time init flag.
    unsafe { READY = 1 };
    0
}

#[no_mangle]
pub extern "C" fn drm_devfs_add_device(dev: *mut DrmDevice) -> c_int {
    if dev.is_null() {
        return -1;
    }
    let mut name = [0u8; 32];

    let card = cact_mm::kmalloc(core::mem::size_of::<VfsNode>() as u32) as *mut VfsNode;
    if card.is_null() {
        return -12;
    }
    // SAFETY: `card` is the fresh `kmalloc` node just checked non-null, so zeroing
    // one `VfsNode` covers exactly that allocation.
    unsafe { core::ptr::write_bytes(card, 0, 1) };
    // SAFETY: `dev` is the live device being registered (checked non-null above), so
    // this `minor` read is in bounds.
    let minor = unsafe { (*dev).minor };
    // SAFETY: `name` is a 32-byte stack array and `cap` is 32.
    unsafe { fmt_indexed(name.as_mut_ptr(), 32, b"card", minor) };
    drm_fill_node(card, name.as_ptr(), dev);
    // SAFETY: recording the new card node on the live device.
    unsafe { (*dev).card_node = card };

    /* A render node is published for every device: Mesa's virgl and GBM
     * both prefer it when they only need GEM, and it can never become
     * master. */
    let render = cact_mm::kmalloc(core::mem::size_of::<VfsNode>() as u32) as *mut VfsNode;
    if render.is_null() {
        // SAFETY: `card` was allocated by `kmalloc` above and has not been
        // published, so freeing it here is valid.
        unsafe { cact_mm::kfree(card as *mut u8) };
        // SAFETY: rolling back the card-node pointer on the live device.
        unsafe { (*dev).card_node = core::ptr::null_mut() };
        return -12;
    }
    // SAFETY: `render` is the fresh `kmalloc` node just checked non-null, so zeroing
    // one `VfsNode` covers exactly that allocation.
    unsafe { core::ptr::write_bytes(render, 0, 1) };
    // SAFETY: `name` is a 32-byte stack array and `cap` is 32.
    unsafe { fmt_indexed(name.as_mut_ptr(), 32, b"renderD", 128 + minor) };
    drm_fill_node(render, name.as_ptr(), dev);
    // SAFETY: recording the new render node on the live device.
    unsafe { (*dev).render_node = render };
    // SAFETY: as above — marking that the device has a render node.
    unsafe { (*dev).have_render_node = 1 };
    0
}
