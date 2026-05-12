//! Slab allocator: per-cache freelists, optional ctor/dtor, and a set of generic size caches.

use crate::ffi::*;
use crate::safe::{KStatic, lock_acquire, lock_release, kprint_str, kprint_int, klog_msg};
use crate::pmm::{kalloc, kfree_page};
use crate::alloc::heap::{kmalloc, kfree_heap};

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
static G_CACHE_LOCK: KStatic<IrqSpinlock> = KStatic::new(IrqSpinlock { spin_locked: 0, saved_flags: 0 });

const GENERIC_CACHE_COUNT: usize = 9;
static GENERIC_SIZES: [u32; GENERIC_CACHE_COUNT] = [8, 16, 32, 64, 128, 256, 512, 1024, 2048];
static G_GENERIC_CACHES: KStatic<[*mut SlabCache; GENERIC_CACHE_COUNT]> =
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
    // SAFETY: head and s are valid pointers maintained by the slab allocator.
    unsafe {
        (*s).prev = core::ptr::null_mut();
        (*s).next = *head;
        if !(*head).is_null() {
            (**head).prev = s;
        }
        *head = s;
    }
}

fn list_remove(head: *mut *mut Slab, s: *mut Slab) {
    // SAFETY: head and s are valid pointers maintained by the slab allocator.
    unsafe {
        if !(*s).prev.is_null() {
            (*(*s).prev).next = (*s).next;
        } else {
            *head = (*s).next;
        }
        if !(*s).next.is_null() {
            (*(*s).next).prev = (*s).prev;
        }
        (*s).prev = core::ptr::null_mut();
        (*s).next = core::ptr::null_mut();
    }
}

fn slab_create_slab(cache: *mut SlabCache) -> *mut Slab {
    let page = kalloc();
    if page.is_null() {
        return core::ptr::null_mut();
    }

    // SAFETY: page is a freshly allocated 4 KiB page.
    unsafe {
        let s = page as *mut Slab;
        (*s).next = core::ptr::null_mut();
        (*s).prev = core::ptr::null_mut();
        (*s).inuse = 0;
        (*s).capacity = (*cache).objs_per_slab;
        (*s).cache = cache;

        let obj_base = (page as *mut u8).add(core::mem::size_of::<Slab>());
        let obj_size = (*cache).obj_size;

        (*s).freelist = obj_base;
        for i in 0..(*s).capacity {
            let slot = obj_base.add((i * obj_size) as usize) as *mut *mut u8;
            if i + 1 < (*s).capacity {
                *slot = obj_base.add(((i + 1) * obj_size) as usize);
            } else {
                *slot = core::ptr::null_mut();
            }
            if let Some(ctor) = (*cache).ctor {
                ctor(slot as *mut u8);
            }
        }

        (*cache).total_slabs += 1;
        s
    }
}

fn slab_destroy_slab(cache: *mut SlabCache, s: *mut Slab) {
    // SAFETY: cache and s are valid slab structures.
    unsafe {
        if let Some(dtor) = (*cache).dtor {
            let obj_base = (s as *mut u8).add(core::mem::size_of::<Slab>());
            for i in 0..(*s).capacity {
                dtor(obj_base.add((i * (*cache).obj_size) as usize));
            }
        }
        kfree_page(s as *mut u8);
        (*cache).total_slabs -= 1;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn slab_cache_create(
    name: *const u8,
    mut obj_size: u32,
    ctor: Option<extern "C" fn(*mut u8)>,
    dtor: Option<extern "C" fn(*mut u8)>,
) -> *mut SlabCache {
    if obj_size < SLAB_MIN_OBJ_SIZE {
        obj_size = SLAB_MIN_OBJ_SIZE;
    }
    if obj_size > SLAB_MAX_OBJ_SIZE {
        kprint_str(b"[SLAB] obj_size too large (> 2048)\n\0".as_ptr());
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

    // SAFETY: cache was just allocated; name is a valid C string.
    unsafe {
        let mut i = 0usize;
        while i < SLAB_NAME_LEN - 1 && *name.add(i) != 0 {
            (*cache).name[i] = *name.add(i);
            i += 1;
        }
        (*cache).name[i] = 0;

        (*cache).obj_size = obj_size;
        (*cache).objs_per_slab = calc_objs_per_slab(obj_size);
        (*cache).partial = core::ptr::null_mut();
        (*cache).full = core::ptr::null_mut();
        (*cache).free = core::ptr::null_mut();
        (*cache).total_slabs = 0;
        (*cache).total_allocs = 0;
        (*cache).total_frees = 0;
        (*cache).ctor = ctor;
        (*cache).dtor = dtor;

        lock_acquire(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);
        (*cache).next = *G_CACHE_LIST.get_mut();
        *G_CACHE_LIST.get_mut() = cache;
        lock_release(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);
    }

    cache
}

#[unsafe(no_mangle)]
pub extern "C" fn slab_alloc(cache: *mut SlabCache) -> *mut u8 {
    if cache.is_null() {
        return core::ptr::null_mut();
    }

    lock_acquire(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);

    // SAFETY: cache is valid and we hold the lock.
    let mut s = unsafe { (*cache).partial };

    if s.is_null() {
        // SAFETY: cache is valid.
        s = unsafe { (*cache).free };
        if !s.is_null() {
            list_remove(unsafe { &raw mut (*cache).free }, s);
            list_push(unsafe { &raw mut (*cache).partial }, s);
        }
    }

    if s.is_null() {
        lock_release(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);
        s = slab_create_slab(cache);
        if s.is_null() {
            return core::ptr::null_mut();
        }
        lock_acquire(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);
        list_push(unsafe { &raw mut (*cache).partial }, s);
    }

    // SAFETY: s is a valid slab with free objects.
    let obj = unsafe { (*s).freelist };
    unsafe {
        (*s).freelist = *(obj as *const *mut u8);
        (*s).inuse += 1;
        (*cache).total_allocs += 1;
    }

    let inuse = unsafe { (*s).inuse };
    let capacity = unsafe { (*s).capacity };
    if inuse == capacity {
        list_remove(unsafe { &raw mut (*cache).partial }, s);
        list_push(unsafe { &raw mut (*cache).full }, s);
    }

    lock_release(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);
    obj
}

#[unsafe(no_mangle)]
pub extern "C" fn slab_free(cache: *mut SlabCache, obj: *mut u8) {
    if cache.is_null() || obj.is_null() {
        return;
    }

    lock_acquire(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);

    // SAFETY: obj resides within a page that starts with a Slab header.
    let s = (obj as u32 & !(PAGE_SIZE - 1)) as *mut Slab;

    // SAFETY: s and cache are valid.
    unsafe {
        if (*s).cache != cache {
            kprint_str(b"[SLAB] slab_free: wrong cache!\n\0".as_ptr());
            lock_release(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);
            return;
        }

        *(obj as *mut *mut u8) = (*s).freelist;
        (*s).freelist = obj;

        let was_full = (*s).inuse == (*s).capacity;
        (*s).inuse -= 1;
        (*cache).total_frees += 1;

        if was_full {
            list_remove(&raw mut (*cache).full, s);
            list_push(&raw mut (*cache).partial, s);
        } else if (*s).inuse == 0 {
            list_remove(&raw mut (*cache).partial, s);
            list_push(&raw mut (*cache).free, s);
        }
    }

    lock_release(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);
}

#[unsafe(no_mangle)]
pub extern "C" fn slab_cache_shrink(cache: *mut SlabCache) {
    if cache.is_null() {
        return;
    }
    lock_acquire(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);
    // SAFETY: cache is valid.
    unsafe {
        let mut s = (*cache).free;
        while !s.is_null() {
            let next = (*s).next;
            slab_destroy_slab(cache, s);
            s = next;
        }
        (*cache).free = core::ptr::null_mut();
    }
    lock_release(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);
}

#[unsafe(no_mangle)]
pub extern "C" fn slab_cache_destroy(cache: *mut SlabCache) {
    if cache.is_null() {
        return;
    }

    lock_acquire(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);

    // SAFETY: cache is valid and we hold the lock.
    unsafe {
        let mut s = (*cache).full;
        while !s.is_null() {
            let n = (*s).next;
            slab_destroy_slab(cache, s);
            s = n;
        }
        s = (*cache).partial;
        while !s.is_null() {
            let n = (*s).next;
            slab_destroy_slab(cache, s);
            s = n;
        }
        s = (*cache).free;
        while !s.is_null() {
            let n = (*s).next;
            slab_destroy_slab(cache, s);
            s = n;
        }
        (*cache).full = core::ptr::null_mut();
        (*cache).partial = core::ptr::null_mut();
        (*cache).free = core::ptr::null_mut();

        let mut pp: *mut *mut SlabCache = G_CACHE_LIST.as_ptr();
        while !(*pp).is_null() && *pp != cache {
            pp = &raw mut (**pp).next;
        }
        if !(*pp).is_null() {
            *pp = (*cache).next;
        }
    }

    lock_release(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock);
    kfree_heap(cache as *mut u8);
}

#[unsafe(no_mangle)]
pub extern "C" fn slab_print_stats(cache: *const SlabCache) {
    if cache.is_null() {
        return;
    }

    let mut partial_slabs: u32 = 0;
    let mut full_slabs: u32 = 0;
    let mut free_slabs: u32 = 0;
    let mut free_objs: u32 = 0;

    // SAFETY: cache is valid (checked above).
    unsafe {
        let mut s = (*cache).partial;
        while !s.is_null() {
            partial_slabs += 1;
            free_objs += (*s).capacity - (*s).inuse;
            s = (*s).next;
        }
        s = (*cache).full;
        while !s.is_null() {
            full_slabs += 1;
            s = (*s).next;
        }
        s = (*cache).free;
        while !s.is_null() {
            free_slabs += 1;
            free_objs += (*cache).objs_per_slab;
            s = (*s).next;
        }
    }

    kprint_str(b"[SLAB] cache=\0".as_ptr());
    // SAFETY: cache is valid.
    unsafe { kprint((*cache).name.as_ptr()); }
    kprint_str(b" obj_size=\0".as_ptr());
    unsafe { kprint_int((*cache).obj_size as i32); }
    kprint_str(b" slabs(full/partial/free)=\0".as_ptr());
    kprint_int(full_slabs as i32);
    kprint_str(b"/\0".as_ptr());
    kprint_int(partial_slabs as i32);
    kprint_str(b"/\0".as_ptr());
    kprint_int(free_slabs as i32);
    kprint_str(b" free_objs=\0".as_ptr());
    kprint_int(free_objs as i32);
    kprint_str(b" allocs=\0".as_ptr());
    unsafe { kprint_int((*cache).total_allocs as i32); }
    kprint_str(b"\n\0".as_ptr());
}

#[unsafe(no_mangle)]
pub extern "C" fn slab_init() {
    // SAFETY: boot-time init, single-threaded.
    unsafe { irq_spinlock_init(G_CACHE_LOCK.as_ptr() as *mut IrqSpinlock) };
    *G_CACHE_LIST.get_mut() = core::ptr::null_mut();

    for i in 0..GENERIC_CACHE_COUNT {
        let mut name = [0u8; SLAB_NAME_LEN];
        let prefix = b"kmalloc-";
        name[..prefix.len()].copy_from_slice(prefix);

        let mut num_buf = [0u8; 8];
        let mut sz = GENERIC_SIZES[i];
        let mut p: usize = 6;
        num_buf[7] = 0;
        loop {
            num_buf[p] = b'0' + (sz % 10) as u8;
            sz /= 10;
            if sz == 0 {
                break;
            }
            p -= 1;
        }
        let mut j = prefix.len();
        let mut k = p;
        while k < 7 && j < SLAB_NAME_LEN - 1 {
            name[j] = num_buf[k];
            j += 1;
            k += 1;
        }
        name[j] = 0;

        let cache = slab_cache_create(name.as_ptr(), GENERIC_SIZES[i], None, None);
        G_GENERIC_CACHES.get_mut()[i] = cache;
        if cache.is_null() {
            // SAFETY: kprint_color is a C function that takes a valid string.
            unsafe {
                kprint_color(b"[SLAB] failed to create cache: \0".as_ptr(), COLOR_LIGHT_RED);
                kprint_color(name.as_ptr(), COLOR_LIGHT_RED);
            }
            kprint_str(b"\n\0".as_ptr());
            klog_msg(LOG_FAIL, b"slab cache creation failed\0".as_ptr());
        }
    }

}

#[unsafe(no_mangle)]
pub extern "C" fn slab_kmalloc(size: u32) -> *mut u8 {
    if size == 0 {
        return core::ptr::null_mut();
    }
    let caches = G_GENERIC_CACHES.get_mut();
    for i in 0..GENERIC_CACHE_COUNT {
        if size <= GENERIC_SIZES[i] {
            return slab_alloc(caches[i]);
        }
    }
    kmalloc(size)
}

#[unsafe(no_mangle)]
pub extern "C" fn slab_kfree(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    // SAFETY: obj resides in a page whose start is a Slab header.
    let s = (ptr as u32 & !(PAGE_SIZE - 1)) as *mut Slab;
    unsafe {
        if !(*s).cache.is_null() && (*s).capacity > 0 && (*s).capacity <= 512 {
            slab_free((*s).cache, ptr);
            return;
        }
    }
    kfree_heap(ptr);
}
