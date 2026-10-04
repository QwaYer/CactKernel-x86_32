//! Generic inode address space: a per-file page cache any filesystem can use.
//!
//! An address space is identified by `(owner, ino)` — the filesystem instance
//! (`node->priv`, stable across lookups) plus the inode number — *not* by the
//! `vfs_node_t` pointer, which some filesystems (ext4) allocate per walk.
//!
//! Objects are memfd page arrays, so a `MAP_SHARED` mmap of the file maps the
//! same frames read()/write() use.  A filesystem with its own backing store
//! (ext4) registers raw read/write callbacks: pages are cold-populated from
//! the store on first read and written through on write (and flushed from the
//! mapping on munmap).  A RAM filesystem (tmpfs) registers no callbacks — the
//! page array *is* the storage.

use core::ffi::{c_char, c_void};
use core::ptr;

const AS_MAX: usize = 128;
const AS_MAX_PAGES: usize = 1024;
const BITMAP_BYTES: usize = AS_MAX_PAGES / 8;
const PAGE: usize = 4096;

/// Raw backing I/O: read/write `size` bytes at file offset `off` for
/// `(owner, ino)`.  Returns the byte count, or negative on error.
pub type BackingFn = unsafe extern "C" fn(*mut c_void, u32, u32, u32, *mut c_char) -> i32;

struct AddrSpace {
    used:    bool,
    owner:   *mut c_void,
    ino:     u32,
    mfd:     i32,
    size:    u32,
    present: [u8; BITMAP_BYTES],
    dirty:   [u8; BITMAP_BYTES],
    rfn:     Option<BackingFn>,
    wfn:     Option<BackingFn>,
}

const AS_EMPTY: AddrSpace = AddrSpace {
    used:    false,
    owner:   ptr::null_mut(),
    ino:     0,
    mfd:     0,
    size:    0,
    present: [0; BITMAP_BYTES],
    dirty:   [0; BITMAP_BYTES],
    rfn:     None,
    wfn:     None,
};

static mut AS: [AddrSpace; AS_MAX] = [AS_EMPTY; AS_MAX];
static mut AS_COUNT: usize = 0;
// Lightweight counters (objects live, page populates, page flushes).
static mut AS_OBJECTS: u32 = 0;
static mut AS_POPULATES: u32 = 0;
static mut AS_FLUSHES: u32 = 0;

unsafe extern "C" {
    fn memfd_create(name: *const u8, name_len: u32, flags: i32) -> i32;
    fn memfd_ref(handle: i32) -> i32;
    fn memfd_close(handle: i32) -> i32;
    fn memfd_read(handle: i32, off: u32, buf: *mut u8, size: u32) -> i32;
    fn memfd_write(handle: i32, off: u32, buf: *const u8, size: u32) -> i32;
    fn memfd_truncate(handle: i32, new_size: u32) -> i32;
    fn memfd_size(handle: i32) -> i32;
    fn memfd_get_page(handle: i32, idx: u32) -> *mut u8;
}

#[inline]
fn bit(bm: &[u8; BITMAP_BYTES], i: usize) -> bool {
    bm[i / 8] & (1 << (i % 8)) != 0
}

#[inline]
fn set_bit(bm: &mut [u8; BITMAP_BYTES], i: usize) {
    bm[i / 8] |= 1 << (i % 8);
}

#[inline]
fn clear_bit(bm: &mut [u8; BITMAP_BYTES], i: usize) {
    bm[i / 8] &= !(1 << (i % 8));
}

unsafe fn as_of(owner: *mut c_void, ino: u32) -> *mut AddrSpace {
    // SAFETY: fixed static table; linear scan / append.
    unsafe {
        let f = as_find(owner, ino);
        if !f.is_null() {
            return f;
        }
        if AS_COUNT >= AS_MAX {
            return ptr::null_mut();
        }
        let h = memfd_create(ptr::null(), 0, 0);
        if h <= 0 {
            return ptr::null_mut();
        }
        memfd_ref(h);
        let a = &mut *ptr::addr_of_mut!(AS[AS_COUNT]);
        a.used = true;
        a.owner = owner;
        a.ino = ino;
        a.mfd = h;
        a.size = 0;
        AS_COUNT += 1;
        AS_OBJECTS += 1;
        ptr::addr_of_mut!(AS[AS_COUNT - 1])
    }
}

/// Find an existing address space without creating one.
unsafe fn as_find(owner: *mut c_void, ino: u32) -> *mut AddrSpace {
    // SAFETY: fixed static table; linear scan.
    unsafe {
        for i in 0..AS_COUNT {
            let a = &*ptr::addr_of!(AS[i]);
            if a.used && a.owner == owner && a.ino == ino {
                return ptr::addr_of_mut!(AS[i]);
            }
        }
    }
    ptr::null_mut()
}

/// Register the raw backing read/write callbacks for `(owner, ino)`.
#[no_mangle]
pub unsafe extern "C" fn vfs_as_set_backing(
    owner: *mut c_void,
    ino: u32,
    rfn: Option<BackingFn>,
    wfn: Option<BackingFn>,
) {
    let a = unsafe { as_of(owner, ino) };
    if a.is_null() {
        return;
    }
    // SAFETY: `a` is a live table entry.
    unsafe {
        (*a).rfn = rfn;
        (*a).wfn = wfn;
    }
}

/// Set the logical file size (grows the backing memfd as needed).
#[no_mangle]
pub unsafe extern "C" fn vfs_as_setsize(owner: *mut c_void, ino: u32, size: u32) {
    let a = unsafe { as_of(owner, ino) };
    if a.is_null() {
        return;
    }
    // SAFETY: `a` is live.
    unsafe {
        if size > (*a).size {
            let cur = memfd_size((*a).mfd);
            if (size as i32) > cur {
                memfd_truncate((*a).mfd, size);
            }
        }
        (*a).size = size;
    }
}

/// Current logical size, or -1.
#[no_mangle]
pub unsafe extern "C" fn vfs_as_size(owner: *mut c_void, ino: u32) -> i32 {
    let a = unsafe { as_of(owner, ino) };
    if a.is_null() {
        return -1;
    }
    // SAFETY: `a` is live.
    unsafe { (*a).size as i32 }
}

/// Populate any missing pages of `[off, off+size)` from the backing read.
unsafe fn populate(a: *mut AddrSpace, off: u32, size: u32) {
    // SAFETY: `a` is live; the memfd and frames stay alive under no eviction.
    unsafe {
        let h = (*a).mfd;
        let first = (off as usize) / PAGE;
        let last = ((off + size - 1) as usize) / PAGE;
        for p in first..=last {
            if p >= AS_MAX_PAGES || bit(&(*a).present, p) {
                continue;
            }
            if let Some(rfn) = (*a).rfn {
                // Ensure the frame exists, then let the filesystem fill it.
                let need = ((p + 1) * PAGE) as u32;
                if (need as i32) > memfd_size(h) {
                    memfd_truncate(h, need);
                }
                let page = memfd_get_page(h, p as u32);
                if !page.is_null() {
                    rfn((*a).owner, (*a).ino, (p * PAGE) as u32, PAGE as u32, page as *mut c_char);
                }
            }
            set_bit(&mut (*a).present, p);
            AS_POPULATES += 1;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_as_read(
    owner: *mut c_void,
    ino: u32,
    off: u32,
    size: u32,
    buf: *mut c_char,
) -> i32 {
    if size == 0 {
        return 0;
    }
    let a = unsafe { as_of(owner, ino) };
    if a.is_null() {
        return -1;
    }
    // SAFETY: `a` is live.
    let fsz = unsafe { (*a).size };
    if off >= fsz {
        return 0;
    }
    let n = if size > fsz - off { fsz - off } else { size };
    unsafe { populate(a, off, n) };
    // SAFETY: `a` is live and `buf` is the caller's buffer.
    unsafe { memfd_read((*a).mfd, off, buf as *mut u8, n) }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_as_write(
    owner: *mut c_void,
    ino: u32,
    off: u32,
    size: u32,
    buf: *mut c_char,
) -> i32 {
    if size == 0 {
        return 0;
    }
    let a = unsafe { as_of(owner, ino) };
    if a.is_null() {
        return -1;
    }
    // SAFETY: `a` is live.
    let w = unsafe { memfd_write((*a).mfd, off, buf as *const u8, size) };
    if w <= 0 {
        return w;
    }
    let end = off + w as u32;
    // SAFETY: `a` is live; mark the touched pages present + dirty.
    unsafe {
        if end > (*a).size {
            (*a).size = end;
        }
        let first = (off as usize) / PAGE;
        let last = ((end - 1) as usize) / PAGE;
        for p in first..=last {
            if p < AS_MAX_PAGES {
                set_bit(&mut (*a).present, p);
                set_bit(&mut (*a).dirty, p);
            }
        }
        // Write through to the backing store so normal write() is durable.
        if let Some(wfn) = (*a).wfn {
            let r = wfn((*a).owner, (*a).ino, off, w as u32, buf);
            if r == w {
                for p in first..=last {
                    if p < AS_MAX_PAGES {
                        clear_bit(&mut (*a).dirty, p);
                    }
                }
            }
        }
    }
    w
}

#[no_mangle]
pub unsafe extern "C" fn vfs_as_truncate(owner: *mut c_void, ino: u32, length: u32) -> i32 {
    let a = unsafe { as_of(owner, ino) };
    if a.is_null() {
        return -1;
    }
    // SAFETY: `a` is live.
    unsafe {
        if ((length + PAGE as u32 - 1) / PAGE as u32) as usize > AS_MAX_PAGES {
            return -1;
        }
        if length > (*a).size {
            memfd_truncate((*a).mfd, length);
        } else {
            memfd_truncate((*a).mfd, length);
        }
        (*a).size = length;
    }
    0
}

/// Flush the pages of `[off, off+size)` to the backing store (if any).
#[no_mangle]
pub unsafe extern "C" fn vfs_as_flush(owner: *mut c_void, ino: u32, off: u32, size: u32) {
    let a = unsafe { as_find(owner, ino) };
    if a.is_null() {
        return;
    }
    // SAFETY: `a` is live.
    unsafe {
        let wfn = match (*a).wfn {
            Some(f) => f,
            None => return,
        };
        let fsz = (*a).size;
        if off >= fsz || size == 0 {
            return;
        }
        let end = core::cmp::min(off + size, fsz);
        let first = (off as usize) / PAGE;
        let last = ((end - 1) as usize) / PAGE;
        for p in first..=last {
            if p >= AS_MAX_PAGES || !bit(&(*a).present, p) {
                continue;
            }
            let page_off = (p * PAGE) as u32;
            let chunk = core::cmp::min(PAGE as u32, fsz - page_off);
            let page = memfd_get_page((*a).mfd, p as u32);
            if !page.is_null() {
                wfn((*a).owner, (*a).ino, page_off, chunk, page as *mut c_char);
            }
            clear_bit(&mut (*a).dirty, p);
            AS_FLUSHES += 1;
        }
    }
}

/// Flush every dirty page of every address space (call at unmount / shutdown).
#[no_mangle]
pub unsafe extern "C" fn vfs_as_flush_all() {
    // SAFETY: fixed static table; flush is idempotent per object.
    unsafe {
        for i in 0..AS_COUNT {
            let a = &*ptr::addr_of!(AS[i]);
            if a.used && a.wfn.is_some() && a.size > 0 {
                vfs_as_flush(a.owner, a.ino, 0, a.size);
            }
        }
    }
}

/// Report `[live objects, page populates, page flushes, capacity]`.
#[no_mangle]
pub unsafe extern "C" fn vfs_as_stats(out: *mut u32) {
    if out.is_null() {
        return;
    }
    // SAFETY: `out` has 4 writable words.
    unsafe {
        *out = AS_OBJECTS;
        *out.add(1) = AS_POPULATES;
        *out.add(2) = AS_FLUSHES;
        *out.add(3) = AS_MAX as u32;
    }
}

/// Resolve mmap backing: the address space's memfd handle.
#[no_mangle]
pub unsafe extern "C" fn vfs_as_backing(
    owner: *mut c_void,
    ino: u32,
    off: u32,
    _len: u32,
    backing: *mut i32,
    obj_off: *mut u32,
) -> i32 {
    if backing.is_null() || obj_off.is_null() {
        return -1;
    }
    let a = unsafe { as_of(owner, ino) };
    if a.is_null() {
        return -1;
    }
    // SAFETY: out-pointers non-null; `a` live.
    unsafe {
        *backing = (*a).mfd;
        *obj_off = off;
    }
    0
}

/// Flush then release the address space for `(owner, ino)`.
#[no_mangle]
pub unsafe extern "C" fn vfs_as_release(owner: *mut c_void, ino: u32) {
    // SAFETY: fixed static table.
    unsafe {
        for i in 0..AS_COUNT {
            let a = &mut *ptr::addr_of_mut!(AS[i]);
            if a.used && a.owner == owner && a.ino == ino {
                if a.wfn.is_some() && a.size > 0 {
                    vfs_as_flush(owner, ino, 0, a.size);
                }
                if a.mfd > 0 {
                    memfd_close(a.mfd);
                    a.mfd = 0;
                }
                a.used = false;
                a.owner = ptr::null_mut();
                a.ino = 0;
                a.size = 0;
                a.rfn = None;
                a.wfn = None;
                if AS_OBJECTS > 0 { AS_OBJECTS -= 1; }
                return;
            }
        }
    }
}
