use crate::ffi::*;

#[repr(C)]
struct HeapBlock {
    magic: u32,
    size: u32,
    is_free: u32,
    next: *mut HeapBlock,
}

static mut HEAP_START_PTR: *mut HeapBlock = HEAP_START as *mut HeapBlock;
static mut HEAP_LOCK: IrqSpinlock = IrqSpinlock { spin_locked: 0, saved_flags: 0 };

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn init_heap() {
    let mut buf = [0u8; 16];
    kprint(b"[HEAP] base=0x\0".as_ptr());
    hex_to_ascii(HEAP_START, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"  size=16 MB  header=\0".as_ptr());
    itoa(core::mem::size_of::<HeapBlock>() as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b" B  magic=0x\0".as_ptr());
    hex_to_ascii(HEAP_MAGIC, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"\n\0".as_ptr());

    irq_spinlock_init(&raw mut HEAP_LOCK);
    HEAP_START_PTR = HEAP_START as *mut HeapBlock;

    (*HEAP_START_PTR).magic = HEAP_MAGIC;
    (*HEAP_START_PTR).size = HEAP_SIZE - core::mem::size_of::<HeapBlock>() as u32;
    (*HEAP_START_PTR).is_free = 1;
    (*HEAP_START_PTR).next = core::ptr::null_mut();

    kprint(b"[HEAP] single free block: usable=\0".as_ptr());
    itoa(((*HEAP_START_PTR).size / 1024) as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b" KB\n\0".as_ptr());
    klog(LOG_OK, b"heap ready\0".as_ptr());
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kmalloc(size: u32) -> *mut u8 {
    if size == 0 {
        return core::ptr::null_mut();
    }
    let size = (size + 7) & !7;
    let hdr_size = core::mem::size_of::<HeapBlock>() as u32;

    irq_spinlock_acquire(&raw mut HEAP_LOCK);

    let mut current = HEAP_START_PTR;
    let mut best_fit: *mut HeapBlock = core::ptr::null_mut();

    while !current.is_null() {
        if (*current).magic != HEAP_MAGIC {
            kprint(b"[FATAL] Heap corruption detected!\n\0".as_ptr());
            irq_spinlock_release(&raw mut HEAP_LOCK);
            return core::ptr::null_mut();
        }
        if (*current).is_free != 0 && (*current).size >= size {
            if best_fit.is_null() || (*current).size < (*best_fit).size {
                best_fit = current;
            }
        }
        current = (*current).next;
    }

    if !best_fit.is_null() {
        if (*best_fit).size >= size + hdr_size + 8 {
            let next_block = (best_fit as *mut u8).add(hdr_size as usize + size as usize)
                as *mut HeapBlock;
            (*next_block).magic = HEAP_MAGIC;
            (*next_block).size = (*best_fit).size - size - hdr_size;
            (*next_block).is_free = 1;
            (*next_block).next = (*best_fit).next;
            (*best_fit).size = size;
            (*best_fit).next = next_block;
        }
        (*best_fit).is_free = 0;
        irq_spinlock_release(&raw mut HEAP_LOCK);
        return (best_fit as *mut u8).add(hdr_size as usize);
    }

    irq_spinlock_release(&raw mut HEAP_LOCK);
    kprint(b"[ERR] kmalloc: Out of heap memory\n\0".as_ptr());
    core::ptr::null_mut()
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kmalloc_aligned(size: u32, align: u32) -> *mut u8 {
    let raw = kmalloc(size + align);
    if raw.is_null() {
        return core::ptr::null_mut();
    }
    let addr = raw as u32;
    let aligned = (addr + align - 1) & !(align - 1);
    aligned as *mut u8
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kfree_heap(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    let hdr_size = core::mem::size_of::<HeapBlock>() as usize;
    irq_spinlock_acquire(&raw mut HEAP_LOCK);

    let block = ptr.sub(hdr_size) as *mut HeapBlock;
    if (*block).magic == HEAP_MAGIC {
        (*block).is_free = 1;

        let mut curr = HEAP_START_PTR;
        while !curr.is_null() && !(*curr).next.is_null() {
            if (*curr).is_free != 0 && (*(*curr).next).is_free != 0 {
                (*curr).size += (*(*curr).next).size + hdr_size as u32;
                (*curr).next = (*(*curr).next).next;
            } else {
                curr = (*curr).next;
            }
        }
    }
    irq_spinlock_release(&raw mut HEAP_LOCK);
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn get_free_heap_memory() -> u32 {
    let mut free_mem: u32 = 0;
    irq_spinlock_acquire(&raw mut HEAP_LOCK);
    let mut current = HEAP_START_PTR;
    while !current.is_null() {
        if (*current).is_free != 0 {
            free_mem += (*current).size;
        }
        current = (*current).next;
    }
    irq_spinlock_release(&raw mut HEAP_LOCK);
    free_mem
}