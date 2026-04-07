use crate::ffi::*;
use crate::pmm::{kalloc, kfree_page};
use crate::paging::vmm_map;

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

static mut SHM_TABLE: [ShmSeg; SHM_MAX_SEGMENTS] = {
    const EMPTY: ShmSeg = ShmSeg {
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
    [EMPTY; SHM_MAX_SEGMENTS]
};
static mut SHM_LOCK: IrqSpinlock = IrqSpinlock { spin_locked: 0, saved_flags: 0 };
static mut SHM_INITIALIZED: i32 = 0;

unsafe fn shm_ensure_init() {
    if SHM_INITIALIZED != 0 {
        return;
    }
    irq_spinlock_init(&raw mut SHM_LOCK);
    for i in 0..SHM_MAX_SEGMENTS {
        SHM_TABLE[i].valid = 0;
        SHM_TABLE[i].nattch = 0;
        SHM_TABLE[i].destroy = 0;
    }
    SHM_INITIALIZED = 1;
}

unsafe fn seg_valid(id: i32) -> bool {
    id >= 1 && id <= SHM_MAX_SEGMENTS as i32 && SHM_TABLE[(id - 1) as usize].valid != 0
}

unsafe fn seg_free(s: *mut ShmSeg) {
    for i in 0..(*s).num_pages as usize {
        if !(*s).pages[i].is_null() {
            kfree_page((*s).pages[i]);
            (*s).pages[i] = core::ptr::null_mut();
        }
    }
    (*s).valid = 0;
    (*s).nattch = 0;
}

unsafe fn find_shm_va(num_pages: u32) -> u32 {
    let size = num_pages * PAGE_SIZE;
    let mut candidate = SHM_VA_BASE;

    while candidate + size <= SHM_VA_LIMIT {
        let mut clash = false;
        for i in 0..TASK_SHM_MAX {
            let id = (*current_task).shm_attachments[i].shm_id;
            if id == 0 || !seg_valid(id) {
                continue;
            }
            let s = &SHM_TABLE[(id - 1) as usize];
            let base = (*current_task).shm_attachments[i].shm_vaddr;
            let end = base + s.num_pages * PAGE_SIZE;
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

unsafe fn shm_unmap_from(pd: *mut u32, va: u32, num_pages: u32) {
    for i in 0..num_pages {
        let addr = va + i * PAGE_SIZE;
        let pdi = (addr >> 22) & 0x3FF;
        if *pd.add(pdi as usize) & PAGE_PRESENT == 0 {
            continue;
        }
        let pt = (*pd.add(pdi as usize) & !0xFFF) as *mut u32;
        let pti = (addr >> 12) & 0x3FF;
        *pt.add(pti as usize) = 0;
        tlb_flush(addr);
    }
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn shm_get(key: i32, size: u32, flags: i32) -> i32 {
    shm_ensure_init();
    irq_spinlock_acquire(&raw mut SHM_LOCK);

    if key != IPC_PRIVATE {
        for i in 0..SHM_MAX_SEGMENTS {
            if SHM_TABLE[i].valid == 0 || SHM_TABLE[i].key != key {
                continue;
            }
            if (flags & IPC_CREAT != 0) && (flags & IPC_EXCL != 0) {
                irq_spinlock_release(&raw mut SHM_LOCK);
                return -1;
            }
            let id = i as i32 + 1;
            irq_spinlock_release(&raw mut SHM_LOCK);
            return id;
        }
        if flags & IPC_CREAT == 0 {
            irq_spinlock_release(&raw mut SHM_LOCK);
            return -1;
        }
    }

    if size == 0 {
        irq_spinlock_release(&raw mut SHM_LOCK);
        return -1;
    }

    let mut slot: i32 = -1;
    for i in 0..SHM_MAX_SEGMENTS {
        if SHM_TABLE[i].valid == 0 {
            slot = i as i32;
            break;
        }
    }
    if slot < 0 {
        irq_spinlock_release(&raw mut SHM_LOCK);
        return -1;
    }

    let npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if npages > SHM_MAX_PAGES as u32 {
        irq_spinlock_release(&raw mut SHM_LOCK);
        return -1;
    }

    let s = &mut SHM_TABLE[slot as usize];
    for i in 0..npages as usize {
        let p = kalloc();
        if p.is_null() {
            for j in 0..i {
                kfree_page(s.pages[j]);
                s.pages[j] = core::ptr::null_mut();
            }
            irq_spinlock_release(&raw mut SHM_LOCK);
            return -1;
        }
        core::ptr::write_bytes(p, 0, PAGE_SIZE as usize);
        s.pages[i] = p;
    }

    s.key = key;
    s.perms = flags & 0o777;
    s.size = size;
    s.num_pages = npages;
    s.nattch = 0;
    s.cpid = if !current_task.is_null() { (*current_task).pid } else { 0 };
    s.lpid = 0;
    s.valid = 1;
    s.destroy = 0;

    irq_spinlock_release(&raw mut SHM_LOCK);
    slot + 1
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn shm_at(shmid: i32, shmaddr: u32, flags: i32) -> u32 {
    shm_ensure_init();

    if current_task.is_null() || (*current_task).is_kernel != 0 {
        return u32::MAX;
    }

    irq_spinlock_acquire(&raw mut SHM_LOCK);

    if !seg_valid(shmid) {
        irq_spinlock_release(&raw mut SHM_LOCK);
        return u32::MAX;
    }

    let mut slot: i32 = -1;
    for i in 0..TASK_SHM_MAX {
        if (*current_task).shm_attachments[i].shm_id == 0 {
            slot = i as i32;
            break;
        }
    }
    if slot < 0 {
        irq_spinlock_release(&raw mut SHM_LOCK);
        return u32::MAX;
    }

    let seg = &mut SHM_TABLE[(shmid - 1) as usize];

    let va;
    if shmaddr != 0 {
        let mut addr = shmaddr;
        if flags & SHM_RND != 0 {
            addr &= !(PAGE_SIZE - 1);
        }
        if addr % PAGE_SIZE != 0 {
            irq_spinlock_release(&raw mut SHM_LOCK);
            return u32::MAX;
        }
        va = addr;
    } else {
        va = find_shm_va(seg.num_pages);
        if va == 0 {
            irq_spinlock_release(&raw mut SHM_LOCK);
            return u32::MAX;
        }
    }

    let mut page_flags = (PAGE_PRESENT | PAGE_USER) as i32;
    if flags & SHM_RDONLY == 0 {
        page_flags |= PAGE_RW as i32;
    }

    for i in 0..seg.num_pages {
        vmm_map(
            (*current_task).page_directory,
            va + i * PAGE_SIZE,
            seg.pages[i as usize] as u32,
            page_flags,
        );
    }

    (*current_task).shm_attachments[slot as usize].shm_id = shmid;
    (*current_task).shm_attachments[slot as usize].shm_vaddr = va;
    seg.nattch += 1;
    seg.lpid = (*current_task).pid;

    irq_spinlock_release(&raw mut SHM_LOCK);
    va
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn shm_dt(shmaddr: u32) -> i32 {
    shm_ensure_init();

    if current_task.is_null() || (*current_task).is_kernel != 0 {
        return -1;
    }

    irq_spinlock_acquire(&raw mut SHM_LOCK);

    let mut slot: i32 = -1;
    for i in 0..TASK_SHM_MAX {
        if (*current_task).shm_attachments[i].shm_vaddr == shmaddr
            && (*current_task).shm_attachments[i].shm_id != 0
        {
            slot = i as i32;
            break;
        }
    }
    if slot < 0 {
        irq_spinlock_release(&raw mut SHM_LOCK);
        return -1;
    }

    let id = (*current_task).shm_attachments[slot as usize].shm_id;
    if !seg_valid(id) {
        (*current_task).shm_attachments[slot as usize].shm_id = 0;
        (*current_task).shm_attachments[slot as usize].shm_vaddr = 0;
        irq_spinlock_release(&raw mut SHM_LOCK);
        return -1;
    }

    let seg = &mut SHM_TABLE[(id - 1) as usize];
    shm_unmap_from((*current_task).page_directory, shmaddr, seg.num_pages);

    (*current_task).shm_attachments[slot as usize].shm_id = 0;
    (*current_task).shm_attachments[slot as usize].shm_vaddr = 0;
    if seg.nattch > 0 {
        seg.nattch -= 1;
    }
    seg.lpid = (*current_task).pid;

    if seg.destroy != 0 && seg.nattch == 0 {
        seg_free(seg);
    }

    irq_spinlock_release(&raw mut SHM_LOCK);
    0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn shm_ctl(shmid: i32, cmd: i32, buf: *mut u8) -> i32 {
    shm_ensure_init();
    irq_spinlock_acquire(&raw mut SHM_LOCK);

    if !seg_valid(shmid) {
        irq_spinlock_release(&raw mut SHM_LOCK);
        return -1;
    }

    let seg = &mut SHM_TABLE[(shmid - 1) as usize];

    if cmd == IPC_RMID {
        seg.destroy = 1;
        if seg.nattch == 0 {
            seg_free(seg);
        }
        irq_spinlock_release(&raw mut SHM_LOCK);
        return 0;
    }

    if cmd == IPC_STAT {
        if buf.is_null() {
            irq_spinlock_release(&raw mut SHM_LOCK);
            return -1;
        }
        let info = buf as *mut ShmInfo;
        (*info).shm_segsz = seg.size;
        (*info).shm_cpid = seg.cpid;
        (*info).shm_lpid = seg.lpid;
        (*info).shm_nattch = seg.nattch;
        irq_spinlock_release(&raw mut SHM_LOCK);
        return 0;
    }

    irq_spinlock_release(&raw mut SHM_LOCK);
    -1
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn shm_detach_all(pid: u32, page_directory: *mut u32) {
    shm_ensure_init();
    if page_directory.is_null() {
        return;
    }

    let mut found: *mut TaskStruct = core::ptr::null_mut();
    let mut cur = task_list_head;
    if cur.is_null() {
        return;
    }
    loop {
        if (*cur).pid == pid {
            found = cur;
            break;
        }
        cur = (*cur).next;
        if cur.is_null() || cur == task_list_head {
            break;
        }
    }

    if found.is_null() {
        return;
    }

    irq_spinlock_acquire(&raw mut SHM_LOCK);

    for i in 0..TASK_SHM_MAX {
        let id = (*found).shm_attachments[i].shm_id;
        if id == 0 {
            continue;
        }
        if !seg_valid(id) {
            (*found).shm_attachments[i].shm_id = 0;
            (*found).shm_attachments[i].shm_vaddr = 0;
            continue;
        }
        let seg = &mut SHM_TABLE[(id - 1) as usize];
        shm_unmap_from(
            page_directory,
            (*found).shm_attachments[i].shm_vaddr,
            seg.num_pages,
        );
        (*found).shm_attachments[i].shm_id = 0;
        (*found).shm_attachments[i].shm_vaddr = 0;
        if seg.nattch > 0 {
            seg.nattch -= 1;
        }
        seg.lpid = pid;
        if seg.destroy != 0 && seg.nattch == 0 {
            seg_free(seg);
        }
    }

    irq_spinlock_release(&raw mut SHM_LOCK);
}