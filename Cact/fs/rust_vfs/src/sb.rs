//! Superblock metadata: per-filesystem-type statfs numbers.
//!
//! A filesystem registers its numbers once at init
//! ([`vfs_sb_register`]); `statfs` on a path finds the mount that backs it,
//! looks the filesystem type up here, and fills a `cact_statfs_t`.  Numbers
//! default to a RAM-filesystem profile (block size 4096, name length 255, zero
//! block/inode counts) when a type never registers — pseudo filesystems report
//! zero blocks, exactly as they do on Linux.

use core::ffi::c_char;
use core::ptr;

const SB_MAX: usize = 16;

#[repr(C)]
struct SbEntry {
    used:   bool,
    fstype: [c_char; 32],
    bsize:  u32,
    blocks: u32,
    bfree:  u32,
    bavail: u32,
    files:  u32,
    ffree:  u32,
}

const SB_EMPTY: SbEntry = SbEntry {
    used:   false,
    fstype: [0; 32],
    bsize:  0,
    blocks: 0,
    bfree:  0,
    bavail: 0,
    files:  0,
    ffree:  0,
};

static mut SB: [SbEntry; SB_MAX] = [SB_EMPTY; SB_MAX];
static mut SB_COUNT: usize = 0;

#[inline]
unsafe fn cstr_eq(a: *const c_char, b: *const c_char) -> bool {
    let mut i = 0isize;
    // SAFETY: both are NUL-terminated C strings.
    unsafe {
        loop {
            let ca = *a.offset(i);
            let cb = *b.offset(i);
            if ca != cb {
                return false;
            }
            if ca == 0 {
                return true;
            }
            i += 1;
        }
    }
}

#[inline]
unsafe fn copy_name(dst: *mut c_char, src: *const c_char) {
    let mut i = 0usize;
    // SAFETY: `src` is NUL-terminated; `dst` has 32 bytes, we stop at 31.
    unsafe {
        while i + 1 < 32 && *src.add(i) != 0 {
            *dst.add(i) = *src.add(i);
            i += 1;
        }
        *dst.add(i) = 0;
    }
}

/// Register (or update) a filesystem type's statfs numbers.  Called at FS init.
#[no_mangle]
pub unsafe extern "C" fn vfs_sb_register(
    fstype: *const c_char,
    bsize: u32,
    blocks: u32,
    bfree: u32,
    bavail: u32,
    files: u32,
    ffree: u32,
) {
    if fstype.is_null() {
        return;
    }
    // SAFETY: single-threaded init-time registry.
    unsafe {
        for i in 0..SB_COUNT {
            let e = &mut *ptr::addr_of_mut!(SB[i]);
            if e.used && cstr_eq(e.fstype.as_ptr(), fstype) {
                e.bsize = bsize;
                e.blocks = blocks;
                e.bfree = bfree;
                e.bavail = bavail;
                e.files = files;
                e.ffree = ffree;
                return;
            }
        }
        if SB_COUNT < SB_MAX {
            let e = &mut *ptr::addr_of_mut!(SB[SB_COUNT]);
            e.used = true;
            copy_name(e.fstype.as_mut_ptr(), fstype);
            e.bsize = bsize;
            e.blocks = blocks;
            e.bfree = bfree;
            e.bavail = bavail;
            e.files = files;
            e.ffree = ffree;
            SB_COUNT += 1;
        }
    }
}

/// Fill a 6-word profile (bsize, blocks, bfree, bavail, files, ffree) for a
/// filesystem type.  Returns false and leaves `out` untouched when the type has
/// not registered (the caller then uses the RAM-filesystem defaults).
pub unsafe fn sb_lookup(fstype: *const c_char, out: *mut u32) -> bool {
    if fstype.is_null() || out.is_null() {
        return false;
    }
    // SAFETY: init-time-written registry, read-only here.
    unsafe {
        for i in 0..SB_COUNT {
            let e = &*ptr::addr_of!(SB[i]);
            if e.used && cstr_eq(e.fstype.as_ptr(), fstype) {
                *out.add(0) = e.bsize;
                *out.add(1) = e.blocks;
                *out.add(2) = e.bfree;
                *out.add(3) = e.bavail;
                *out.add(4) = e.files;
                *out.add(5) = e.ffree;
                return true;
            }
        }
    }
    false
}
