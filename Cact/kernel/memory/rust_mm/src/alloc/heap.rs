//! Intrusive free-list heap in the fixed `[HEAP_START, HEAP_START+HEAP_SIZE)` window.
//!
//! `kmalloc` / `kfree` are IRQ-spinlocked and back much of the kernel and Rust MM code.

use crate::ffi::*;
use crate::safe::{KStatic, lock_acquire, lock_release, kprint_str, klog_msg};

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
fn heap_addr_in_range(addr: u32) -> bool {
    let start = HEAP_START;
    let end = HEAP_START + HEAP_SIZE;
    addr >= start && addr < end
}

#[unsafe(no_mangle)]
pub extern "C" fn init_heap() {
    if HEAP_START < RESERVED_END {
        klog_msg(LOG_FAIL, b"heap layout invalid\0".as_ptr());
        return;
    }

    unsafe { irq_spinlock_init(HEAP_LOCK.as_ptr() as *mut IrqSpinlock) };
    *HEAP_START_PTR.get_mut() = HEAP_START as *mut HeapBlock;

    let head = *HEAP_START_PTR.get_mut();
    unsafe {
        (*head).magic = HEAP_MAGIC;
        (*head).size = HEAP_SIZE - core::mem::size_of::<HeapBlock>() as u32;
        (*head).is_free = 1;
        (*head).next = core::ptr::null_mut();
    }

}

#[unsafe(no_mangle)]
pub extern "C" fn kmalloc(size: u32) -> *mut u8 {
    if size == 0 {
        return core::ptr::null_mut();
    }
    let user_size = (size + 7) & !7;
    let mut size = user_size + 4;
    size = (size + 7) & !7;
    let hdr_size = core::mem::size_of::<HeapBlock>() as u32;
    lock_acquire(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);

    let mut current = *HEAP_START_PTR.get_mut();
    if (current as u32) < RESERVED_END {
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        kprint_str(b"[FATAL] kmalloc: heap pointer below reserved boundary\n\0".as_ptr());
        return core::ptr::null_mut();
    }
    let mut best_fit: *mut HeapBlock = core::ptr::null_mut();

    while !current.is_null() {
        let magic = unsafe { (*current).magic };
        if magic != HEAP_MAGIC {
            kprint_str(b"[FATAL] Heap corruption detected!\n\0".as_ptr());
            klog_msg(LOG_FAIL, b"heap corruption in allocation walk\0".as_ptr());
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
    klog_msg(LOG_WARN, b"heap out of memory\0".as_ptr());
    core::ptr::null_mut()
}

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

#[unsafe(no_mangle)]
pub extern "C" fn kfree_aligned(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    let raw = unsafe { *((ptr as u32 - 4) as *const u32) } as *mut u8;
    kfree(raw);
}

#[unsafe(no_mangle)]
pub extern "C" fn kfree(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    let hdr_size = core::mem::size_of::<HeapBlock>() as usize;
    lock_acquire(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);

    let block = unsafe { ptr.sub(hdr_size) } as *mut HeapBlock;
    if !heap_addr_in_range(block as u32) {
        klog_msg(LOG_WARN, b"kfree pointer outside heap range\0".as_ptr());
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }

    // Accept free() only for blocks that are still linked in the heap list.
    let mut walk = *HEAP_START_PTR.get_mut();
    let mut found = false;
    while !walk.is_null() {
        if !heap_addr_in_range(walk as u32) {
            kprint_str(b"[FATAL] kfree: walk pointer left heap range\n\0".as_ptr());
            klog_msg(LOG_FAIL, b"heap list pointer out of range\0".as_ptr());
            lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
            return;
        }
        let walk_magic = unsafe { (*walk).magic };
        if walk_magic != HEAP_MAGIC {
            kprint_str(b"[FATAL] kfree: list magic corrupted during lookup\n\0".as_ptr());
            klog_msg(LOG_FAIL, b"heap list magic corrupted\0".as_ptr());
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
        klog_msg(LOG_WARN, b"kfree ignored for unknown block\0".as_ptr());
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }

    let magic = unsafe { (*block).magic };
    if magic != HEAP_MAGIC {
        klog_msg(LOG_WARN, b"kfree bad block magic\0".as_ptr());
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }

    let bsize = unsafe { (*block).size } as usize;
    if bsize < 4 {
        kprint_str(b"[FATAL] kfree: block too small for tail canary\n\0".as_ptr());
        klog_msg(LOG_FAIL, b"heap block too small for canary\0".as_ptr());
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }
    let tail = unsafe { ptr.add(bsize - 4) as *mut u32 };
    let tail_magic = unsafe { *tail };
    if tail_magic != HEAP_TAIL_MAGIC {
        kprint_str(b"[FATAL] kfree: tail canary corrupted (buffer overflow)\n\0".as_ptr());
        klog_msg(LOG_FAIL, b"heap tail canary corrupted\0".as_ptr());
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }

    if unsafe { (*block).is_free } != 0 {
        klog_msg(LOG_WARN, b"double free ignored\0".as_ptr());
        lock_release(HEAP_LOCK.as_ptr() as *mut IrqSpinlock);
        return;
    }

    unsafe { (*block).is_free = 1; }

    let mut curr = *HEAP_START_PTR.get_mut();
    while !curr.is_null() {
        let curr_magic = unsafe { (*curr).magic };
        if curr_magic != HEAP_MAGIC {
            kprint_str(b"[FATAL] kfree: heap corruption at curr\n\0".as_ptr());
            klog_msg(LOG_FAIL, b"heap corruption during coalesce\0".as_ptr());
            break;
        }
        let curr_free = unsafe { (*curr).is_free };
        let next = unsafe { (*curr).next };
        if curr_free != 0 && !next.is_null() {
            let next_magic = unsafe { (*next).magic };
            if next_magic != HEAP_MAGIC {
                kprint_str(b"[FATAL] kfree: next header corrupted, stopping coalesce\n\0".as_ptr());
                klog_msg(LOG_FAIL, b"next heap header corrupted\0".as_ptr());
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