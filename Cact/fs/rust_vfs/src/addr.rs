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
/// Initial page count the per-object mirror bitmaps cover (4 MiB); they are
/// grown from the heap on demand, so an object is not limited to this.  The old
/// fixed `AS_MAX_PAGES = 1024` silently stopped tracking anything past 4 MiB —
/// and on the populate path it stopped *filling* those pages entirely.
const AS_PAGES_INITIAL: usize = 1024;
const PAGE: usize = 4096;
/// Ceiling on one object's mirror bitmaps: the frames the PMM can hand out.
const AS_PAGES_MAX: usize = (cact_mm::USER_FRAME_CEIL / PAGE as u32) as usize;

/// Bytes a bitmap describing `pages` pages occupies.
#[inline]
const fn bitmap_bytes(pages: usize) -> usize {
    (pages + 7) / 8
}

/// Raw backing I/O: read/write `size` bytes at file offset `off` for
/// `(owner, ino)`.  Returns the byte count, or negative on error.
pub type BackingFn = unsafe extern "C" fn(*mut c_void, u32, u32, u32, *mut c_char) -> i32;

struct AddrSpace {
    used:    bool,
    owner:   *mut c_void,
    ino:     u32,
    mfd:     i32,
    size:    u32,
    /// Pages the two bitmaps below can describe (0 until the first growth).
    cap_pages: u32,
    /// `kmalloc`'d "page is populated" bitmap (`bitmap_bytes(cap_pages)` bytes).
    present: *mut u8,
    /// `kmalloc`'d "page needs write-back" bitmap, same size as `present`.
    dirty:   *mut u8,
    rfn:     Option<BackingFn>,
    wfn:     Option<BackingFn>,
}

const AS_EMPTY: AddrSpace = AddrSpace {
    used:    false,
    owner:   ptr::null_mut(),
    ino:     0,
    mfd:     0,
    size:    0,
    cap_pages: 0,
    present: ptr::null_mut(),
    dirty:   ptr::null_mut(),
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

/// # Safety
///
/// `bm` must point to a bitmap covering page index `i`.
#[inline]
unsafe fn bit(bm: *const u8, i: usize) -> bool {
    // SAFETY: per the caller contract the byte holding bit `i` is in bounds.
    unsafe { *bm.add(i / 8) & (1 << (i % 8)) != 0 }
}

/// # Safety
///
/// `bm` must point to a bitmap covering page index `i`.
#[inline]
unsafe fn set_bit(bm: *mut u8, i: usize) {
    // SAFETY: per the caller contract the byte holding bit `i` is in bounds.
    unsafe { *bm.add(i / 8) |= 1 << (i % 8) };
}

/// # Safety
///
/// `bm` must point to a bitmap covering page index `i`.
#[inline]
unsafe fn clear_bit(bm: *mut u8, i: usize) {
    // SAFETY: per the caller contract the byte holding bit `i` is in bounds.
    unsafe { *bm.add(i / 8) &= !(1 << (i % 8)) };
}

/// Make sure `a`'s mirror bitmaps can describe `need_pages` pages, growing both
/// from the heap (and preserving what they already record).  Returns false when
/// the growth is impossible; the caller then simply does not track those pages.
fn ensure_bits(a: &mut AddrSpace, need_pages: usize) -> bool {
    if !a.present.is_null() && need_pages <= a.cap_pages as usize {
        return true;
    }
    let mut cap = if a.cap_pages == 0 { AS_PAGES_INITIAL } else { a.cap_pages as usize };
    while cap < need_pages {
        cap = core::cmp::min(cap.saturating_mul(2), AS_PAGES_MAX);
    }
    if cap < need_pages {
        return false;
    }
    let bytes = bitmap_bytes(cap);
    let new_present = cact_mm::kmalloc(bytes as u32);
    let new_dirty = cact_mm::kmalloc(bytes as u32);
    if new_present.is_null() || new_dirty.is_null() {
        // SAFETY: each non-null block was just allocated here and is owned here.
        unsafe {
            if !new_present.is_null() { cact_mm::kfree(new_present); }
            if !new_dirty.is_null() { cact_mm::kfree(new_dirty); }
        }
        return false;
    }
    // SAFETY: both new blocks are `bitmap_bytes(cap)`-byte `kmalloc` allocations
    // (checked non-null), the old ones are this object's own (or null) and hold
    // `bitmap_bytes(cap_pages)` bytes each; the copy stays inside both, and the
    // old blocks are released exactly once as they are replaced.
    unsafe {
        core::ptr::write_bytes(new_present, 0, bytes);
        core::ptr::write_bytes(new_dirty, 0, bytes);
        if !a.present.is_null() {
            let old = bitmap_bytes(a.cap_pages as usize);
            core::ptr::copy_nonoverlapping(a.present, new_present, old.min(bytes));
            cact_mm::kfree(a.present);
        }
        if !a.dirty.is_null() {
            let old = bitmap_bytes(a.cap_pages as usize);
            core::ptr::copy_nonoverlapping(a.dirty, new_dirty, old.min(bytes));
            cact_mm::kfree(a.dirty);
        }
    }
    a.present = new_present;
    a.dirty = new_dirty;
    a.cap_pages = cap as u32;
    true
}

unsafe fn as_of(owner: *mut c_void, ino: u32) -> *mut AddrSpace {
    // SAFETY: fixed static table; linear scan / reuse / append.
    unsafe {
        let f = as_find(owner, ino);
        if !f.is_null() {
            return f;
        }
        // Reuse a slot an earlier `vfs_as_release` cleared, so the table is
        // bounded by the objects *alive* rather than by every object ever made
        // (a shm or /tmp workload that creates and drops objects in a loop
        // would otherwise exhaust AS_MAX permanently).
        let mut idx = AS_COUNT;
        for i in 0..AS_COUNT {
            if !(*ptr::addr_of!(AS[i])).used {
                idx = i;
                break;
            }
        }
        if idx == AS_COUNT && AS_COUNT >= AS_MAX {
            return ptr::null_mut();
        }
        let h = memfd_create(ptr::null(), 0, 0);
        if h <= 0 {
            return ptr::null_mut();
        }
        memfd_ref(h);
        let a = &mut *ptr::addr_of_mut!(AS[idx]);
        a.used = true;
        a.owner = owner;
        a.ino = ino;
        a.mfd = h;
        a.size = 0;
        a.cap_pages = 0;
        a.present = ptr::null_mut();
        a.dirty = ptr::null_mut();
        a.rfn = None;
        a.wfn = None;
        if idx == AS_COUNT {
            AS_COUNT += 1;
        }
        AS_OBJECTS += 1;
        ptr::addr_of_mut!(AS[idx])
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
        // Pages past the bitmap can only be re-populated every time (never
        // skipped as present), which keeps reads correct at any file size.
        let tracked = ensure_bits(&mut *a, last + 1);
        for p in first..=last {
            if tracked && bit((*a).present, p) {
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
            if tracked {
                set_bit((*a).present, p);
            }
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
        // A page the bitmaps cannot describe is simply left untracked; the write
        // itself already went through the memfd.
        let tracked = ensure_bits(&mut *a, last + 1);
        if tracked {
            for p in first..=last {
                set_bit((*a).present, p);
                set_bit((*a).dirty, p);
            }
        }
        // Write through to the backing store so normal write() is durable.
        if let Some(wfn) = (*a).wfn {
            let r = wfn((*a).owner, (*a).ino, off, w as u32, buf);
            if r == w && tracked {
                for p in first..=last {
                    clear_bit((*a).dirty, p);
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
        // No fixed size ceiling here: the memfd behind the object grows to
        // whatever `MEMFD_PAGES_MAX` (the PMM's own frame ceiling) allows.
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
            if p >= (*a).cap_pages as usize || !bit((*a).present, p) {
                continue;
            }
            let page_off = (p * PAGE) as u32;
            let chunk = core::cmp::min(PAGE as u32, fsz - page_off);
            let page = memfd_get_page((*a).mfd, p as u32);
            if !page.is_null() {
                wfn((*a).owner, (*a).ino, page_off, chunk, page as *mut c_char);
            }
            clear_bit((*a).dirty, p);
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
                // The mirror bitmaps belong to this entry; give them back so a
                // later object reusing the slot starts clean.
                if !a.present.is_null() {
                    cact_mm::kfree(a.present);
                    a.present = ptr::null_mut();
                }
                if !a.dirty.is_null() {
                    cact_mm::kfree(a.dirty);
                    a.dirty = ptr::null_mut();
                }
                a.cap_pages = 0;
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
