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
        klog_msg(LOG_FAIL, c"heap layout invalid".as_ptr() as *const u8);
        return;
    }

    // SAFETY: boot-time initialisation of `HEAP_LOCK` before any kernel thread or
    // SMP AP exists; single-threaded.
    unsafe { irq_spinlock_init(HEAP_LOCK.as_ptr()) };
    // SAFETY: `HEAP_START_PTR` is seeded with the start of the dedicated heap
    // window during single-threaded boot; no concurrent allocator call yet.
    *unsafe { KStatic::get_mut(HEAP_START_PTR.as_ptr()) } = HEAP_START as *mut HeapBlock;

    // SAFETY: `HEAP_START_PTR` read during the same single-threaded init.
    let head = *unsafe { KStatic::get_mut(HEAP_START_PTR.as_ptr()) };
    // SAFETY: `head` is the freshly seeded first `HeapBlock` at `HEAP_START`, a
    // 16 MB identity-mapped window reserved for the heap; it is large enough for
    // the header it is being initialised with and this is its only reference
    // during single-threaded init.
    let head = unsafe { &mut *head };
    head.magic = HEAP_MAGIC;
    head.size = HEAP_SIZE - core::mem::size_of::<HeapBlock>() as u32;
    head.is_free = 1;
    head.next = core::ptr::null_mut();

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
    lock_acquire(HEAP_LOCK.as_ptr());

    // SAFETY: `HEAP_START_PTR` read under `HEAP_LOCK`, acquired above, so the list
    // head cannot be swapped by a concurrent allocation.
    let mut current = *unsafe { KStatic::get_mut(HEAP_START_PTR.as_ptr()) };
    if (current as u32) < RESERVED_END {
        lock_release(HEAP_LOCK.as_ptr());
        kprint_str(c"[FATAL] kmalloc: heap pointer below reserved boundary\n".as_ptr() as *const u8);
        return core::ptr::null_mut();
    }
    let mut best_fit: *mut HeapBlock = core::ptr::null_mut();

    while !current.is_null() {
        // SAFETY: `current` walks the heap list under `HEAP_LOCK`; every node was
        // range-checked when the list was built.
        let magic = unsafe { (*current).magic };
        if magic != HEAP_MAGIC {
            kprint_str(c"[FATAL] Heap corruption detected!\n".as_ptr() as *const u8);
            klog_msg(LOG_FAIL, c"heap corruption in allocation walk".as_ptr() as *const u8);
            lock_release(HEAP_LOCK.as_ptr());
            return core::ptr::null_mut();
        }
        // SAFETY: `(*current).is_free` of the same locked heap-list node.
        let is_free = unsafe { (*current).is_free };
        // SAFETY: `(*current).size` of the same locked heap-list node.
        let cur_size = unsafe { (*current).size };
        if is_free != 0 && cur_size >= size {
            // SAFETY: `best_fit` is a heap-list node found by this same locked walk.
            let best_size = if best_fit.is_null() { u32::MAX } else { unsafe { (*best_fit).size } };
            if cur_size < best_size {
                best_fit = current;
            }
        }
        // SAFETY: following the `next` link of a locked heap-list node.
        current = unsafe { (*current).next };
    }

    if !best_fit.is_null() {
        // SAFETY: `(*best_fit).size` of a locked heap-list node.
        let bf_size = unsafe { (*best_fit).size };
        if bf_size >= size + hdr_size + 8 {
            // SAFETY: splitting `best_fit` under `HEAP_LOCK`: `next_block` lies
            // `hdr_size + size` bytes into the block, which the size check above
            // proved is still inside that block's own extent in the heap window.
            let next_block = unsafe {
                (best_fit as *mut u8).add(hdr_size as usize + size as usize) as *mut HeapBlock
            };
            // SAFETY: `next_block` is the freshly-carved sub-block inside
            // `best_fit`'s extent and no reference to it exists yet.
            let next_block_ref = unsafe { &mut *next_block };
            // SAFETY: `best_fit` is the chosen free block and this call owns it
            // under `HEAP_LOCK`; the borrow ends at the end of this block, before
            // the tail canary below.
            let best_fit_ref = unsafe { &mut *best_fit };
            next_block_ref.magic = HEAP_MAGIC;
            next_block_ref.size = best_fit_ref.size - size - hdr_size;
            next_block_ref.is_free = 1;
            next_block_ref.next = best_fit_ref.next;
            best_fit_ref.size = size;
            best_fit_ref.next = next_block;
        }
        // SAFETY: marking the chosen block in-use, under `HEAP_LOCK`.
        unsafe { (*best_fit).is_free = 0; }
        lock_release(HEAP_LOCK.as_ptr());
        // SAFETY: `user_ptr` is `best_fit + hdr_size`, still inside the block whose
        // size was validated above (every block is at least `hdr_size + 4` bytes).
        let user_ptr = unsafe { (best_fit as *mut u8).add(hdr_size as usize) };
        // SAFETY: `(*best_fit).size` of the block this call owns under `HEAP_LOCK`.
        let block_size = unsafe { (*best_fit).size };
        // SAFETY: the tail canary lives at `user_ptr + block_size - 4`, i.e. 4 bytes
        // before the end of the block whose size was reserved by `kmalloc`.
        let tail = unsafe { user_ptr.add(block_size as usize - 4) as *mut u32 };
        // SAFETY: writing the tail canary into the reserved slot computed above.
        unsafe { *tail = HEAP_TAIL_MAGIC; }
        return user_ptr;
    }

    lock_release(HEAP_LOCK.as_ptr());
    klog_msg(LOG_WARN, c"heap out of memory".as_ptr() as *const u8);
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
    // SAFETY: `kmalloc_aligned` reserved four extra bytes below `aligned` for
    // exactly this back-pointer; `aligned` is inside the block just returned by
    // `kmalloc`, which lives in the identity-mapped heap window.
    unsafe { *((aligned - 4) as *mut u32) = raw as u32; }
    aligned as *mut u8
}

/// # Safety
///
/// `ptr` must be null or a pointer previously returned by `kmalloc` /
/// `kmalloc_aligned` (or a C caller honouring the same heap contract) that has
/// not already been freed, and it must stay valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kfree(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    let hdr_size = core::mem::size_of::<HeapBlock>() as usize;

    // Normal heap block?
    // SAFETY: `kfree` may only be called with a pointer from `kmalloc`, so
    // stepping back by the header size stays inside the same heap block; the
    // result is range-checked before any dereference.
    let block = unsafe { ptr.sub(hdr_size) } as *mut HeapBlock;
    // SAFETY: `block` is inside the heap window and correctly aligned (checked
    // by `heap_addr_in_range` before the dereference).
    if heap_addr_in_range(block as u32) && unsafe { (*block).magic } == HEAP_MAGIC {
        kfree_impl(ptr);
        return;
    }

    // Linux-style fallback: kmalloc_aligned stores the raw heap pointer
    // 4 bytes below the aligned address.
    // SAFETY: aligned-allocation fallback — `ptr - 4` holds the raw heap pointer
    // `kmalloc_aligned` stored at allocation time; it is read before any check
    // but the address is validated immediately afterwards.
    let raw = unsafe { *((ptr as u32 - 4) as *const u32) } as *mut u8;
    if heap_addr_in_range(raw as u32) {
        // SAFETY: `raw` is only dereferenced after `heap_addr_in_range` proved it
        // lies in the heap window, so stepping back by the header size stays within
        // that window.
        let rblock = unsafe { raw.sub(hdr_size) } as *mut HeapBlock;
        // SAFETY: `rblock` is inside the heap window, verified by the check above.
        if unsafe { (*rblock).magic } == HEAP_MAGIC {
            kfree_impl(raw);
        }
    }
}

fn kfree_impl(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    let hdr_size = core::mem::size_of::<HeapBlock>() as usize;
    lock_acquire(HEAP_LOCK.as_ptr());

    // SAFETY: `kfree_impl` is called from `kfree` (or the slab layer) with a
    // pointer returned by `kmalloc`; the header step-back stays in the same block
    // and `HEAP_LOCK` is already held, so the list cannot move under us.
    let block = unsafe { ptr.sub(hdr_size) } as *mut HeapBlock;
    if !heap_addr_in_range(block as u32) {
        klog_msg(LOG_WARN, c"kfree pointer outside heap range".as_ptr() as *const u8);
        lock_release(HEAP_LOCK.as_ptr());
        return;
    }

    // Accept free() only for blocks that are still linked in the heap list.
    // SAFETY: `HEAP_START_PTR` read under `HEAP_LOCK`.
    let mut walk = *unsafe { KStatic::get_mut(HEAP_START_PTR.as_ptr()) };
    let mut found = false;
    while !walk.is_null() {
        if !heap_addr_in_range(walk as u32) {
            kprint_str(c"[FATAL] kfree: walk pointer left heap range\n".as_ptr() as *const u8);
            klog_msg(LOG_FAIL, c"heap list pointer out of range".as_ptr() as *const u8);
            lock_release(HEAP_LOCK.as_ptr());
            return;
        }
        // SAFETY: `walk` is a node of the locked heap list, range-checked at the top
        // of every iteration before the dereference.
        let walk_magic = unsafe { (*walk).magic };
        if walk_magic != HEAP_MAGIC {
            kprint_str(c"[FATAL] kfree: list magic corrupted during lookup\n".as_ptr() as *const u8);
            klog_msg(LOG_FAIL, c"heap list magic corrupted".as_ptr() as *const u8);
            lock_release(HEAP_LOCK.as_ptr());
            return;
        }
        if walk == block {
            found = true;
            break;
        }
        // SAFETY: `(*walk).next` of a node proven to be in the heap range.
        walk = unsafe { (*walk).next };
    }
    if !found {
        klog_msg(LOG_WARN, c"kfree ignored for unknown block".as_ptr() as *const u8);
        lock_release(HEAP_LOCK.as_ptr());
        return;
    }

    // SAFETY: `block` was just found to be a linked node of the heap list, so it
    // is a live `HeapBlock`; still under `HEAP_LOCK`.
    let magic = unsafe { (*block).magic };
    if magic != HEAP_MAGIC {
        klog_msg(LOG_WARN, c"kfree bad block magic".as_ptr() as *const u8);
        lock_release(HEAP_LOCK.as_ptr());
        return;
    }

    // SAFETY: `(*block).size` of the validated heap block.
    let bsize = unsafe { (*block).size } as usize;
    if bsize < 4 {
        kprint_str(c"[FATAL] kfree: block too small for tail canary\n".as_ptr() as *const u8);
        klog_msg(LOG_FAIL, c"heap block too small for canary".as_ptr() as *const u8);
        lock_release(HEAP_LOCK.as_ptr());
        return;
    }
    // SAFETY: `bsize >= 4` is checked above and the block was validated as a live
    // heap block, so `ptr + bsize - 4` is the tail canary slot.
    let tail = unsafe { ptr.add(bsize - 4) as *mut u32 };
    // SAFETY: reading the tail canary slot computed above, inside the block.
    let tail_magic = unsafe { *tail };
    if tail_magic != HEAP_TAIL_MAGIC {
        kprint_str(c"[FATAL] kfree: tail canary corrupted (buffer overflow)\n".as_ptr() as *const u8);
        klog_msg(LOG_FAIL, c"heap tail canary corrupted".as_ptr() as *const u8);
        lock_release(HEAP_LOCK.as_ptr());
        return;
    }

    // SAFETY: `(*block).is_free` of the validated heap block, under `HEAP_LOCK`.
    if unsafe { (*block).is_free } != 0 {
        klog_msg(LOG_WARN, c"double free ignored".as_ptr() as *const u8);
        lock_release(HEAP_LOCK.as_ptr());
        return;
    }

    // SAFETY: releasing `block` back to the heap list, under `HEAP_LOCK`.
    unsafe { (*block).is_free = 1; }

    // SAFETY: `HEAP_START_PTR` re-read under `HEAP_LOCK` for the coalescing walk.
    let mut curr = *unsafe { KStatic::get_mut(HEAP_START_PTR.as_ptr()) };
    while !curr.is_null() {
        // SAFETY: `curr` walks the locked heap list; each node is range/ magic checked
        // before use.
        let curr_magic = unsafe { (*curr).magic };
        if curr_magic != HEAP_MAGIC {
            kprint_str(c"[FATAL] kfree: heap corruption at curr\n".as_ptr() as *const u8);
            klog_msg(LOG_FAIL, c"heap corruption during coalesce".as_ptr() as *const u8);
            break;
        }
        // SAFETY: `(*curr).is_free` of the validated list node.
        let curr_free = unsafe { (*curr).is_free };
        // SAFETY: `(*curr).next` of the validated list node.
        let next = unsafe { (*curr).next };
        if curr_free != 0 && !next.is_null() {
            // SAFETY: `(*next).magic` of a node checked non-null and in heap range.
            let next_magic = unsafe { (*next).magic };
            if next_magic != HEAP_MAGIC {
                kprint_str(c"[FATAL] kfree: next header corrupted, stopping coalesce\n".as_ptr() as *const u8);
                klog_msg(LOG_FAIL, c"next heap header corrupted".as_ptr() as *const u8);
                break;
            }
            // SAFETY: `(*next).is_free` of the magic-checked node.
            let next_free = unsafe { (*next).is_free };
            if next_free != 0 {
                // SAFETY: `(*next).size` of the magic-checked node.
                let next_size = unsafe { (*next).size };
                // SAFETY: `(*next).next` of the magic-checked node.
                let next_next = unsafe { (*next).next };
                {
                    // SAFETY: `next` is the adjacent node being absorbed, and this
                    // call owns the locked heap list.
                    let next_ref = unsafe { &mut *next };
                    // SAFETY: `curr` is the node being widened; it is distinct from
                    // `next` (adjacent list nodes) and this call owns it under
                    // `HEAP_LOCK`.
                    let curr_ref = unsafe { &mut *curr };
                    next_ref.magic = 0;
                    curr_ref.size += next_size + hdr_size as u32;
                    curr_ref.next = next_next;
                }
                continue;
            }
        }
        curr = next;
    }
    lock_release(HEAP_LOCK.as_ptr());
}

#[unsafe(no_mangle)]
pub extern "C" fn get_free_heap_memory() -> u32 {
    let mut free_mem: u32 = 0;
    lock_acquire(HEAP_LOCK.as_ptr());
    // SAFETY: `HEAP_START_PTR` read under `HEAP_LOCK`.
    let mut current = *unsafe { KStatic::get_mut(HEAP_START_PTR.as_ptr()) };
    while !current.is_null() {
        // SAFETY: `(*current).is_free` of a heap-list node, under `HEAP_LOCK`.
        let is_free = unsafe { (*current).is_free };
        // SAFETY: `(*current).size` of the same node.
        let size = unsafe { (*current).size };
        if is_free != 0 {
            free_mem += size;
        }
        // SAFETY: following `(*current).next` in the locked heap list.
        current = unsafe { (*current).next };
    }
    lock_release(HEAP_LOCK.as_ptr());
    free_mem
}