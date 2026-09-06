//! memfd: anonymous, RAM-backed file objects (Linux memfd_create).
//!
//! Each object owns a growable array of reference-counted physical frames.
//! fd-style accessors (read/write/truncate) operate on the same frames that
//! MAP_SHARED mmap regions point at, so file I/O and shared mappings observe
//! each other.  Lifetimes are managed by two counters:
//!
//!   * `fds`  — number of open file descriptions referencing the object;
//!   * `maps` — number of live MAP_SHARED mmap regions referencing it.
//!
//! The object is destroyed (all frames returned) when both counters hit zero.
//! Frame refcounts additionally track per-PTE mappings for fork/unmap.

use crate::ffi::*;
use crate::pmm::{kalloc, free_page};
use crate::safe::{KStatic, lock_acquire, lock_release, zero_page, kprint_str};

pub const MEMFD_MAX: usize = 32;
pub const MEMFD_MAX_PAGES: u32 = 1024;
pub const MEMFD_NAME_MAX: usize = 128;

pub const MFD_CLOEXEC: i32 = 0x0001;

#[repr(C)]
#[derive(Clone, Copy)]
struct MemFd {
    name: [u8; MEMFD_NAME_MAX],
    name_len: u32,
    size: u32,
    num_pages: u32,
    pages: [*mut u8; MEMFD_MAX_PAGES as usize],
    flags: u32,
    fds: i32,
    maps: i32,
    valid: i32,
}

const MEMFD_EMPTY: MemFd = MemFd {
    name: [0; MEMFD_NAME_MAX],
    name_len: 0,
    size: 0,
    num_pages: 0,
    pages: [core::ptr::null_mut(); MEMFD_MAX_PAGES as usize],
    flags: 0,
    fds: 0,
    maps: 0,
    valid: 0,
};

static MEMFD_TABLE: KStatic<[MemFd; MEMFD_MAX]> =
    KStatic::new([MEMFD_EMPTY; MEMFD_MAX]);
static MEMFD_LOCK: KStatic<IrqSpinlock> =
    KStatic::new(IrqSpinlock { spin_locked: 0, saved_flags: 0 });
static MEMFD_INITIALIZED: KStatic<i32> = KStatic::new(0);

fn memfd_ensure_init() {
    if *MEMFD_INITIALIZED.get_mut() != 0 {
        return;
    }
    // SAFETY: boot-time / first-use init, single-threaded.
    unsafe { irq_spinlock_init(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock) };
    *MEMFD_INITIALIZED.get_mut() = 1;
}

fn handle_valid(h: i32) -> bool {
    h >= 1 && (h as usize) <= MEMFD_MAX && MEMFD_TABLE.get_mut()[(h - 1) as usize].valid != 0
}

fn ceil_pages(bytes: u32) -> u32 {
    (bytes + PAGE_SIZE - 1) / PAGE_SIZE
}

/// Grow the object so it covers `need_pages` frames.  Caller holds MEMFD_LOCK.
fn grow_locked(s: &mut MemFd, need_pages: u32) -> bool {
    if need_pages <= s.num_pages {
        return true;
    }
    if need_pages > MEMFD_MAX_PAGES {
        return false;
    }
    let start = s.num_pages;
    for i in start..need_pages {
        let p = kalloc();
        if p.is_null() {
            // Roll back the frames allocated in this call.
            for j in start..i {
                free_page(s.pages[j as usize]);
                s.pages[j as usize] = core::ptr::null_mut();
            }
            return false;
        }
        zero_page(p);
        s.pages[i as usize] = p;
    }
    s.num_pages = need_pages;
    true
}

/// Free the object once both reference counters are zero.  Caller holds lock.
fn maybe_free_locked(slot: usize) {
    let s = &mut MEMFD_TABLE.get_mut()[slot];
    if s.valid == 0 || s.fds != 0 || s.maps != 0 {
        return;
    }
    for i in 0..s.num_pages as usize {
        if !s.pages[i].is_null() {
            free_page(s.pages[i]);
            s.pages[i] = core::ptr::null_mut();
        }
    }
    s.num_pages = 0;
    s.size = 0;
    s.name_len = 0;
    s.flags = 0;
    s.valid = 0;
}

#[unsafe(no_mangle)]
pub extern "C" fn memfd_create(name: *const u8, name_len: u32, flags: i32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);

    let table = MEMFD_TABLE.get_mut();
    let mut slot: usize = MEMFD_MAX;
    for i in 0..MEMFD_MAX {
        if table[i].valid == 0 {
            slot = i;
            break;
        }
    }
    if slot == MEMFD_MAX {
        lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
        kprint_str(b"[MEMFD] memfd_create: table full\n\0".as_ptr());
        return -1;
    }

    let s = &mut table[slot];
    let mut len = name_len as usize;
    if len > MEMFD_NAME_MAX - 1 {
        len = MEMFD_NAME_MAX - 1;
    }
    s.name[0] = 0;
    if !name.is_null() && len > 0 {
        // SAFETY: C passes a kernel buffer of at least `len` readable bytes.
        unsafe { core::ptr::copy_nonoverlapping(name, s.name.as_mut_ptr(), len) };
    }
    s.name[len] = 0;
    s.name_len = len as u32;
    s.size = 0;
    s.num_pages = 0;
    s.flags = flags as u32;
    s.fds = 0;
    s.maps = 0;
    s.valid = 1;

    lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    (slot + 1) as i32
}

/// Increment the fd reference count (node open / fork).  Returns handle.
#[unsafe(no_mangle)]
pub extern "C" fn memfd_ref(handle: i32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }
    MEMFD_TABLE.get_mut()[(handle - 1) as usize].fds += 1;
    lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    handle
}

/// Decrement the fd reference count (last node close).  Frees if possible.
#[unsafe(no_mangle)]
pub extern "C" fn memfd_close(handle: i32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }
    let slot = (handle - 1) as usize;
    if MEMFD_TABLE.get_mut()[slot].fds > 0 {
        MEMFD_TABLE.get_mut()[slot].fds -= 1;
    }
    maybe_free_locked(slot);
    lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    0
}

/// Add a MAP_SHARED mmap-region reference.
#[unsafe(no_mangle)]
pub extern "C" fn memfd_map_inc(handle: i32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }
    MEMFD_TABLE.get_mut()[(handle - 1) as usize].maps += 1;
    lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    0
}

/// Drop a MAP_SHARED mmap-region reference.  Frees if possible.
#[unsafe(no_mangle)]
pub extern "C" fn memfd_map_dec(handle: i32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }
    let slot = (handle - 1) as usize;
    if MEMFD_TABLE.get_mut()[slot].maps > 0 {
        MEMFD_TABLE.get_mut()[slot].maps -= 1;
    }
    maybe_free_locked(slot);
    lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn memfd_size(handle: i32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    let size = if handle_valid(handle) {
        MEMFD_TABLE.get_mut()[(handle - 1) as usize].size as i32
    } else {
        -1
    };
    lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    size
}

/// Grow the object to cover at least byte offset `end` (used by mmap).
pub(crate) fn memfd_grow_to(handle: i32, end: u32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    let res = if handle_valid(handle) {
        let s = &mut MEMFD_TABLE.get_mut()[(handle - 1) as usize];
        if grow_locked(s, ceil_pages(end)) {
            if end > s.size {
                s.size = end;
            }
            0
        } else {
            -1
        }
    } else {
        -1
    };
    lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    res
}

/// Fetch the frame pointer for page index `idx` (caller must have grown).
pub(crate) fn memfd_get_page(handle: i32, idx: u32) -> *mut u8 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    let page = if handle_valid(handle) {
        let s = &MEMFD_TABLE.get_mut()[(handle - 1) as usize];
        if idx < s.num_pages {
            s.pages[idx as usize]
        } else {
            core::ptr::null_mut()
        }
    } else {
        core::ptr::null_mut()
    };
    lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    page
}

#[unsafe(no_mangle)]
pub extern "C" fn memfd_read(handle: i32, off: u32, buf: *mut u8, size: u32) -> i32 {
    if size == 0 {
        return 0;
    }
    if buf.is_null() {
        return -1;
    }
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }
    let s = &MEMFD_TABLE.get_mut()[(handle - 1) as usize];
    if off >= s.size {
        lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
        return 0;
    }
    let mut n = size;
    if off + n > s.size || off + n < off {
        n = s.size - off;
    }
    let mut copied = 0u32;
    let mut pos = off;
    while copied < n {
        let page_idx = pos / PAGE_SIZE;
        let in_page = pos % PAGE_SIZE;
        let chunk = core::cmp::min(n - copied, PAGE_SIZE - in_page);
        let page = s.pages[page_idx as usize];
        // SAFETY: page is a valid frame; buf is a valid C buffer (>= size).
        unsafe {
            core::ptr::copy_nonoverlapping(page.add(in_page as usize), buf.add(copied as usize), chunk as usize);
        }
        copied += chunk;
        pos += chunk;
    }
    lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    copied as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn memfd_write(handle: i32, off: u32, buf: *const u8, size: u32) -> i32 {
    if size == 0 {
        return 0;
    }
    if buf.is_null() {
        return -1;
    }
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }
    let end = off.saturating_add(size);
    if end < off || end > MEMFD_MAX_PAGES * PAGE_SIZE {
        lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }
    let s = &mut MEMFD_TABLE.get_mut()[(handle - 1) as usize];
    if !grow_locked(s, ceil_pages(end)) {
        lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }
    if end > s.size {
        s.size = end;
    }

    let mut written = 0u32;
    let mut pos = off;
    while written < size {
        let page_idx = pos / PAGE_SIZE;
        let in_page = pos % PAGE_SIZE;
        let chunk = core::cmp::min(size - written, PAGE_SIZE - in_page);
        let page = s.pages[page_idx as usize];
        // SAFETY: page is a valid frame; buf is a valid C buffer (>= size).
        unsafe {
            core::ptr::copy_nonoverlapping(buf.add(written as usize), page.add(in_page as usize), chunk as usize);
        }
        written += chunk;
        pos += chunk;
    }
    lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    written as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn memfd_truncate(handle: i32, new_size: u32) -> i32 {
    if new_size > MEMFD_MAX_PAGES * PAGE_SIZE {
        return -1;
    }
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }
    let s = &mut MEMFD_TABLE.get_mut()[(handle - 1) as usize];
    if new_size > s.size {
        if !grow_locked(s, ceil_pages(new_size)) {
            lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
            return -1;
        }
    }
    // Shrink only reduces the logical size; the backing frames are left in
    // place until object destruction so live MAP_SHARED mappings stay valid.
    s.size = new_size;
    lock_release(MEMFD_LOCK.as_ptr() as *mut IrqSpinlock);
    0
}
