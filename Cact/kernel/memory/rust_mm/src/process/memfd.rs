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
use crate::alloc::heap::{kfree, kmalloc};
use crate::pmm::{kalloc, free_page};
use crate::safe::{KStatic, lock_acquire, lock_release, zero_page, kprint_str};

pub const MEMFD_MAX: usize = 128;
/// Initial slot count of an object's frame-pointer array (4 MiB worth); the
/// array is a `kmalloc` block grown in `MEMFD_PAGES_GROW` steps, so an object is
/// no longer capped at 4 MiB.  A fixed `MEMFD_MAX_PAGES = 1024` array (or cap)
/// is what kept a 502 MiB tmpfs file — the localrepo firmware archive — from
/// ever being written.
pub const MEMFD_PAGES_INITIAL: u32 = 1024;
/// Slots added per growth step (64 MiB worth).
pub const MEMFD_PAGES_GROW: u32 = 16 * 1024;
/// Ceiling on one object's frame count: the frames the PMM can actually hand out
/// between the boot reservation and `USER_FRAME_CEIL`.  `grow_locked` also rolls
/// back when `kalloc` runs out, so free RAM is the real limit.
pub const MEMFD_PAGES_MAX: u32 = USER_FRAME_CEIL / PAGE_SIZE;
pub const MEMFD_NAME_MAX: usize = 128;

pub const MFD_CLOEXEC: i32 = 0x0001;

#[repr(C)]
#[derive(Clone, Copy)]
struct MemFd {
    name: [u8; MEMFD_NAME_MAX],
    name_len: u32,
    size: u32,
    num_pages: u32,
    /// Slots `pages` can hold (0 while `pages` is null).
    pages_cap: u32,
    /// `kmalloc` block of `pages_cap` frame pointers, grown on demand; released
    /// by `maybe_free_locked` when the object is destroyed.
    pages: *mut *mut u8,
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
    pages_cap: 0,
    pages: core::ptr::null_mut(),
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
    // SAFETY: `MEMFD_INITIALIZED` is a boot/first-use latch; a benign race can only
    // cause the idempotent `irq_spinlock_init` to run twice.
    if *unsafe { KStatic::get_mut(MEMFD_INITIALIZED.as_ptr()) } != 0 {
        return;
    }
    // SAFETY: boot-time / first-use init, single-threaded.
    unsafe { irq_spinlock_init(MEMFD_LOCK.as_ptr()) };
    // SAFETY: single-threaded first-use init: the latch is set after the lock is
    // initialised.
    *unsafe { KStatic::get_mut(MEMFD_INITIALIZED.as_ptr()) } = 1;
}

fn handle_valid(h: i32) -> bool {
    // SAFETY: `MEMFD_TABLE` read; every `handle_valid` caller holds `MEMFD_LOCK`,
    // so the entry cannot be freed under us.
    h >= 1 && (h as usize) <= MEMFD_MAX && (unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[(h - 1) as usize].valid != 0
}

fn ceil_pages(bytes: u32) -> u32 {
    bytes.div_ceil(PAGE_SIZE)
}

/// Make sure `s` can index `need_pages` slots, growing the `kmalloc` block in
/// place (copying the live entries, freeing the old block).  Caller holds
/// `MEMFD_LOCK`.
fn ensure_capacity_locked(s: &mut MemFd, need_pages: u32) -> bool {
    if need_pages <= s.pages_cap {
        return true;
    }
    if need_pages > MEMFD_PAGES_MAX {
        return false;
    }
    let mut cap = if s.pages_cap == 0 { MEMFD_PAGES_INITIAL } else { s.pages_cap };
    while cap < need_pages {
        // `need_pages <= MEMFD_PAGES_MAX` (checked above), so the clamp cannot
        // leave `cap` short of `need_pages` and the loop terminates.
        cap = core::cmp::min(cap.saturating_add(MEMFD_PAGES_GROW), MEMFD_PAGES_MAX);
    }
    let bytes = cap * core::mem::size_of::<*mut u8>() as u32;
    let new_arr = kmalloc(bytes) as *mut *mut u8;
    if new_arr.is_null() {
        return false;
    }
    // SAFETY: `new_arr` is a fresh `kmalloc` block of `cap` slots (checked
    // non-null); zeroing all `cap` slots is in bounds, copying the
    // `num_pages <= pages_cap` live entries out of the old array is in bounds
    // for both, and the old block is freed exactly once and replaced.
    unsafe {
        core::ptr::write_bytes(new_arr, 0, cap as usize);
        if !s.pages.is_null() {
            core::ptr::copy_nonoverlapping(s.pages, new_arr, s.num_pages as usize);
            kfree(s.pages as *mut u8);
        }
    }
    s.pages = new_arr;
    s.pages_cap = cap;
    true
}

/// Grow the object so it covers `need_pages` frames.  Caller holds MEMFD_LOCK.
fn grow_locked(s: &mut MemFd, need_pages: u32) -> bool {
    if need_pages <= s.num_pages {
        return true;
    }
    if !ensure_capacity_locked(s, need_pages) {
        return false;
    }
    let start = s.num_pages;
    for i in start..need_pages {
        let p = kalloc();
        if p.is_null() {
            // Roll back the frames allocated in this call.
            for j in start..i {
                // SAFETY: slot `j < num_pages <= pages_cap` still holds a live
                // frame allocated above; one reference is released per slot.
                unsafe { free_page(*s.pages.add(j as usize)) };
                unsafe { *s.pages.add(j as usize) = core::ptr::null_mut() };
            }
            return false;
        }
        zero_page(p);
        // SAFETY: `i < need_pages <= pages_cap`, so the slot exists.
        unsafe { *s.pages.add(i as usize) = p };
    }
    s.num_pages = need_pages;
    true
}

/// Free the object once both reference counters are zero.  Caller holds lock.
fn maybe_free_locked(slot: usize) {
    // SAFETY: `MEMFD_TABLE[slot]` freed while `MEMFD_LOCK` is held (the `_locked`
    // suffix records that contract).
    let s = &mut (unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[slot];
    if s.valid == 0 || s.fds != 0 || s.maps != 0 {
        return;
    }
    for i in 0..s.num_pages as usize {
        // SAFETY: `i < num_pages <= pages_cap` and `pages` is this object's live
        // array, so the slot load is in bounds.
        let p = unsafe { *s.pages.add(i) };
        if !p.is_null() {
            // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
            unsafe { free_page(p) };
        }
    }
    // SAFETY: `s.pages` is this object's own `kmalloc` block (or null) and this
    // is the single release for it; the slot becomes reusable only after the
    // reset below.
    if !s.pages.is_null() {
        unsafe { kfree(s.pages as *mut u8) };
        s.pages = core::ptr::null_mut();
    }
    s.pages_cap = 0;
    s.num_pages = 0;
    s.size = 0;
    s.name_len = 0;
    s.flags = 0;
    s.valid = 0;
}

/// # Safety
///
/// `name` must be null or point to at least `name_len` readable bytes that stay
/// valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn memfd_create(name: *const u8, name_len: u32, flags: i32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr());

    // SAFETY: `MEMFD_TABLE` mutated for the whole of `memfd_create`, with
    // `MEMFD_LOCK` held.
    let table = unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) };
    let slot: usize = table.iter().position(|e| e.valid == 0).unwrap_or(MEMFD_MAX);
    if slot == MEMFD_MAX {
        lock_release(MEMFD_LOCK.as_ptr());
        kprint_str(c"[MEMFD] memfd_create: table full\n".as_ptr() as *const u8);
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
    // A reusable slot has already released its frame array in
    // `maybe_free_locked` (the only path that clears `valid`), so the block is
    // allocated lazily by the first `grow_locked`.
    s.pages_cap = 0;
    s.flags = flags as u32;
    s.fds = 0;
    s.maps = 0;
    s.valid = 1;

    lock_release(MEMFD_LOCK.as_ptr());
    (slot + 1) as i32
}

/// Increment the fd reference count (node open / fork).  Returns handle.
#[unsafe(no_mangle)]
pub extern "C" fn memfd_ref(handle: i32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr());
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr());
        return -1;
    }
    // SAFETY: `MEMFD_TABLE[handle-1].fds` incremented under `MEMFD_LOCK`, after
    // `handle_valid` confirmed the entry.
    (unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[(handle - 1) as usize].fds += 1;
    lock_release(MEMFD_LOCK.as_ptr());
    handle
}

/// Decrement the fd reference count (last node close).  Frees if possible.
#[unsafe(no_mangle)]
pub extern "C" fn memfd_close(handle: i32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr());
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr());
        return -1;
    }
    let slot = (handle - 1) as usize;
    // SAFETY: `MEMFD_TABLE[slot].fds` read under `MEMFD_LOCK`.
    if (unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[slot].fds > 0 {
        // SAFETY: decrement of the same reference count, still under `MEMFD_LOCK`.
        (unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[slot].fds -= 1;
    }
    maybe_free_locked(slot);
    lock_release(MEMFD_LOCK.as_ptr());
    0
}

/// Add a MAP_SHARED mmap-region reference.
#[unsafe(no_mangle)]
pub extern "C" fn memfd_map_inc(handle: i32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr());
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr());
        return -1;
    }
    // SAFETY: `MEMFD_TABLE[handle-1].maps` incremented under `MEMFD_LOCK`.
    (unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[(handle - 1) as usize].maps += 1;
    lock_release(MEMFD_LOCK.as_ptr());
    0
}

/// Drop a MAP_SHARED mmap-region reference.  Frees if possible.
#[unsafe(no_mangle)]
pub extern "C" fn memfd_map_dec(handle: i32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr());
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr());
        return -1;
    }
    let slot = (handle - 1) as usize;
    // SAFETY: `MEMFD_TABLE[slot].maps` read under `MEMFD_LOCK`.
    if (unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[slot].maps > 0 {
        // SAFETY: decrement of the same mapping count, under `MEMFD_LOCK`.
        (unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[slot].maps -= 1;
    }
    maybe_free_locked(slot);
    lock_release(MEMFD_LOCK.as_ptr());
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn memfd_size(handle: i32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr());
    let size = if handle_valid(handle) {
        // SAFETY: `MEMFD_TABLE[handle-1].size` read under `MEMFD_LOCK`.
        (unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[(handle - 1) as usize].size as i32
    } else {
        -1
    };
    lock_release(MEMFD_LOCK.as_ptr());
    size
}

/// Grow the object to cover at least byte offset `end` (used by mmap).
pub(crate) fn memfd_grow_to(handle: i32, end: u32) -> i32 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr());
    let res = if handle_valid(handle) {
        // SAFETY: `MEMFD_TABLE[handle-1]` mutated by `memfd_grow_to`, which holds
        // `MEMFD_LOCK`.
        let s = &mut (unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[(handle - 1) as usize];
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
    lock_release(MEMFD_LOCK.as_ptr());
    res
}

/// Fetch the frame pointer for page index `idx` (caller must have grown).
/// Exported so kernel subsystems can touch a shared object's storage directly
/// (DRM GEM buffers, framebuffer blits) instead of copying through
/// memfd_read/memfd_write.  Frames are identity-mapped, so the returned
/// pointer is usable as a plain kernel pointer.
#[unsafe(no_mangle)]
pub extern "C" fn memfd_get_page(handle: i32, idx: u32) -> *mut u8 {
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr());
    let page = if handle_valid(handle) {
        // SAFETY: `MEMFD_TABLE[handle-1]` read under `MEMFD_LOCK`; the returned frame
        // stays owned by the memfd while `MEMFD_LOCK` is held and is refcounted by
        // `page_ref_inc` for the caller.
        let s = &(unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[(handle - 1) as usize];
        if idx < s.num_pages {
            // SAFETY: `idx < num_pages <= pages_cap`, so the slot is in bounds.
            unsafe { *s.pages.add(idx as usize) }
        } else {
            core::ptr::null_mut()
        }
    } else {
        core::ptr::null_mut()
    };
    lock_release(MEMFD_LOCK.as_ptr());
    page
}

/// # Safety
///
/// `buf` must be null or point to at least `size` writable bytes that stay
/// valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn memfd_read(handle: i32, off: u32, buf: *mut u8, size: u32) -> i32 {
    if size == 0 {
        return 0;
    }
    if buf.is_null() {
        return -1;
    }
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr());
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr());
        return -1;
    }
    // SAFETY: `MEMFD_TABLE[handle-1]` read under `MEMFD_LOCK`; `grow_locked` (and
    // thus its refcounted frames) cannot be freed while we hold the lock.
    let s = &(unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[(handle - 1) as usize];
    if off >= s.size {
        lock_release(MEMFD_LOCK.as_ptr());
        return 0;
    }
    let mut n = size;
    // Overflow-safe replacement for the C `off + n < off` guard: a wrapped
    // sum is treated exactly like the original overflow test.
    let end_fits = match off.checked_add(n) {
        Some(end) => end <= s.size,
        None => false,
    };
    if !end_fits {
        n = s.size - off;
    }
    let mut copied = 0u32;
    let mut pos = off;
    while copied < n {
        let page_idx = pos / PAGE_SIZE;
        let in_page = pos % PAGE_SIZE;
        let chunk = core::cmp::min(n - copied, PAGE_SIZE - in_page);
        // SAFETY: `page_idx < num_pages` (the caller is bounded by `size`, which
        // never exceeds `num_pages` frames) and `num_pages <= pages_cap`, so the
        // slot load is in bounds.
        let page = unsafe { *s.pages.add(page_idx as usize) };
        // SAFETY: `in_page < PAGE_SIZE` and `page` is a live frame, so this is a
        // valid pointer to `chunk <= PAGE_SIZE - in_page` readable bytes.
        let src = unsafe { page.add(in_page as usize) };
        // SAFETY: `copied < size` and `buf` is a valid C buffer of at least `size`
        // bytes, so this is a valid pointer to `chunk` writable bytes.
        let dst = unsafe { buf.add(copied as usize) };
        // SAFETY: `src`/`dst` are the in-bounds pointers computed above and the two
        // ranges are distinct (a mapped frame vs the caller's buffer).
        unsafe { core::ptr::copy_nonoverlapping(src, dst, chunk as usize) };
        copied += chunk;
        pos += chunk;
    }
    lock_release(MEMFD_LOCK.as_ptr());
    copied as i32
}

/// # Safety
///
/// `buf` must be null or point to at least `size` readable bytes that stay
/// valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn memfd_write(handle: i32, off: u32, buf: *const u8, size: u32) -> i32 {
    if size == 0 {
        return 0;
    }
    if buf.is_null() {
        return -1;
    }
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr());
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr());
        return -1;
    }
    let end = off.saturating_add(size);
    if end < off || ceil_pages(end) > MEMFD_PAGES_MAX {
        lock_release(MEMFD_LOCK.as_ptr());
        return -1;
    }
    // SAFETY: `MEMFD_TABLE[handle-1]` read/updated under `MEMFD_LOCK`; the frames
    // it owns stay live for the duration of the copy.
    let s = &mut (unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[(handle - 1) as usize];
    if !grow_locked(s, ceil_pages(end)) {
        lock_release(MEMFD_LOCK.as_ptr());
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
        // SAFETY: `page_idx < num_pages` (the `grow_locked` above covers `end`,
        // and `end <= num_pages * PAGE_SIZE`) and `num_pages <= pages_cap`, so
        // the slot load is in bounds.
        let page = unsafe { *s.pages.add(page_idx as usize) };
        // SAFETY: `written < size` and `buf` is a valid C buffer of at least `size`
        // bytes, so this is a valid pointer to `chunk` readable bytes.
        let src = unsafe { buf.add(written as usize) };
        // SAFETY: `in_page < PAGE_SIZE` and `page` is a live frame, so this is a
        // valid pointer to `chunk <= PAGE_SIZE - in_page` writable bytes.
        let dst = unsafe { page.add(in_page as usize) };
        // SAFETY: `src`/`dst` are the in-bounds pointers computed above and the two
        // ranges are distinct (the caller's buffer vs a mapped frame).
        unsafe { core::ptr::copy_nonoverlapping(src, dst, chunk as usize) };
        written += chunk;
        pos += chunk;
    }
    lock_release(MEMFD_LOCK.as_ptr());
    written as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn memfd_truncate(handle: i32, new_size: u32) -> i32 {
    if ceil_pages(new_size) > MEMFD_PAGES_MAX {
        return -1;
    }
    memfd_ensure_init();
    lock_acquire(MEMFD_LOCK.as_ptr());
    if !handle_valid(handle) {
        lock_release(MEMFD_LOCK.as_ptr());
        return -1;
    }
    // SAFETY: `MEMFD_TABLE[handle-1]` mutated under `MEMFD_LOCK`.
    let s = &mut (unsafe { KStatic::get_mut(MEMFD_TABLE.as_ptr()) })[(handle - 1) as usize];
    if new_size > s.size
        && !grow_locked(s, ceil_pages(new_size)) {
            lock_release(MEMFD_LOCK.as_ptr());
            return -1;
        }
    // Shrink only reduces the logical size; the backing frames are left in
    // place until object destruction so live MAP_SHARED mappings stay valid.
    s.size = new_size;
    lock_release(MEMFD_LOCK.as_ptr());
    0
}
