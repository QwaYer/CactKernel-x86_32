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
    // SAFETY: `SHM_INITIALIZED` boot/first-use latch; a racing read can at worst run
    // the idempotent init twice.
    if *unsafe { KStatic::get_mut(SHM_INITIALIZED.as_ptr()) } != 0 {
        return;
    }
    // SAFETY: boot-time init.
    unsafe { irq_spinlock_init(SHM_LOCK.as_ptr()) };
    // SAFETY: `SHM_TABLE` reset during single-threaded first-use init.
    let table = unsafe { KStatic::get_mut(SHM_TABLE.as_ptr()) };
    for e in table.iter_mut() {
        e.valid = 0;
        e.nattch = 0;
        e.destroy = 0;
    }
    // SAFETY: latch set last, after `SHM_LOCK` and the table are initialised.
    *unsafe { KStatic::get_mut(SHM_INITIALIZED.as_ptr()) } = 1;
}

pub(crate) fn seg_valid(id: i32) -> bool {
    if id < 1 || id > SHM_MAX_SEGMENTS as i32 {
        return false;
    }
    // SAFETY: `SHM_TABLE[id-1].valid` read; all `seg_valid` callers hold `SHM_LOCK`.
    (unsafe { KStatic::get_mut(SHM_TABLE.as_ptr()) })[(id - 1) as usize].valid != 0
}

pub(crate) fn seg_free(s: *mut ShmSeg) {
    // SAFETY: `s` is a valid `ShmSeg` pointer (per the caller contract); this
    // borrow is exclusive for the whole teardown, and `free_page` only touches the
    // frame it is handed.
    let s = unsafe { &mut *s };
    let n = s.num_pages as usize;
    for slot in s.pages.iter_mut().take(n) {
        if !slot.is_null() {
            // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
            unsafe { free_page(*slot) };
            *slot = core::ptr::null_mut();
        }
    }
    s.valid = 0;
    s.nattch = 0;
}

pub(crate) fn find_shm_va(num_pages: u32) -> u32 {
    let size = num_pages * PAGE_SIZE;
    let mut candidate = SHM_VA_BASE;

    // SAFETY: `current_task` is a valid kernel global.
    let t = unsafe { *current_task.get() };
    if t.is_null() {
        return 0;
    }
    // SAFETY: `t` is the live current task (non-null, checked above), so this
    // `proc` field read is in bounds.
    let proc_meta = unsafe { (*t).proc };
    if proc_meta.is_null() {
        return 0;
    }

    while candidate + size <= SHM_VA_LIMIT {
        let mut clash = false;
        for i in 0..TASK_SHM_MAX {
            // SAFETY: `proc_meta` is the live `ProcMeta` and `i < TASK_SHM_MAX`, so
            // this attachment field read is in bounds.
            let id = unsafe { (*proc_meta).shm_attachments[i].shm_id };
            if id == 0 || !seg_valid(id) {
                continue;
            }
            // SAFETY: `SHM_TABLE[id-1]` read; `seg_valid(id)` passed and the caller holds
            // `SHM_LOCK`.
            let seg = &(unsafe { KStatic::get_mut(SHM_TABLE.as_ptr()) })[(id - 1) as usize];
            // SAFETY: as above, for the attachment's virtual address.
            let base = unsafe { (*proc_meta).shm_attachments[i].shm_vaddr };
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
        // SAFETY: `pd` is valid and `pdi` is masked to 10 bits, so this PD entry
        // pointer is in bounds.
        let pde_entry = unsafe { pd.add(pdi as usize) };
        // SAFETY: `pde_entry` points at one initialised PD entry.
        let pde = unsafe { *pde_entry };
        if pde & PAGE_PRESENT == 0 {
            continue;
        }
        let pt = (pde & !0xFFF) as *mut u32;
        let pti = (addr >> 12) & 0x3FF;
        // SAFETY: `pt` is the live page table named by the present PDE and `pti` is
        // masked to 10 bits, so this entry pointer is in bounds.
        let pte_entry = unsafe { pt.add(pti as usize) };
        // SAFETY: `pte_entry` points at one PTE, which this unmap clears.
        unsafe { *pte_entry = 0 };
        flush_tlb(addr);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn shm_get(key: i32, size: u32, flags: i32) -> i32 {
    shm_ensure_init();
    lock_acquire(SHM_LOCK.as_ptr());

    // SAFETY: `SHM_TABLE` mutated during the whole of `shm_get`, under `SHM_LOCK`.
    let table = unsafe { KStatic::get_mut(SHM_TABLE.as_ptr()) };

    if key != IPC_PRIVATE {
        for (i, e) in table.iter().enumerate() {
            if e.valid == 0 || e.key != key {
                continue;
            }
            if (flags & IPC_CREAT != 0) && (flags & IPC_EXCL != 0) {
                lock_release(SHM_LOCK.as_ptr());
                return -1;
            }
            let id = i as i32 + 1;
            lock_release(SHM_LOCK.as_ptr());
            return id;
        }
        if flags & IPC_CREAT == 0 {
            lock_release(SHM_LOCK.as_ptr());
            return -1;
        }
    }

    if size == 0 {
        lock_release(SHM_LOCK.as_ptr());
        return -1;
    }

    let slot: i32 = table
        .iter()
        .position(|e| e.valid == 0)
        .map(|i| i as i32)
        .unwrap_or(-1);
    if slot < 0 {
        lock_release(SHM_LOCK.as_ptr());
        return -1;
    }

    let npages = size.div_ceil(PAGE_SIZE);
    if npages > SHM_MAX_PAGES as u32 {
        lock_release(SHM_LOCK.as_ptr());
        return -1;
    }

    let s = &mut table[slot as usize];
    for i in 0..npages as usize {
        let p = kalloc();
        if p.is_null() {
            for slot in s.pages.iter_mut().take(i) {
                // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
                unsafe { free_page(*slot) };
                *slot = core::ptr::null_mut();
            }
            lock_release(SHM_LOCK.as_ptr());
            return -1;
        }
        zero_page(p);
        s.pages[i] = p;
    }

    // SAFETY: `current_task` is a valid kernel global; it may be null, which the
    // test below handles.
    let cur_task = unsafe { *current_task.get() };
    let cur_pid = if !cur_task.is_null() {
        // SAFETY: `cur_task` is the live current task (non-null, checked above), so
        // this `pid` field read is in bounds.
        unsafe { (*cur_task).pid }
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

    lock_release(SHM_LOCK.as_ptr());
    slot + 1
}

#[path = "shm_api.rs"]
mod shm_api;
pub use shm_api::*;
