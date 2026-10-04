//! Directory-entry cache for the Rust VFS core.
//!
//! Keyed on `(parent_node, name) -> child_node`.  Only *positive* `walk()`
//! results are cached; mount points are resolved through the mount table each
//! time so their reference semantics are unchanged.
//!
//! Synthetic filesystems that create names on the fly — procfs (`/proc/<pid>`
//! appears and disappears) and devfs (runtime `register_chrdev`) — must not be
//! cached.  They call [`vfs_set_nocache`] on their root; the flag then
//! *propagates* to every node reached below them, so no binding under a dynamic
//! subtree is ever cached.  This keeps the dcache correct without adding a
//! cacheability field to `vfs_node_t` (which would change its size and force
//! every `.cctk` module to be rebuilt in lockstep).
//!
//! The cache is protected by its own IRQ spinlock; nothing here is exported to
//! C except `vfs_set_nocache`.

use core::ffi::c_char;
use core::mem::MaybeUninit;
use core::ptr;

use cact_sync::irq_spinlock_t;

use crate::abi::VfsNode;

const DCACHE_BUCKETS: usize = 128;
const DCACHE_MAX: usize = 512;
const NOCACHE_MAX: usize = 256;
const PARENT_MAX: usize = 256;

#[repr(C)]
struct Entry {
    used:   bool,
    parent: *mut VfsNode,
    name:   [c_char; 128],
    child:  *mut VfsNode,
    next:   i32, // index of the next entry in the same bucket, -1 = end
}

const ENTRY_INIT: Entry = Entry {
    used:   false,
    parent: ptr::null_mut(),
    name:   [0; 128],
    child:  ptr::null_mut(),
    next:   -1,
};

static mut ENTRIES: [Entry; DCACHE_MAX] = [ENTRY_INIT; DCACHE_MAX];
static mut BUCKETS: [i32; DCACHE_BUCKETS] = [-1; DCACHE_BUCKETS];
static mut NOCACHE: [*mut VfsNode; NOCACHE_MAX] = [ptr::null_mut(); NOCACHE_MAX];
static mut NOCACHE_COUNT: usize = 0;
// child directory -> physical parent directory, recorded as lookups happen.
// Lets `finddir_vfs(dir, "..")` (the final component of a dirfd-relative
// operation) find the parent, which names-on-nodes give us no other way to know.
static mut PARENTS: [(*mut VfsNode, *mut VfsNode); PARENT_MAX] =
    [(ptr::null_mut(), ptr::null_mut()); PARENT_MAX];
static mut PARENT_COUNT: usize = 0;
static mut LOCK: MaybeUninit<irq_spinlock_t> = MaybeUninit::uninit();

#[inline]
unsafe fn lock() -> *mut irq_spinlock_t {
    ptr::addr_of_mut!(LOCK) as *mut irq_spinlock_t
}

fn name_hash(name: *const c_char) -> u32 {
    // djb2 over the C string.
    let mut h: u32 = 5381;
    let mut i = 0usize;
    // SAFETY: `name` is a NUL-terminated C string supplied by the caller.
    unsafe {
        let mut c = *name;
        while c != 0 {
            h = h.wrapping_mul(33).wrapping_add(c as u8 as u32);
            i += 1;
            c = *name.add(i);
        }
    }
    h
}

#[inline]
fn bucket_of(parent: *mut VfsNode, name: *const c_char) -> usize {
    ((parent as usize as u32) ^ name_hash(name)) as usize & (DCACHE_BUCKETS - 1)
}

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
    // SAFETY: `src` is NUL-terminated; `dst` has 128 bytes and we stop at 127.
    unsafe {
        while i + 1 < 128 && *src.add(i) != 0 {
            *dst.add(i) = *src.add(i);
            i += 1;
        }
        *dst.add(i) = 0;
    }
}

/// Clear the whole cache and forget the nocache registry.
pub unsafe fn reset() {
    // SAFETY: called once from `vfs_init` before any lookup; the lock is
    // initialised here and each static written through a raw pointer.
    unsafe {
        cact_sync::irq_spinlock_init(lock());
        for i in 0..DCACHE_MAX {
            (*ptr::addr_of_mut!(ENTRIES[i])).used = false;
        }
        for b in 0..DCACHE_BUCKETS {
            *ptr::addr_of_mut!(BUCKETS[b]) = -1;
        }
        NOCACHE_COUNT = 0;
        PARENT_COUNT = 0;
    }
}

unsafe fn set_nocache_locked(node: *mut VfsNode) {
    // SAFETY: caller holds the dcache lock; fixed static array.
    unsafe {
        for i in 0..NOCACHE_COUNT {
            if *ptr::addr_of!(NOCACHE[i]) == node {
                return;
            }
        }
        if NOCACHE_COUNT < NOCACHE_MAX {
            *ptr::addr_of_mut!(NOCACHE[NOCACHE_COUNT]) = node;
            NOCACHE_COUNT += 1;
        }
    }
}

unsafe fn is_nocache_locked(node: *mut VfsNode) -> bool {
    if node.is_null() {
        return true;
    }
    // SAFETY: caller holds the dcache lock; fixed static array.
    unsafe {
        for i in 0..NOCACHE_COUNT {
            if *ptr::addr_of!(NOCACHE[i]) == node {
                return true;
            }
        }
    }
    false
}

unsafe fn set_parent_locked(child: *mut VfsNode, parent: *mut VfsNode) {
    // SAFETY: caller holds the dcache lock; fixed static array.
    unsafe {
        for i in 0..PARENT_COUNT {
            if (*ptr::addr_of!(PARENTS[i])).0 == child {
                (*ptr::addr_of_mut!(PARENTS[i])).1 = parent;
                return;
            }
        }
        if PARENT_COUNT < PARENT_MAX {
            *ptr::addr_of_mut!(PARENTS[PARENT_COUNT]) = (child, parent);
            PARENT_COUNT += 1;
        }
    }
}

unsafe fn get_parent_locked(node: *mut VfsNode) -> *mut VfsNode {
    // SAFETY: caller holds the dcache lock; fixed static array.
    unsafe {
        for i in 0..PARENT_COUNT {
            let e = *ptr::addr_of!(PARENTS[i]);
            if e.0 == node {
                return e.1;
            }
        }
    }
    ptr::null_mut()
}

/// Record that `child` (a directory) was reached from `parent`.  Used so the
/// final `..` of a dirfd-relative lookup can find its physical parent.
pub unsafe fn set_parent(child: *mut VfsNode, parent: *mut VfsNode) {
    if child.is_null() || child == parent {
        return;
    }
    // SAFETY: the lock serialises the parent table.
    unsafe {
        cact_sync::irq_spinlock_acquire(lock());
        set_parent_locked(child, parent);
        cact_sync::irq_spinlock_release(lock());
    }
}

/// Physical parent of `node`, or null when unknown.
pub unsafe fn parent_of(node: *mut VfsNode) -> *mut VfsNode {
    if node.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: the lock serialises the parent table.
    unsafe {
        cact_sync::irq_spinlock_acquire(lock());
        let r = get_parent_locked(node);
        cact_sync::irq_spinlock_release(lock());
        r
    }
}

/// Remember that `node`'s subtree must not be cached.  Idempotent.  Exported to
/// C so procfs/devfs can mark their dynamic roots.
#[no_mangle]
pub unsafe extern "C" fn vfs_set_nocache(node: *mut VfsNode) {
    if node.is_null() {
        return;
    }
    // SAFETY: the dcache lock serialises registry mutation.
    unsafe {
        cact_sync::irq_spinlock_acquire(lock());
        set_nocache_locked(node);
        cact_sync::irq_spinlock_release(lock());
    }
}

unsafe fn invalidate_locked(parent: *mut VfsNode, name: *const c_char) {
    let b = bucket_of(parent, name);
    // SAFETY: caller holds the lock; manipulating the bucket chain with
    // in-range indices.
    unsafe {
        let mut idx = *ptr::addr_of!(BUCKETS[b]);
        let mut prev: i32 = -1;
        while idx >= 0 {
            let e = &mut *ptr::addr_of_mut!(ENTRIES[idx as usize]);
            if e.used && e.parent == parent && cstr_eq(e.name.as_ptr(), name) {
                if prev < 0 {
                    *ptr::addr_of_mut!(BUCKETS[b]) = e.next;
                } else {
                    (*ptr::addr_of_mut!(ENTRIES[prev as usize])).next = e.next;
                }
                e.used = false;
                return;
            }
            prev = idx;
            idx = e.next;
        }
    }
}

/// Drop the entry for `(parent, name)` if present.
pub unsafe fn invalidate(parent: *mut VfsNode, name: *const c_char) {
    // SAFETY: the lock serialises chain edits.
    unsafe {
        cact_sync::irq_spinlock_acquire(lock());
        invalidate_locked(parent, name);
        cact_sync::irq_spinlock_release(lock());
    }
}

/// Look up `(parent, name)`.  Returns the cached child, or null on a miss.
pub unsafe fn lookup(parent: *mut VfsNode, name: *const c_char) -> *mut VfsNode {
    let b = bucket_of(parent, name);
    // SAFETY: the lock serialises the scan; indices stay inside `ENTRIES`.
    unsafe {
        cact_sync::irq_spinlock_acquire(lock());
        let mut result = ptr::null_mut();
        let mut idx = *ptr::addr_of!(BUCKETS[b]);
        while idx >= 0 {
            let e = &*ptr::addr_of!(ENTRIES[idx as usize]);
            if e.used && e.parent == parent && cstr_eq(e.name.as_ptr(), name) {
                result = e.child;
                break;
            }
            idx = e.next;
        }
        cact_sync::irq_spinlock_release(lock());
        result
    }
}

/// Cache `(parent, name) -> child`.  Skips when `parent` is non-cacheable (and
/// marks `child` non-cacheable so its own lookups stay uncached too).  A full
/// pool simply drops the insertion — correctness does not depend on caching.
pub unsafe fn insert(parent: *mut VfsNode, name: *const c_char, child: *mut VfsNode) {
    if child.is_null() {
        return;
    }
    // SAFETY: the lock serialises pool and registry mutation.
    unsafe {
        cact_sync::irq_spinlock_acquire(lock());

        if is_nocache_locked(parent) {
            set_nocache_locked(child);
            cact_sync::irq_spinlock_release(lock());
            return;
        }

        invalidate_locked(parent, name);

        let mut slot: i32 = -1;
        for i in 0..DCACHE_MAX {
            if !(*ptr::addr_of!(ENTRIES[i])).used {
                slot = i as i32;
                break;
            }
        }
        if slot >= 0 {
            let b = bucket_of(parent, name);
            let e = &mut *ptr::addr_of_mut!(ENTRIES[slot as usize]);
            e.used = true;
            e.parent = parent;
            copy_name(e.name.as_mut_ptr(), name);
            e.child = child;
            e.next = *ptr::addr_of!(BUCKETS[b]);
            *ptr::addr_of_mut!(BUCKETS[b]) = slot;
        }

        cact_sync::irq_spinlock_release(lock());
    }
}
