//! Shared-memory attach/detach/ctl syscall entry points. Split out of `shm.rs`.

use crate::ffi::*;
use crate::vmm::paging::vmm_map;
use crate::safe::{lock_acquire, lock_release};
use crate::process::shm::{
    find_shm_va, seg_free, seg_valid, shm_ensure_init, shm_unmap_from, ShmSeg, SHM_LOCK, SHM_TABLE,
};

#[unsafe(no_mangle)]
pub extern "C" fn shm_at(shmid: i32, shmaddr: u32, flags: i32) -> u32 {
    shm_ensure_init();

    // SAFETY: current_task is a valid kernel global.
    let t = unsafe { *current_task.get() };
    if t.is_null() || unsafe { (*t).is_kernel } != 0 || unsafe { (*t).proc.is_null() } {
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
        if unsafe { (*(*t).proc).shm_attachments[i].shm_id } == 0 {
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
        (*(*t).proc).shm_attachments[slot as usize].shm_id = shmid;
        (*(*t).proc).shm_attachments[slot as usize].shm_vaddr = va;
    }
    seg.nattch += 1;
    seg.lpid = unsafe { (*t).pid };

    lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
    va
}

#[unsafe(no_mangle)]
pub extern "C" fn shm_dt(shmaddr: u32) -> i32 {
    shm_ensure_init();

    // SAFETY: current_task is a valid kernel global.
    let t = unsafe { *current_task.get() };
    if t.is_null() || unsafe { (*t).is_kernel } != 0 || unsafe { (*t).proc.is_null() } {
        return -1;
    }

    lock_acquire(SHM_LOCK.as_ptr() as *mut IrqSpinlock);

    let mut slot: i32 = -1;
    // SAFETY: t is valid.
    for i in 0..TASK_SHM_MAX {
        let att = unsafe { &(*(*t).proc).shm_attachments[i] };
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
    let id = unsafe { (*(*t).proc).shm_attachments[slot as usize].shm_id };
    if !seg_valid(id) {
        unsafe {
            (*(*t).proc).shm_attachments[slot as usize].shm_id = 0;
            (*(*t).proc).shm_attachments[slot as usize].shm_vaddr = 0;
        }
        lock_release(SHM_LOCK.as_ptr() as *mut IrqSpinlock);
        return -1;
    }

    let seg = &mut SHM_TABLE.get_mut()[(id - 1) as usize];
    shm_unmap_from(unsafe { (*t).page_directory }, shmaddr, seg.num_pages);

    unsafe {
        (*(*t).proc).shm_attachments[slot as usize].shm_id = 0;
        (*(*t).proc).shm_attachments[slot as usize].shm_vaddr = 0;
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

#[unsafe(no_mangle)]
pub extern "C" fn shm_detach_all(pid: u32, page_directory: *mut u32) {
    shm_ensure_init();
    if page_directory.is_null() {
        return;
    }

    // SAFETY: task_list_head is a valid kernel global.
    let mut found: *mut TaskStruct = core::ptr::null_mut();
    let mut cur = unsafe { *task_list_head.get() };
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
        if cur.is_null() || cur == unsafe { *task_list_head.get() } {
            break;
        }
    }

    if found.is_null() {
        return;
    }

    lock_acquire(SHM_LOCK.as_ptr() as *mut IrqSpinlock);

    // SAFETY: found is a valid TaskStruct.
    for i in 0..TASK_SHM_MAX {
        let id = unsafe { (*(*found).proc).shm_attachments[i].shm_id };
        if id == 0 {
            continue;
        }
        if !seg_valid(id) {
            unsafe {
                (*(*found).proc).shm_attachments[i].shm_id = 0;
                (*(*found).proc).shm_attachments[i].shm_vaddr = 0;
            }
            continue;
        }
        let seg = &mut SHM_TABLE.get_mut()[(id - 1) as usize];
        shm_unmap_from(
            page_directory,
            unsafe { (*(*found).proc).shm_attachments[i].shm_vaddr },
            seg.num_pages,
        );
        unsafe {
            (*(*found).proc).shm_attachments[i].shm_id = 0;
            (*(*found).proc).shm_attachments[i].shm_vaddr = 0;
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
