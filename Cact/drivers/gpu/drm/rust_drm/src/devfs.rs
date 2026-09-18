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
    fn kmalloc(size: u32) -> *mut c_void;
    fn kfree(ptr: *mut c_void);
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
    // SAFETY: walks the device table and compares against our own nodes.
    unsafe {
        let t = device_table();
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
            fmt_indexed(card.as_mut_ptr(), 16, b"card", (*dev).minor);
            fmt_indexed(render.as_mut_ptr(), 16, b"renderD", 128 + (*dev).minor);
            if cstr_eq(name, card.as_ptr()) {
                return (*dev).card_node as *mut VfsNode;
            }
            if (*dev).have_render_node != 0 && cstr_eq(name, render.as_ptr()) {
                return (*dev).render_node as *mut VfsNode;
            }
        }
    }
    core::ptr::null_mut()
}

extern "C" fn drm_dri_readdir(_priv: *mut c_void, index: u32) -> *mut VfsDirent {
    // SAFETY: walks the device table; `DE` is our private scratch entry.
    unsafe {
        let t = device_table();
        let de = core::ptr::addr_of_mut!(DE);
        let mut i = 0u32;
        for slot in t.iter() {
            let dev = match slot {
                Some(d) => *d,
                None => continue,
            };
            if dev.is_null() {
                continue;
            }
            if i == index {
                fmt_indexed((*de).name.as_mut_ptr(), 128, b"card", (*dev).minor);
                (*de).inode = (*dev).minor as u32;
                return de;
            }
            i += 1;
            if (*dev).have_render_node != 0 {
                if i == index {
                    fmt_indexed((*de).name.as_mut_ptr(), 128, b"renderD", 128 + (*dev).minor);
                    (*de).inode = 128u32 + (*dev).minor as u32;
                    return de;
                }
                i += 1;
            }
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
    // SAFETY: one-time registration of the global driver table.
    unsafe {
        if READY != 0 {
            return 0;
        }
        if register_chrdev(
            b"dri\0".as_ptr(),
            DEVFS_F_DIR,
            core::ptr::addr_of_mut!(DRIVER),
            core::ptr::null_mut(),
        )
        .is_null()
        {
            crate::ffi::printk(b"\x013  drm         : cannot register /dev/dri\n\0".as_ptr());
            return -1;
        }
        READY = 1;
    }
    0
}

#[no_mangle]
pub extern "C" fn drm_devfs_add_device(dev: *mut DrmDevice) -> c_int {
    if dev.is_null() {
        return -1;
    }
    // SAFETY: caller's device; the nodes it gets are ours.
    unsafe {
        let mut name = [0u8; 32];

        let card = kmalloc(core::mem::size_of::<VfsNode>() as u32) as *mut VfsNode;
        if card.is_null() {
            return -12;
        }
        core::ptr::write_bytes(card, 0, 1);
        fmt_indexed(name.as_mut_ptr(), 32, b"card", (*dev).minor);
        drm_fill_node(card, name.as_ptr(), dev);
        (*dev).card_node = card;

        /* A render node is published for every device: Mesa's virgl and GBM
         * both prefer it when they only need GEM, and it can never become
         * master. */
        let render = kmalloc(core::mem::size_of::<VfsNode>() as u32) as *mut VfsNode;
        if render.is_null() {
            kfree(card as *mut c_void);
            (*dev).card_node = core::ptr::null_mut();
            return -12;
        }
        core::ptr::write_bytes(render, 0, 1);
        fmt_indexed(name.as_mut_ptr(), 32, b"renderD", 128 + (*dev).minor);
        drm_fill_node(render, name.as_ptr(), dev);
        (*dev).render_node = render;
        (*dev).have_render_node = 1;
    }
    0
}
