//! Fixed-size shared-memory segment table: attach/detach, refcount, and page mapping into a PD.

use crate::ffi::*;
use crate::pmm::{kalloc, free_page};
use crate::safe::{KStatic, lock_acquire, lock_release, zero_page, flush_tlb};

#[repr(C)]
pub(crate) struct ShmSeg {
    key: i32,
    perms: i32,
    size: u32,
    num_pages: u32,
    pages: [*mut u8; SHM_MAX_PAGES],
    nattch: u32,
    cpid: u32,
    lpid: u32,
    valid: i32,
    destroy: i32,
}

const SHM_SEG_EMPTY: ShmSeg = ShmSeg {
    key: 0,
    perms: 0,
    size: 0,
    num_pages: 0,
    pages: [core::ptr::null_mut(); SHM_MAX_PAGES],
    nattch: 0,
    cpid: 0,
    lpid: 0,
    valid: 0,
    destroy: 0,
};

pub(crate) static SHM_TABLE: KStatic<[ShmSeg; SHM_MAX_SEGMENTS]> = KStatic::new([SHM_SEG_EMPTY; SHM_MAX_SEGMENTS]);
pub(crate) static SHM_LOCK: KStatic<IrqSpinlock> = KStatic::new(IrqSpinlock { spin_locked: 0, saved_flags: 0 });
pub(crate) static SHM_INITIALIZED: KStatic<i32> = KStatic::new(0);

pub(crate) fn shm_ensure_init() {
    if *SHM_INITIALIZED.get_mut() != 0 {
        return;
    }
    // SAFETY: boot-time init.
    unsafe { irq_spinlock_init(SHM_LOCK.as_ptr() as *mut IrqSpinlock) };
    let table = SHM_TABLE.get_mut();
    for i in 0..SHM_MAX_SEGMENTS {
        table[i].valid = 0;
        table[i].nattch = 0;
        table[i].destroy = 0;
    }
    *SHM_INITIALIZED.get_mut() = 1;
}

pub(crate) fn seg_valid(id: i32) -> bool {
    if id < 1 || id > SHM_MAX_SEGMENTS as i32 {
        return false;
    }
    SHM_TABLE.get_mut()[(id - 1) as usize].valid != 0
}

pub(crate) fn seg_free(s: *mut ShmSeg) {
    // SAFETY: s is a valid ShmSeg pointer.
    unsafe {
        for i in 0..(*s).num_pages as usize {
            if !(*s).pages[i].is_null() {
                free_page((*s).pages[i]);
                (*s).pages[i] = core::ptr::null_mut();
            }
        }
        (*s).valid = 0;
        (*s).nattch = 0;
    }
}

pub(crate) fn find_shm_va(num_pages: u32) -> u32 {
    let size = num_pages * PAGE_SIZE;
    let mut candidate = SHM_VA_BASE;

    // SAFETY: current_task is a valid kernel global.
    let t = unsafe { *current_task.get() };
    if t.is_null() || unsafe { (*t).proc.is_null() } {
        return 0;
    }

    while candidate + size <= SHM_VA_LIMIT {
        let mut clash = false;
        // SAFETY: t is valid.
        for i in 0..TASK_SHM_MAX {
            let id = unsafe { (*(*t).proc).shm_attachments[i].shm_id };
            if id == 0 || !seg_valid(id) {
                continue;
            }
            let seg = &SHM_TABLE.get_mut()[(id - 1) as usize];
            let base = unsafe { (*(*t).proc).shm_attachments[i].shm_vaddr };
            let end = base + seg.num_pages * PAGE_SIZE;
            let cend = candidate + size;
            if candidate < end && cend > base {
                candidate = (end + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
                clash = true;
                break;
            }
        }
        if !clash {
            return candidate;
        }
    }
    0
}

pub(crate) fn shm_unmap_from(pd: *mut u32, va: u32, num_pages: u32) {
    for i in 0..num_pages {
        let addr = va + i * PAGE_SIZE;
        let pdi = (addr >> 22) & 0x3FF;
        // SAFETY: pd is valid.
        let pde = unsafe { *pd.add(pdi as usize) };
        if pde & PAGE_PRESENT == 0 {
            continue;
        }
        let pt = (pde & !0xFFF) as *mut u32;
        let pti = (addr >> 12) & 0x3FF;
        // SAFETY: pt is valid.
        unsafe { *pt.add(pti as usize) = 0; }
        flush_tlb(addr);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn shm_get(key: i32, size: u32, flags: i32) -> i32 {
    shm_ensure_init();
    lock_acquire(SHM_LOCK.as_ptr() as *mut IrqSpinlock);

    let table = SHM_TABLE.get_mut();

    if key != IPC_PRIVATE {
        for i in 0..SHM_MAX_SEGMENTS {
            if table[i].valid == 0 || table[i].key != key {
                continue;
            }
            if (flags & IPC_CREAT != 0) && (flags & IPC_EXCL != 0) {
                lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
                return -1;
            }
            let id = i as i32 + 1;
            lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
            return id;
        }
        if flags & IPC_CREAT == 0 {
            lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
            return -1;
        }
    }

    if size == 0 {
        lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }

    let mut slot: i32 = -1;
    for i in 0..SHM_MAX_SEGMENTS {
        if table[i].valid == 0 {
            slot = i as i32;
            break;
        }
    }
    if slot < 0 {
        lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }

    let npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if npages > SHM_MAX_PAGES as u32 {
        lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }

    let s = &mut table[slot as usize];
    for i in 0..npages as usize {
        let p = kalloc();
        if p.is_null() {
            for j in 0..i {
                free_page(s.pages[j]);
                s.pages[j] = core::ptr::null_mut();
            }
            lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
            return -1;
        }
        zero_page(p);
        s.pages[i] = p;
    }

    let cur_pid = if !unsafe { *current_task.get() }.is_null() {
        unsafe { (*(*current_task.get())).pid }
    } else {
        0
    };

    s.key = key;
    s.perms = flags & 0o777;
    s.size = size;
    s.num_pages = npages;
    s.nattch = 0;
    s.cpid = cur_pid;
    s.lpid = 0;
    s.valid = 1;
    s.destroy = 0;

    lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
    slot + 1
}

#[path = "shm_api.rs"]
mod shm_api;
pub use shm_api::*;
