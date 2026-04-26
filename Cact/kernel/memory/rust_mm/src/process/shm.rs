use crate::ffi::*;
use crate::pmm::{kalloc, kfree_page};
use crate::vmm::paging::vmm_map;
use crate::safe::{KStatic, lock_acquire, lock_release, zero_page, flush_tlb};

#[repr(C)]
struct ShmSeg {
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

static SHM_TABLE: KStatic<[ShmSeg; SHM_MAX_SEGMENTS]> = KStatic::new([SHM_SEG_EMPTY; SHM_MAX_SEGMENTS]);
static SHM_LOCK: KStatic<IrqSpinlock> = KStatic::new(IrqSpinlock { spin_locked: 0, saved_flags: 0 });
static SHM_INITIALIZED: KStatic<i32> = KStatic::new(0);

fn shm_ensure_init() {
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

fn seg_valid(id: i32) -> bool {
    if id < 1 || id > SHM_MAX_SEGMENTS as i32 {
        return false;
    }
    SHM_TABLE.get_mut()[(id - 1) as usize].valid != 0
}

fn seg_free(s: *mut ShmSeg) {
    // SAFETY: s is a valid ShmSeg pointer.
    unsafe {
        for i in 0..(*s).num_pages as usize {
            if !(*s).pages[i].is_null() {
                kfree_page((*s).pages[i]);
                (*s).pages[i] = core::ptr::null_mut();
            }
        }
        (*s).valid = 0;
        (*s).nattch = 0;
    }
}

fn find_shm_va(num_pages: u32) -> u32 {
    let size = num_pages * PAGE_SIZE;
    let mut candidate = SHM_VA_BASE;

    // SAFETY: current_task is a valid kernel global.
    let t = unsafe { current_task };
    if t.is_null() {
        return 0;
    }

    while candidate + size <= SHM_VA_LIMIT {
        let mut clash = false;
        // SAFETY: t is valid.
        for i in 0..TASK_SHM_MAX {
            let id = unsafe { (*t).shm_attachments[i].shm_id };
            if id == 0 || !seg_valid(id) {
                continue;
            }
            let seg = &SHM_TABLE.get_mut()[(id - 1) as usize];
            let base = unsafe { (*t).shm_attachments[i].shm_vaddr };
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

fn shm_unmap_from(pd: *mut u32, va: u32, num_pages: u32) {
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

//public api
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
                kfree_page(s.pages[j]);
                s.pages[j] = core::ptr::null_mut();
            }
            lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
            return -1;
        }
        zero_page(p);
        s.pages[i] = p;
    }

    let cur_pid = if !unsafe { current_task }.is_null() {
        unsafe { (*current_task).pid }
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

//public api
#[unsafe(no_mangle)]
pub extern "C" fn shm_at(shmid: i32, shmaddr: u32, flags: i32) -> u32 {
    shm_ensure_init();

    // SAFETY: current_task is a valid kernel global.
    let t = unsafe { current_task };
    if t.is_null() || unsafe { (*t).is_kernel } != 0 {
        return u32::MAX;
    }

    lock_acquire(SHM_LOCK.as_ptr() as *mut IrqSpinlock);

    if !seg_valid(shmid) {
        lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
        return u32::MAX;
    }

    let mut slot: i32 = -1;
    // SAFETY: t is valid.
    for i in 0..TASK_SHM_MAX {
        if unsafe { (*t).shm_attachments[i].shm_id } == 0 {
            slot = i as i32;
            break;
        }
    }
    if slot < 0 {
        lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
        return u32::MAX;
    }

    let seg = &mut SHM_TABLE.get_mut()[(shmid - 1) as usize];

    let va;
    if shmaddr != 0 {
        let mut addr = shmaddr;
        if flags & SHM_RND != 0 {
            addr &= !(PAGE_SIZE - 1);
        }
        if addr % PAGE_SIZE != 0 {
            lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
            return u32::MAX;
        }
        va = addr;
    } else {
        va = find_shm_va(seg.num_pages);
        if va == 0 {
            lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
            return u32::MAX;
        }
    }

    let mut page_flags = (PAGE_PRESENT | PAGE_USER) as i32;
    if flags & SHM_RDONLY == 0 {
        page_flags |= PAGE_RW as i32;
    }

    for i in 0..seg.num_pages {
        vmm_map(
            unsafe { (*t).page_directory },
            va + i * PAGE_SIZE,
            seg.pages[i as usize] as u32,
            page_flags,
        );
    }

    // SAFETY: t is valid.
    unsafe {
        (*t).shm_attachments[slot as usize].shm_id = shmid;
        (*t).shm_attachments[slot as usize].shm_vaddr = va;
    }
    seg.nattch += 1;
    seg.lpid = unsafe { (*t).pid };

    lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
    va
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn shm_dt(shmaddr: u32) -> i32 {
    shm_ensure_init();

    // SAFETY: current_task is a valid kernel global.
    let t = unsafe { current_task };
    if t.is_null() || unsafe { (*t).is_kernel } != 0 {
        return -1;
    }

    lock_acquire(SHM_LOCK.as_ptr() as *mut IrqSpinlock);

    let mut slot: i32 = -1;
    // SAFETY: t is valid.
    for i in 0..TASK_SHM_MAX {
        let att = unsafe { &(*t).shm_attachments[i] };
        if att.shm_vaddr == shmaddr && att.shm_id != 0 {
            slot = i as i32;
            break;
        }
    }
    if slot < 0 {
        lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }

    // SAFETY: t is valid.
    let id = unsafe { (*t).shm_attachments[slot as usize].shm_id };
    if !seg_valid(id) {
        unsafe {
            (*t).shm_attachments[slot as usize].shm_id = 0;
            (*t).shm_attachments[slot as usize].shm_vaddr = 0;
        }
        lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }

    let seg = &mut SHM_TABLE.get_mut()[(id - 1) as usize];
    shm_unmap_from(unsafe { (*t).page_directory }, shmaddr, seg.num_pages);

    unsafe {
        (*t).shm_attachments[slot as usize].shm_id = 0;
        (*t).shm_attachments[slot as usize].shm_vaddr = 0;
    }
    if seg.nattch > 0 {
        seg.nattch -= 1;
    }
    seg.lpid = unsafe { (*t).pid };

    if seg.destroy != 0 && seg.nattch == 0 {
        seg_free(seg as *mut ShmSeg);
    }

    lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
    0
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn shm_ctl(shmid: i32, cmd: i32, buf: *mut u8) -> i32 {
    shm_ensure_init();
    lock_acquire(SHM_LOCK.as_ptr() as *mut IrqSpinlock);

    if !seg_valid(shmid) {
        lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }

    let seg = &mut SHM_TABLE.get_mut()[(shmid - 1) as usize];

    if cmd == IPC_RMID {
        seg.destroy = 1;
        if seg.nattch == 0 {
            seg_free(seg as *mut ShmSeg);
        }
        lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
        return 0;
    }

    if cmd == IPC_STAT {
        if buf.is_null() {
            lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
            return -1;
        }
        // SAFETY: buf is a valid ShmInfo buffer provided by the caller.
        let info = buf as *mut ShmInfo;
        unsafe {
            (*info).shm_segsz = seg.size;
            (*info).shm_cpid = seg.cpid;
            (*info).shm_lpid = seg.lpid;
            (*info).shm_nattch = seg.nattch;
        }
        lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
        return 0;
    }

    lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
    -1
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn shm_detach_all(pid: u32, page_directory: *mut u32) {
    shm_ensure_init();
    if page_directory.is_null() {
        return;
    }

    // SAFETY: task_list_head is a valid kernel global.
    let mut found: *mut TaskStruct = core::ptr::null_mut();
    let mut cur = unsafe { task_list_head };
    if cur.is_null() {
        return;
    }
    loop {
        // SAFETY: walking the circular task list.
        if unsafe { (*cur).pid } == pid {
            found = cur;
            break;
        }
        cur = unsafe { (*cur).next };
        if cur.is_null() || cur == unsafe { task_list_head } {
            break;
        }
    }

    if found.is_null() {
        return;
    }

    lock_acquire(SHM_LOCK.as_ptr() as *mut IrqSpinlock);

    // SAFETY: found is a valid TaskStruct.
    for i in 0..TASK_SHM_MAX {
        let id = unsafe { (*found).shm_attachments[i].shm_id };
        if id == 0 {
            continue;
        }
        if !seg_valid(id) {
            unsafe {
                (*found).shm_attachments[i].shm_id = 0;
                (*found).shm_attachments[i].shm_vaddr = 0;
            }
            continue;
        }
        let seg = &mut SHM_TABLE.get_mut()[(id - 1) as usize];
        shm_unmap_from(
            page_directory,
            unsafe { (*found).shm_attachments[i].shm_vaddr },
            seg.num_pages,
        );
        unsafe {
            (*found).shm_attachments[i].shm_id = 0;
            (*found).shm_attachments[i].shm_vaddr = 0;
        }
        if seg.nattch > 0 {
            seg.nattch -= 1;
        }
        seg.lpid = pid;
        if seg.destroy != 0 && seg.nattch == 0 {
            seg_free(seg as *mut ShmSeg);
        }
    }

    lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
}
