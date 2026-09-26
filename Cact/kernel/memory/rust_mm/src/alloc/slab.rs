//! Slab allocator: per-cache freelists, optional ctor/dtor, and a set of generic size caches.

use crate::ffi::*;
use crate::safe::{KStatic, lock_acquire, lock_release, kprint_str, kprint_int};
use crate::pmm::{kalloc, free_page};
use crate::alloc::heap::{kmalloc, kfree};

#[repr(C)]
pub struct Slab {
    next: *mut Slab,
    prev: *mut Slab,
    freelist: *mut u8,
    inuse: u32,
    capacity: u32,
    cache: *mut SlabCache,
}

#[repr(C)]
pub struct SlabCache {
    name: [u8; SLAB_NAME_LEN],
    obj_size: u32,
    objs_per_slab: u32,
    partial: *mut Slab,
    full: *mut Slab,
    free: *mut Slab,
    total_slabs: u32,
    total_allocs: u32,
    total_frees: u32,
    ctor: Option<extern "C" fn(*mut u8)>,
    dtor: Option<extern "C" fn(*mut u8)>,
    next: *mut SlabCache,
}

static G_CACHE_LIST: KStatic<*mut SlabCache> = KStatic::new(core::ptr::null_mut());
pub(crate) static G_CACHE_LOCK: KStatic<IrqSpinlock> = KStatic::new(IrqSpinlock { spin_locked: 0, saved_flags: 0 });

/// Number of generic (kmalloc-style) size caches.
pub(crate) const GENERIC_CACHE_COUNT: usize = 9;

pub(crate) static G_GENERIC_CACHES: KStatic<[*mut SlabCache; GENERIC_CACHE_COUNT]> =
    KStatic::new([core::ptr::null_mut(); GENERIC_CACHE_COUNT]);

fn align_up(size: u32, align: u32) -> u32 {
    (size + align - 1) & !(align - 1)
}

fn calc_objs_per_slab(obj_size: u32) -> u32 {
    let usable = PAGE_SIZE - core::mem::size_of::<Slab>() as u32;
    let n = usable / obj_size;
    if n < 1 { 1 } else { n }
}

fn list_push(head: *mut *mut Slab, s: *mut Slab) {
    // SAFETY: `s` is a live `Slab` pointer maintained by the slab allocator.
    unsafe { (*s).prev = core::ptr::null_mut() };
    // SAFETY: `head` is the list-head slot, which holds the current head (or null).
    let old_head = unsafe { *head };
    // SAFETY: `s` is the node being linked.
    unsafe { (*s).next = old_head };
    if !old_head.is_null() {
        // SAFETY: `old_head` is the current list head (non-null, checked above), a
        // live `Slab`, and its `prev` link is updated in bounds.
        unsafe { (*old_head).prev = s };
    }
    // SAFETY: `head` is the list-head slot.
    unsafe { *head = s };
}

fn list_remove(head: *mut *mut Slab, s: *mut Slab) {
    // SAFETY: `s` is a live `Slab` node maintained by the slab allocator; these two
    // reads capture its links before they are modified.
    let prev = unsafe { (*s).prev };
    // SAFETY: as above, for the `next` link.
    let next = unsafe { (*s).next };
    if !prev.is_null() {
        // SAFETY: `prev` is the live node before `s` (non-null, checked above).
        unsafe { (*prev).next = next };
    } else {
        // SAFETY: `head` is the list-head slot and `s` was its head.
        unsafe { *head = next };
    }
    if !next.is_null() {
        // SAFETY: `next` is the live node after `s` (non-null, checked above).
        unsafe { (*next).prev = prev };
    }
    // SAFETY: `s` is the node being unlinked; clearing its links is in bounds.
    unsafe { (*s).prev = core::ptr::null_mut() };
    // SAFETY: as above, for the `next` link.
    unsafe { (*s).next = core::ptr::null_mut() };
}

fn slab_create_slab(cache: *mut SlabCache) -> *mut Slab {
    let page = kalloc();
    if page.is_null() {
        return core::ptr::null_mut();
    }

    // SAFETY: `page` is the fresh `kalloc` page just checked non-null, so it is a
    // live, exclusively-owned slab header.
    let s = unsafe { &mut *(page as *mut Slab) };
    // SAFETY: `cache` is the live cache whose slab this is (per the caller
    // contract); the fresh slab above does not alias it.
    let cache_ref = unsafe { &mut *cache };
    s.next = core::ptr::null_mut();
    s.prev = core::ptr::null_mut();
    s.inuse = 0;
    s.capacity = cache_ref.objs_per_slab;
    s.cache = cache;

    // SAFETY: `page` addresses the whole 4 KiB slab page and `size_of::<Slab>() <
    // PAGE_SIZE`, so this offset into the page is in bounds.
    let obj_base = unsafe { page.add(core::mem::size_of::<Slab>()) };
    let obj_size = cache_ref.obj_size;

    s.freelist = obj_base;
    let capacity = s.capacity;
    for i in 0..capacity {
        // SAFETY: object `i`'s freelist slot lies inside the page's usable area, so
        // this pointer is in bounds.
        let slot = unsafe { obj_base.add((i * obj_size) as usize) as *mut *mut u8 };
        if i + 1 < capacity {
            // SAFETY: `(i + 1) * obj_size` is the next freelist slot, still inside
            // the page's usable area, so this pointer is in bounds.
            let next_slot = unsafe { obj_base.add(((i + 1) * obj_size) as usize) };
            // SAFETY: `slot` points at one in-bounds freelist slot.
            unsafe { *slot = next_slot };
        } else {
            // SAFETY: `slot` points at one in-bounds freelist slot.
            unsafe { *slot = core::ptr::null_mut() };
        }
        if let Some(ctor) = cache_ref.ctor {
            ctor(slot as *mut u8);
        }
    }

    cache_ref.total_slabs += 1;
    s as *mut Slab
}

fn slab_destroy_slab(cache: *mut SlabCache, s: *mut Slab) {
    // SAFETY: `cache` is the live cache that owns `s` (per the caller contract),
    // so this shared borrow is valid for the whole teardown.
    let cache_ref = unsafe { &*cache };
    if let Some(dtor) = cache_ref.dtor {
        // SAFETY: `s` is a live slab page and `size_of::<Slab>() < PAGE_SIZE`, so
        // this offset into the page is in bounds.
        let obj_base = unsafe { (s as *mut u8).add(core::mem::size_of::<Slab>()) };
        // SAFETY: `s` is a live `Slab` header, so this capacity read is in bounds.
        let capacity = unsafe { (*s).capacity };
        for i in 0..capacity {
            // SAFETY: object `i` lies inside the page's usable area, so this pointer
            // is in bounds.
            let obj = unsafe { obj_base.add((i * cache_ref.obj_size) as usize) };
            dtor(obj);
        }
    }
    // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
    unsafe { free_page(s as *mut u8) };
    // SAFETY: `s` is a live `Slab` header owned by `cache`, so this counter update
    // is in bounds; the caller serialises concurrent use.
    unsafe { (*cache).total_slabs -= 1 };
}

/// # Safety
///
/// `name` must be null or a readable, NUL-terminated byte string of at most
/// `SLAB_NAME_LEN` bytes valid for the call.  `ctor`/`dtor` must be valid
/// function pointers for the objects this cache will hand out.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_cache_create(
    name: *const u8,
    mut obj_size: u32,
    ctor: Option<extern "C" fn(*mut u8)>,
    dtor: Option<extern "C" fn(*mut u8)>,
) -> *mut SlabCache {
    if obj_size < SLAB_MIN_OBJ_SIZE {
        obj_size = SLAB_MIN_OBJ_SIZE;
    }
    if obj_size > SLAB_MAX_OBJ_SIZE {
        kprint_str(c"[SLAB] obj_size too large (> 2048)\n".as_ptr() as *const u8);
        return core::ptr::null_mut();
    }
    if obj_size < core::mem::size_of::<*mut u8>() as u32 {
        obj_size = core::mem::size_of::<*mut u8>() as u32;
    }
    obj_size = align_up(obj_size, 8);

    let cache = kmalloc(core::mem::size_of::<SlabCache>() as u32) as *mut SlabCache;
    if cache.is_null() {
        return core::ptr::null_mut();
    }

    // SAFETY: `cache` was just allocated by `kmalloc` (checked non-null above) and
    // is exclusively owned by this call.
    let cache_ref = unsafe { &mut *cache };
    let mut i = 0usize;
    while i < SLAB_NAME_LEN - 1 {
        // SAFETY: `i < SLAB_NAME_LEN - 1` and the caller contract makes `name` a
        // readable NUL-terminated string of at most `SLAB_NAME_LEN` bytes, so this
        // byte pointer is in bounds.
        let p = unsafe { name.add(i) };
        // SAFETY: `p` points at one byte of that string.
        let ch = unsafe { *p };
        if ch == 0 {
            break;
        }
        cache_ref.name[i] = ch;
        i += 1;
    }
    cache_ref.name[i] = 0;

    cache_ref.obj_size = obj_size;
    cache_ref.objs_per_slab = calc_objs_per_slab(obj_size);
    cache_ref.partial = core::ptr::null_mut();
    cache_ref.full = core::ptr::null_mut();
    cache_ref.free = core::ptr::null_mut();
    cache_ref.total_slabs = 0;
    cache_ref.total_allocs = 0;
    cache_ref.total_frees = 0;
    cache_ref.ctor = ctor;
    cache_ref.dtor = dtor;

    lock_acquire(G_CACHE_LOCK.as_ptr());
    // SAFETY: `G_CACHE_LIST` is the global cache-list head; the lock is held, so
    // this borrow is exclusive for the relink.
    let head = unsafe { *KStatic::get_mut(G_CACHE_LIST.as_ptr()) };
    cache_ref.next = head;
    // SAFETY: as above — publishing the new node at the list head under
    // `G_CACHE_LOCK`.
    unsafe { *KStatic::get_mut(G_CACHE_LIST.as_ptr()) = cache };
    lock_release(G_CACHE_LOCK.as_ptr());

    cache
}

/// # Safety
///
/// `cache` must be null or a pointer returned by `slab_cache_create` that is
/// still alive; the caller must not destroy it concurrently.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_alloc(cache: *mut SlabCache) -> *mut u8 {
    if cache.is_null() {
        return core::ptr::null_mut();
    }

    lock_acquire(G_CACHE_LOCK.as_ptr());

    // SAFETY: cache is valid and we hold the lock.
    let mut s = unsafe { (*cache).partial };

    if s.is_null() {
        // SAFETY: cache is valid.
        s = unsafe { (*cache).free };
        if !s.is_null() {
            // SAFETY: `&raw mut (*cache).free` — `cache` is non-null and live (the
            // caller of `slab_alloc` guarantees it) and `G_CACHE_LOCK` is held.
            list_remove(unsafe { &raw mut (*cache).free }, s);
            // SAFETY: `&raw mut (*cache).partial` of the same live cache under
            // `G_CACHE_LOCK`.
            list_push(unsafe { &raw mut (*cache).partial }, s);
        }
    }

    if s.is_null() {
        lock_release(G_CACHE_LOCK.as_ptr());
        s = slab_create_slab(cache);
        if s.is_null() {
            return core::ptr::null_mut();
        }
        lock_acquire(G_CACHE_LOCK.as_ptr());
        // SAFETY: `&raw mut (*cache).partial` of the live cache, under `G_CACHE_LOCK`.
        list_push(unsafe { &raw mut (*cache).partial }, s);
    }

    // SAFETY: s is a valid slab with free objects.
    let obj = unsafe { (*s).freelist };
    // SAFETY: `obj` is the first free object of `s`, so reading the next-free
    // pointer stored in its first word is valid.
    let next_free = unsafe { *(obj as *const *mut u8) };
    // SAFETY: `s` is a slab with free objects and `G_CACHE_LOCK` is held, so its
    // freelist is exclusively owned here.
    unsafe { (*s).freelist = next_free };
    // SAFETY: as above — the slab's in-use count.
    unsafe { (*s).inuse += 1 };
    // SAFETY: as above — the owning cache's allocation counter.
    unsafe { (*cache).total_allocs += 1 };

    // SAFETY: `(*s).inuse` of the slab under `G_CACHE_LOCK`.
    let inuse = unsafe { (*s).inuse };
    // SAFETY: `(*s).capacity` of the same slab.
    let capacity = unsafe { (*s).capacity };
    if inuse == capacity {
        // SAFETY: moving the now-full slab from partial to full, under `G_CACHE_LOCK`.
        list_remove(unsafe { &raw mut (*cache).partial }, s);
        // SAFETY: `&raw mut (*cache).full` of the live cache under `G_CACHE_LOCK`.
        list_push(unsafe { &raw mut (*cache).full }, s);
    }

    lock_release(G_CACHE_LOCK.as_ptr());
    obj
}

/// # Safety
///
/// `cache` must be a live cache from `slab_cache_create` and `obj` must be a
/// pointer currently allocated from that same cache (or null).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_free(cache: *mut SlabCache, obj: *mut u8) {
    if cache.is_null() || obj.is_null() {
        return;
    }

    lock_acquire(G_CACHE_LOCK.as_ptr());

    // SAFETY: obj resides within a page that starts with a Slab header.
    let s = (obj as u32 & !(PAGE_SIZE - 1)) as *mut Slab;

    // SAFETY: `cache` is a live cache (per the caller contract); `G_CACHE_LOCK` is
    // held, so this borrow is exclusive for the counter updates below.
    let cache_ref = unsafe { &mut *cache };
    {
        // SAFETY: `s` is the slab page containing `obj`, and `G_CACHE_LOCK` is held,
        // so this borrow of the header is exclusive; it ends before the list
        // operations below re-touch `s` through the raw pointer.
        let s_ref = unsafe { &mut *s };
        if s_ref.cache != cache {
            kprint_str(c"[SLAB] slab_free: wrong cache!\n".as_ptr() as *const u8);
            lock_release(G_CACHE_LOCK.as_ptr());
            return;
        }

        // SAFETY: `obj` is a live object of this slab, so storing the current
        // freelist head into its first word is in bounds.
        unsafe { *(obj as *mut *mut u8) = s_ref.freelist };
        s_ref.freelist = obj;

        // SAFETY: the slab's counters are exclusive under `G_CACHE_LOCK`.
        let was_full = s_ref.inuse == s_ref.capacity;
        s_ref.inuse -= 1;
        cache_ref.total_frees += 1;

        if was_full {
            list_remove(&raw mut cache_ref.full, s);
            list_push(&raw mut cache_ref.partial, s);
        } else if s_ref.inuse == 0 {
            list_remove(&raw mut cache_ref.partial, s);
            list_push(&raw mut cache_ref.free, s);
        }
    }

    lock_release(G_CACHE_LOCK.as_ptr());
}

/// # Safety
///
/// `cache` must be null or a live cache created by `slab_cache_create`, and it
/// must not be destroyed concurrently.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_cache_shrink(cache: *mut SlabCache) {
    if cache.is_null() {
        return;
    }
    lock_acquire(G_CACHE_LOCK.as_ptr());
    // SAFETY: `cache` is a live cache (per the caller contract) and
    // `G_CACHE_LOCK` is held, so the free-slab list head is exclusively read here.
    let mut s = unsafe { (*cache).free };
    while !s.is_null() {
        // SAFETY: `s` is a live slab node of that list.
        let next = unsafe { (*s).next };
        slab_destroy_slab(cache, s);
        s = next;
    }
    // SAFETY: clearing the free-slab list head of the live cache, under the lock.
    unsafe { (*cache).free = core::ptr::null_mut() };
    lock_release(G_CACHE_LOCK.as_ptr());
}

/// # Safety
///
/// `cache` must be null or a pointer returned by `slab_cache_create`; it is
/// freed here and must not be used again afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_cache_destroy(cache: *mut SlabCache) {
    if cache.is_null() {
        return;
    }

    lock_acquire(G_CACHE_LOCK.as_ptr());

    // SAFETY: `cache` is a live cache (per the caller contract) and
    // `G_CACHE_LOCK` is held, so the slab lists are exclusively owned here.
    let mut s = unsafe { (*cache).full };
    while !s.is_null() {
        // SAFETY: `s` is a live slab node of the full list.
        let n = unsafe { (*s).next };
        slab_destroy_slab(cache, s);
        s = n;
    }
    // SAFETY: as above, for the partial list head.
    s = unsafe { (*cache).partial };
    while !s.is_null() {
        // SAFETY: `s` is a live slab node of the partial list.
        let n = unsafe { (*s).next };
        slab_destroy_slab(cache, s);
        s = n;
    }
    // SAFETY: as above, for the free list head.
    s = unsafe { (*cache).free };
    while !s.is_null() {
        // SAFETY: `s` is a live slab node of the free list.
        let n = unsafe { (*s).next };
        slab_destroy_slab(cache, s);
        s = n;
    }
    // SAFETY: clearing the now-empty slab-list heads of the live cache.
    unsafe { (*cache).full = core::ptr::null_mut() };
    // SAFETY: as above, for the partial list.
    unsafe { (*cache).partial = core::ptr::null_mut() };
    // SAFETY: as above, for the free list.
    unsafe { (*cache).free = core::ptr::null_mut() };

    let mut pp: *mut *mut SlabCache = G_CACHE_LIST.as_ptr();
    loop {
        // SAFETY: `pp` is the address of a `next` link of the global cache list
        // (starting at its head), so this reads a live link.
        let p = unsafe { *pp };
        if p.is_null() || p == cache {
            break;
        }
        // SAFETY: `p` is a live cache node (non-null, checked above), so the address
        // of its `next` link is valid.
        pp = unsafe { &raw mut (*p).next };
    }
    // SAFETY: `pp` is the in-bounds list link that points at `cache` (or the list
    // tail), read under `G_CACHE_LOCK`.
    let slot = unsafe { *pp };
    if !slot.is_null() {
        // SAFETY: `slot` is `cache`, so its `next` link is in bounds.
        let next = unsafe { (*cache).next };
        // SAFETY: `pp` is the in-bounds list link, relinked past `cache`.
        unsafe { *pp = next };
    }

    lock_release(G_CACHE_LOCK.as_ptr());
    // SAFETY: `cache` was allocated with `kmalloc` in `slab_cache_create` and has
    // just been unlinked from `G_CACHE_LIST`, so it is safe to return to the heap.
    unsafe { kfree(cache as *mut u8); }
}

/// # Safety
///
/// `cache` must be null or point to a live, initialised `SlabCache` that stays
/// valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_print_stats(cache: *const SlabCache) {
    if cache.is_null() {
        return;
    }

    let mut partial_slabs: u32 = 0;
    let mut full_slabs: u32 = 0;
    let mut free_slabs: u32 = 0;
    let mut free_objs: u32 = 0;

    // SAFETY: `cache` is a valid `SlabCache` (checked non-null above); this shared
    // borrow is consumed by the field reads below, and no call in between mutates
    // it.
    let cache = unsafe { &*cache };
    let mut s = cache.partial;
    while !s.is_null() {
        partial_slabs += 1;
        // SAFETY: `s` is a live slab node of the partial list, so this shared borrow
        // is in bounds and consumed by the two field reads.
        let slab = unsafe { &*s };
        free_objs += slab.capacity - slab.inuse;
        s = slab.next;
    }
    s = cache.full;
    while !s.is_null() {
        full_slabs += 1;
        // SAFETY: `s` is a live slab node of the full list.
        s = unsafe { (*s).next };
    }
    s = cache.free;
    while !s.is_null() {
        free_slabs += 1;
        free_objs += cache.objs_per_slab;
        // SAFETY: `s` is a live slab node of the free list.
        s = unsafe { (*s).next };
    }

    kprint_str(c"[SLAB] cache=".as_ptr() as *const u8);
    // SAFETY: `cache` is a live `SlabCache` and `name` is a NUL-terminated array
    // inside it, so `printk` reads a valid string.
    unsafe { printk(cache.name.as_ptr()) };
    kprint_str(c" obj_size=".as_ptr() as *const u8);
    kprint_int(cache.obj_size as i32);
    kprint_str(c" slabs(full/partial/free)=".as_ptr() as *const u8);
    kprint_int(full_slabs as i32);
    kprint_str(c"/".as_ptr() as *const u8);
    kprint_int(partial_slabs as i32);
    kprint_str(c"/".as_ptr() as *const u8);
    kprint_int(free_slabs as i32);
    kprint_str(c" free_objs=".as_ptr() as *const u8);
    kprint_int(free_objs as i32);
    kprint_str(c" allocs=".as_ptr() as *const u8);
    kprint_int(cache.total_allocs as i32);
    kprint_str(c"\n".as_ptr() as *const u8);
}

#[path = "slab_generic.rs"]
mod slab_generic;
pub use slab_generic::*;
