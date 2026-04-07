use crate::ffi::*;
use crate::pmm::{kalloc, kfree_page};
use crate::heap::{kmalloc, kfree_heap};

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
    ctor: Option<unsafe extern "C" fn(*mut u8)>,
    dtor: Option<unsafe extern "C" fn(*mut u8)>,
    next: *mut SlabCache,
}

static mut G_CACHE_LIST: *mut SlabCache = core::ptr::null_mut();
static mut G_CACHE_LOCK: IrqSpinlock = IrqSpinlock { spin_locked: 0, saved_flags: 0 };

const GENERIC_CACHE_COUNT: usize = 9;
static GENERIC_SIZES: [u32; GENERIC_CACHE_COUNT] = [8, 16, 32, 64, 128, 256, 512, 1024, 2048];
static mut G_GENERIC_CACHES: [*mut SlabCache; GENERIC_CACHE_COUNT] =
    [core::ptr::null_mut(); GENERIC_CACHE_COUNT];

fn align_up(size: u32, align: u32) -> u32 {
    (size + align - 1) & !(align - 1)
}

fn calc_objs_per_slab(obj_size: u32) -> u32 {
    let usable = PAGE_SIZE - core::mem::size_of::<Slab>() as u32;
    let n = usable / obj_size;
    if n < 1 { 1 } else { n }
}

unsafe fn list_push(head: *mut *mut Slab, s: *mut Slab) {
    (*s).prev = core::ptr::null_mut();
    (*s).next = *head;
    if !(*head).is_null() {
        (**head).prev = s;
    }
    *head = s;
}

unsafe fn list_remove(head: *mut *mut Slab, s: *mut Slab) {
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

unsafe fn slab_create_slab(cache: *mut SlabCache) -> *mut Slab {
    let page = kalloc();
    if page.is_null() {
        return core::ptr::null_mut();
    }

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

unsafe fn slab_destroy_slab(cache: *mut SlabCache, s: *mut Slab) {
    if let Some(dtor) = (*cache).dtor {
        let obj_base = (s as *mut u8).add(core::mem::size_of::<Slab>());
        for i in 0..(*s).capacity {
            dtor(obj_base.add((i * (*cache).obj_size) as usize));
        }
    }
    kfree_page(s as *mut u8);
    (*cache).total_slabs -= 1;
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_cache_create(
    name: *const u8,
    mut obj_size: u32,
    ctor: Option<unsafe extern "C" fn(*mut u8)>,
    dtor: Option<unsafe extern "C" fn(*mut u8)>,
) -> *mut SlabCache {
    if obj_size < SLAB_MIN_OBJ_SIZE {
        obj_size = SLAB_MIN_OBJ_SIZE;
    }
    if obj_size > SLAB_MAX_OBJ_SIZE {
        kprint(b"[SLAB] obj_size too large (> 2048)\n\0".as_ptr());
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

    irq_spinlock_acquire(&raw mut G_CACHE_LOCK);
    (*cache).next = G_CACHE_LIST;
    G_CACHE_LIST = cache;
    irq_spinlock_release(&raw mut G_CACHE_LOCK);

    cache
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_alloc(cache: *mut SlabCache) -> *mut u8 {
    if cache.is_null() {
        return core::ptr::null_mut();
    }

    irq_spinlock_acquire(&raw mut G_CACHE_LOCK);
    let mut s = (*cache).partial;

    if s.is_null() {
        s = (*cache).free;
        if !s.is_null() {
            list_remove(&raw mut (*cache).free, s);
            list_push(&raw mut (*cache).partial, s);
        }
    }

    if s.is_null() {
        irq_spinlock_release(&raw mut G_CACHE_LOCK);
        s = slab_create_slab(cache);
        if s.is_null() {
            return core::ptr::null_mut();
        }
        irq_spinlock_acquire(&raw mut G_CACHE_LOCK);
        list_push(&raw mut (*cache).partial, s);
    }

    let obj = (*s).freelist;
    (*s).freelist = *(obj as *const *mut u8);
    (*s).inuse += 1;
    (*cache).total_allocs += 1;

    if (*s).inuse == (*s).capacity {
        list_remove(&raw mut (*cache).partial, s);
        list_push(&raw mut (*cache).full, s);
    }

    irq_spinlock_release(&raw mut G_CACHE_LOCK);
    obj
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_free(cache: *mut SlabCache, obj: *mut u8) {
    if cache.is_null() || obj.is_null() {
        return;
    }

    irq_spinlock_acquire(&raw mut G_CACHE_LOCK);

    let s = (obj as u32 & !(PAGE_SIZE - 1)) as *mut Slab;

    if (*s).cache != cache {
        kprint(b"[SLAB] slab_free: wrong cache!\n\0".as_ptr());
        irq_spinlock_release(&raw mut G_CACHE_LOCK);
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

    irq_spinlock_release(&raw mut G_CACHE_LOCK);
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_cache_shrink(cache: *mut SlabCache) {
    if cache.is_null() {
        return;
    }
    irq_spinlock_acquire(&raw mut G_CACHE_LOCK);
    let mut s = (*cache).free;
    while !s.is_null() {
        let next = (*s).next;
        slab_destroy_slab(cache, s);
        s = next;
    }
    (*cache).free = core::ptr::null_mut();
    irq_spinlock_release(&raw mut G_CACHE_LOCK);
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_cache_destroy(cache: *mut SlabCache) {
    if cache.is_null() {
        return;
    }

    irq_spinlock_acquire(&raw mut G_CACHE_LOCK);

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

    let mut pp: *mut *mut SlabCache = &raw mut G_CACHE_LIST;
    while !(*pp).is_null() && *pp != cache {
        pp = &raw mut (**pp).next;
    }
    if !(*pp).is_null() {
        *pp = (*cache).next;
    }

    irq_spinlock_release(&raw mut G_CACHE_LOCK);
    kfree_heap(cache as *mut u8);
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_print_stats(cache: *const SlabCache) {
    if cache.is_null() {
        return;
    }

    let mut partial_slabs: u32 = 0;
    let mut full_slabs: u32 = 0;
    let mut free_slabs: u32 = 0;
    let mut free_objs: u32 = 0;

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

    let mut buf = [0u8; 16];
    kprint(b"[SLAB] cache=\0".as_ptr());
    kprint((*cache).name.as_ptr());
    kprint(b" obj_size=\0".as_ptr());
    itoa((*cache).obj_size as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b" slabs(full/partial/free)=\0".as_ptr());
    itoa(full_slabs as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"/\0".as_ptr());
    itoa(partial_slabs as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"/\0".as_ptr());
    itoa(free_slabs as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b" free_objs=\0".as_ptr());
    itoa(free_objs as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b" allocs=\0".as_ptr());
    itoa((*cache).total_allocs as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"\n\0".as_ptr());
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_init() {
    kprint(b"[SLAB] initializing  generic caches: 8 16 32 64 128 256 512 1024 2048 B\n\0".as_ptr());

    irq_spinlock_init(&raw mut G_CACHE_LOCK);
    G_CACHE_LIST = core::ptr::null_mut();

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

        G_GENERIC_CACHES[i] =
            slab_cache_create(name.as_ptr(), GENERIC_SIZES[i], None, None);
        if G_GENERIC_CACHES[i].is_null() {
            kprint_color(
                b"[SLAB] failed to create cache: \0".as_ptr(),
                COLOR_LIGHT_RED,
            );
            kprint_color(name.as_ptr(), COLOR_LIGHT_RED);
            kprint(b"\n\0".as_ptr());
            klog(LOG_FAIL, b"slab cache creation failed\0".as_ptr());
        }
    }

    let mut buf = [0u8; 4];
    kprint(b"[SLAB] \0".as_ptr());
    itoa(GENERIC_CACHE_COUNT as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b" generic caches ready\n\0".as_ptr());
    klog(LOG_OK, b"slab allocator ready\0".as_ptr());
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_kmalloc(size: u32) -> *mut u8 {
    if size == 0 {
        return core::ptr::null_mut();
    }
    for i in 0..GENERIC_CACHE_COUNT {
        if size <= GENERIC_SIZES[i] {
            return slab_alloc(G_GENERIC_CACHES[i]);
        }
    }
    kmalloc(size)
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn slab_kfree(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    let s = (ptr as u32 & !(PAGE_SIZE - 1)) as *mut Slab;

    if !(*s).cache.is_null() && (*s).capacity > 0 && (*s).capacity <= 512 {
        slab_free((*s).cache, ptr);
        return;
    }
    kfree_heap(ptr);
}