use crate::ffi::*;
use crate::safe::{KStatic, lock_acquire, lock_release, kprint_str, kprint_int, kprint_hex, klog_msg};

#[repr(C)]
struct HeapBlock {
    magic: u32,
    size: u32,
    is_free: u32,
    next: *mut HeapBlock,
}

static HEAP_START_PTR: KStatic<*mut HeapBlock> = KStatic::new(HEAP_START as *mut HeapBlock);
static HEAP_LOCK: KStatic<IrqSpinlock> = KStatic::new(IrqSpinlock { spin_locked: 0, saved_flags: 0 });
const HEAP_TAIL_MAGIC: u32 = 0xC0DEC0DE;

#[inline(always)]
fn dbg_return_addr() -> u32 {
    let ret: u32;
    // SAFETY: debug-only best-effort caller address from frame chain.
    unsafe {
        core::arch::asm!(
            "mov eax, [ebp + 4]",
            out("eax") ret,
            options(nostack, preserves_flags)
        );
    }
    ret
}

#[inline(always)]
fn heap_addr_in_range(addr: u32) -> bool {
    let start = HEAP_START;
    let end = HEAP_START + HEAP_SIZE;
    addr >= start && addr < end
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn init_heap() {
    kprint_str(b"[HEAPDBG] build marker: HEAPDBG_V3\n\0".as_ptr());
    if HEAP_START < RESERVED_END {
        kprint_str(b"[HEAP] invalid layout: HEAP_START < RESERVED_END\n\0".as_ptr());
        klog_msg(LOG_FAIL, b"heap layout invalid\0".as_ptr());
        return;
    }

    kprint_str(b"[HEAP] base=0x\0".as_ptr());
    kprint_hex(HEAP_START);
    kprint_str(b"  size=16 MB  header=\0".as_ptr());
    kprint_int(core::mem::size_of::<HeapBlock>() as i32);
    kprint_str(b" B  magic=0x\0".as_ptr());
    kprint_hex(HEAP_MAGIC);
    kprint_str(b"\n\0".as_ptr());

    unsafe { irq_spinlock_init(HEAP_LOCK.as_ptr() as *mut IrqSpinlock) };
    *HEAP_START_PTR.get_mut() = HEAP_START as *mut HeapBlock;

    let head = *HEAP_START_PTR.get_mut();
    unsafe {
        (*head).magic = HEAP_MAGIC;
        (*head).size = HEAP_SIZE - core::mem::size_of::<HeapBlock>() as u32;
        (*head).is_free = 1;
        (*head).next = core::ptr::null_mut();
    }

    kprint_str(b"[HEAP] single free block: usable=\0".as_ptr());
    // SAFETY: head was just initialized above.
    let usable_kb = unsafe { (*head).size / 1024 };
    kprint_int(usable_kb as i32);
    kprint_str(b" KB\n\0".as_ptr());
    klog_msg(LOG_OK, b"heap ready\0".as_ptr());
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn kmalloc(size: u32) -> *mut u8 {
    if size == 0 {
        return core::ptr::null_mut();
    }
    let user_size = (size + 7) & !7;
    let mut size = user_size + 4;
    size = (size + 7) & !7;
    let hdr_size = core::mem::size_of::<HeapBlock>() as u32;
    let caller = dbg_return_addr();

    lock_acquire(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);

    let mut current = *HEAP_START_PTR.get_mut();
    if (current as u32) < RESERVED_END {
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        kprint_str(b"[FATAL] kmalloc: heap pointer below reserved boundary\n\0".as_ptr());
        return core::ptr::null_mut();
    }
    let mut best_fit: *mut HeapBlock = core::ptr::null_mut();
    let mut prev: *mut HeapBlock = core::ptr::null_mut();

    while !current.is_null() {
        let magic = unsafe { (*current).magic };
        if magic != HEAP_MAGIC {
            kprint_str(b"[FATAL] Heap corruption detected!\n\0".as_ptr());
            kprint_str(b"[DBG] kmalloc req=\0".as_ptr());
            kprint_int(size as i32);
            kprint_str(b" caller=0x\0".as_ptr());
            kprint_hex(caller);
            kprint_str(b"[DBG] kmalloc bad block=0x\0".as_ptr());
            kprint_hex(current as u32);
            kprint_str(b" magic=0x\0".as_ptr());
            kprint_hex(magic);
            kprint_str(b" size=\0".as_ptr());
            kprint_int(unsafe { (*current).size } as i32);
            kprint_str(b" is_free=\0".as_ptr());
            kprint_int(unsafe { (*current).is_free } as i32);
            kprint_str(b" next=0x\0".as_ptr());
            kprint_hex(unsafe { (*current).next } as u32);
            if !prev.is_null() {
                kprint_str(b"[DBG] kmalloc prev=0x\0".as_ptr());
                kprint_hex(prev as u32);
                kprint_str(b" prev_magic=0x\0".as_ptr());
                kprint_hex(unsafe { (*prev).magic });
                kprint_str(b" prev_size=\0".as_ptr());
                kprint_int(unsafe { (*prev).size } as i32);
                kprint_str(b" prev_free=\0".as_ptr());
                kprint_int(unsafe { (*prev).is_free } as i32);
                kprint_str(b" prev_next=0x\0".as_ptr());
                kprint_hex(unsafe { (*prev).next } as u32);
            }
            kprint_str(b"\n\0".as_ptr());
            lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
            return core::ptr::null_mut();
        }
        let is_free = unsafe { (*current).is_free };
        let cur_size = unsafe { (*current).size };
        if is_free != 0 && cur_size >= size {
            let best_size = if best_fit.is_null() { u32::MAX } else { unsafe { (*best_fit).size } };
            if cur_size < best_size {
                best_fit = current;
            }
        }
        prev = current;
        current = unsafe { (*current).next };
    }

    if !best_fit.is_null() {
        let bf_size = unsafe { (*best_fit).size };
        if bf_size >= size + hdr_size + 8 {
            unsafe {
                let next_block = (best_fit as *mut u8).add(hdr_size as usize + size as usize)
                    as *mut HeapBlock;
                (*next_block).magic = HEAP_MAGIC;
                (*next_block).size = (*best_fit).size - size - hdr_size;
                (*next_block).is_free = 1;
                (*next_block).next = (*best_fit).next;
                (*best_fit).size = size;
                (*best_fit).next = next_block;
            }
        }
        unsafe { (*best_fit).is_free = 0; }
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        let user_ptr = unsafe { (best_fit as *mut u8).add(hdr_size as usize) };
        let tail = unsafe { user_ptr.add((*best_fit).size as usize - 4) as *mut u32 };
        unsafe { *tail = HEAP_TAIL_MAGIC; }
        return user_ptr;
    }

    lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
    kprint_str(b"[ERR] kmalloc: Out of heap memory\n\0".as_ptr());
    core::ptr::null_mut()
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn kmalloc_aligned(size: u32, align: u32) -> *mut u8 {
    let align = if align < 4 { 4 } else { align };
    let raw = kmalloc(size + align + 4);
    if raw.is_null() {
        return core::ptr::null_mut();
    }
    let addr = raw as u32 + 4;
    let aligned = (addr + align - 1) & !(align - 1);
    unsafe { *((aligned - 4) as *mut u32) = raw as u32; }
    aligned as *mut u8
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn kfree_aligned(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    let raw = unsafe { *((ptr as u32 - 4) as *const u32) } as *mut u8;
    kfree_heap(raw);
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn kfree_heap(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    let hdr_size = core::mem::size_of::<HeapBlock>() as usize;
    lock_acquire(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);

    let block = unsafe { ptr.sub(hdr_size) } as *mut HeapBlock;
    if !heap_addr_in_range(block as u32) {
        kprint_str(b"[ERR] kfree_heap: ptr outside heap range\n\0".as_ptr());
        kprint_str(b"[DBG] free_ptr=0x\0".as_ptr());
        kprint_hex(ptr as u32);
        kprint_str(b" free_block=0x\0".as_ptr());
        kprint_hex(block as u32);
        kprint_str(b"\n\0".as_ptr());
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }

    // Accept free() only for blocks that are still linked in the heap list.
    let mut walk = *HEAP_START_PTR.get_mut();
    let mut found = false;
    while !walk.is_null() {
        if !heap_addr_in_range(walk as u32) {
            kprint_str(b"[FATAL] kfree_heap: walk pointer left heap range\n\0".as_ptr());
            kprint_str(b"[DBG] walk=0x\0".as_ptr());
            kprint_hex(walk as u32);
            kprint_str(b" free_block=0x\0".as_ptr());
            kprint_hex(block as u32);
            kprint_str(b"\n\0".as_ptr());
            lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
            return;
        }
        let walk_magic = unsafe { (*walk).magic };
        if walk_magic != HEAP_MAGIC {
            kprint_str(b"[FATAL] kfree_heap: list magic corrupted during lookup\n\0".as_ptr());
            kprint_str(b"[DBG] walk=0x\0".as_ptr());
            kprint_hex(walk as u32);
            kprint_str(b" walk_magic=0x\0".as_ptr());
            kprint_hex(walk_magic);
            kprint_str(b"\n\0".as_ptr());
            lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
            return;
        }
        if walk == block {
            found = true;
            break;
        }
        walk = unsafe { (*walk).next };
    }
    if !found {
        kprint_str(b"[ERR] kfree_heap: free of non-heap-list block ignored\n\0".as_ptr());
        kprint_str(b"[DBG] free_ptr=0x\0".as_ptr());
        kprint_hex(ptr as u32);
        kprint_str(b" free_block=0x\0".as_ptr());
        kprint_hex(block as u32);
        kprint_str(b"\n\0".as_ptr());
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }

    let magic = unsafe { (*block).magic };
    if magic != HEAP_MAGIC {
        kprint_str(b"[ERR] kfree_heap: bad magic, ignoring\n\0".as_ptr());
        kprint_str(b"[DBG] kfree_heap: ptr=0x\0".as_ptr());
        kprint_hex(ptr as u32);
        kprint_str(b" block=0x\0".as_ptr());
        kprint_hex(block as u32);
        kprint_str(b" magic=0x\0".as_ptr());
        kprint_hex(magic);
        kprint_str(b"\n\0".as_ptr());
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }

    let bsize = unsafe { (*block).size } as usize;
    if bsize < 4 {
        kprint_str(b"[FATAL] kfree_heap: block too small for tail canary\n\0".as_ptr());
        kprint_str(b"[DBG] free_ptr=0x\0".as_ptr());
        kprint_hex(ptr as u32);
        kprint_str(b" free_block=0x\0".as_ptr());
        kprint_hex(block as u32);
        kprint_str(b" bsize=\0".as_ptr());
        kprint_int(bsize as i32);
        kprint_str(b"\n\0".as_ptr());
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }
    let tail = unsafe { ptr.add(bsize - 4) as *mut u32 };
    let tail_magic = unsafe { *tail };
    if tail_magic != HEAP_TAIL_MAGIC {
        kprint_str(b"[FATAL] kfree_heap: tail canary corrupted (buffer overflow)\n\0".as_ptr());
        kprint_str(b"[DBG] free_ptr=0x\0".as_ptr());
        kprint_hex(ptr as u32);
        kprint_str(b" free_block=0x\0".as_ptr());
        kprint_hex(block as u32);
        kprint_str(b" bsize=\0".as_ptr());
        kprint_int(bsize as i32);
        kprint_str(b" tail=0x\0".as_ptr());
        kprint_hex(tail as u32);
        kprint_str(b" tail_magic=0x\0".as_ptr());
        kprint_hex(tail_magic);
        kprint_str(b"\n\0".as_ptr());
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }

    if unsafe { (*block).is_free } != 0 {
        kprint_str(b"[ERR] kfree_heap: double free ignored\n\0".as_ptr());
        kprint_str(b"[DBG] free_ptr=0x\0".as_ptr());
        kprint_hex(ptr as u32);
        kprint_str(b" free_block=0x\0".as_ptr());
        kprint_hex(block as u32);
        kprint_str(b"\n\0".as_ptr());
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }

    unsafe { (*block).is_free = 1; }

    let mut curr = *HEAP_START_PTR.get_mut();
    while !curr.is_null() {
        let curr_magic = unsafe { (*curr).magic };
        if curr_magic != HEAP_MAGIC {
            kprint_str(b"[FATAL] kfree_heap: heap corruption at curr\n\0".as_ptr());
            kprint_str(b"[DBG] curr=0x\0".as_ptr());
            kprint_hex(curr as u32);
            kprint_str(b" curr_magic=0x\0".as_ptr());
            kprint_hex(curr_magic);
            kprint_str(b" free_ptr=0x\0".as_ptr());
            kprint_hex(ptr as u32);
            kprint_str(b" free_block=0x\0".as_ptr());
            kprint_hex(block as u32);
            kprint_str(b"\n\0".as_ptr());
            break;
        }
        let curr_free = unsafe { (*curr).is_free };
        let next = unsafe { (*curr).next };
        if curr_free != 0 && !next.is_null() {
            let next_magic = unsafe { (*next).magic };
            if next_magic != HEAP_MAGIC {
                kprint_str(b"[FATAL] kfree_heap: next header corrupted, stopping coalesce\n\0".as_ptr());
                kprint_str(b"[DBG] curr=0x\0".as_ptr());
                kprint_hex(curr as u32);
                kprint_str(b" curr_size=\0".as_ptr());
                kprint_int(unsafe { (*curr).size } as i32);
                kprint_str(b" next=0x\0".as_ptr());
                kprint_hex(next as u32);
                kprint_str(b" next_magic=0x\0".as_ptr());
                kprint_hex(next_magic);
                kprint_str(b" free_ptr=0x\0".as_ptr());
                kprint_hex(ptr as u32);
                kprint_str(b" free_block=0x\0".as_ptr());
                kprint_hex(block as u32);
                kprint_str(b"\n\0".as_ptr());
                break;
            }
            let next_free = unsafe { (*next).is_free };
            if next_free != 0 {
                let next_size = unsafe { (*next).size };
                let next_next = unsafe { (*next).next };
                unsafe {
                    (*next).magic = 0;
                    (*curr).size += next_size + hdr_size as u32;
                    (*curr).next = next_next;
                }
                continue;
            }
        }
        curr = next;
    }
    lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn get_free_heap_memory() -> u32 {
    let mut free_mem: u32 = 0;
    lock_acquire(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
    let mut current = *HEAP_START_PTR.get_mut();
    while !current.is_null() {
        let is_free = unsafe { (*current).is_free };
        let size = unsafe { (*current).size };
        if is_free != 0 {
            free_mem += size;
        }
        current = unsafe { (*current).next };
    }
    lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
    free_mem
}