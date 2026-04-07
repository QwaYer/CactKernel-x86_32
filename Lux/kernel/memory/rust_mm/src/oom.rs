use crate::ffi::*;

static mut G_STATS: OomStats = OomStats {
    oom_kills: 0,
    pages_reclaimed: 0,
    last_killed_pid: 0,
};

unsafe fn oom_score(t: *mut TaskStruct) -> u32 {
    if t.is_null() {
        return 0;
    }
    if (*t).is_kernel != 0 {
        return 0;
    }
    if (*t).pid <= 1 {
        return 0;
    }
    if (*t).state == TASK_ZOMBIE {
        return 0;
    }
    (*t).mm.count
}

pub unsafe fn oom_kill_internal() -> i32 {
    oom_kill()
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn oom_kill() -> i32 {
    irq_spinlock_acquire(&raw mut scheduler_lock);

    if task_list_head.is_null() {
        irq_spinlock_release(&raw mut scheduler_lock);
        return -1;
    }

    let mut victim: *mut TaskStruct = core::ptr::null_mut();
    let mut best_score: u32 = 0;

    let mut t = task_list_head;
    let mut count = 0;
    loop {
        let score = oom_score(t);
        if score > best_score {
            best_score = score;
            victim = t;
        }
        t = (*t).next;
        count += 1;
        if t == task_list_head || count >= 256 {
            break;
        }
    }

    if victim.is_null() || best_score == 0 {
        irq_spinlock_release(&raw mut scheduler_lock);
        kprint_color(b"[OOM] no killable process found\n\0".as_ptr(), COLOR_LIGHT_RED);
        return -1;
    }

    let victim_pid = (*victim).pid;
    let victim_pages = (*victim).mm.count;

    (*victim).pending_signals |= SIGKILL;

    if (*victim).state == TASK_SLEEPING {
        extern "C" {
            static mut sleep_queue: SchedQueue;
        }
        sched_queue_remove(&raw mut sleep_queue, victim);
        (*victim).state = TASK_READY;
        sched_queue_push(&raw mut ready_queue, victim);
    }
    if (*victim).state == TASK_WAITING {
        extern "C" {
            static mut wait_queue: SchedQueue;
        }
        sched_queue_remove(&raw mut wait_queue, victim);
        (*victim).state = TASK_READY;
        sched_queue_push(&raw mut ready_queue, victim);
    }

    irq_spinlock_release(&raw mut scheduler_lock);

    G_STATS.oom_kills += 1;
    G_STATS.pages_reclaimed += victim_pages;
    G_STATS.last_killed_pid = victim_pid;

    let mut buf = [0u8; 16];
    kprint_color(b"\n[OOM] Killed pid=\0".as_ptr(), COLOR_LIGHT_RED);
    itoa(victim_pid as i32, buf.as_mut_ptr());
    kprint_color(buf.as_ptr(), COLOR_LIGHT_RED);
    kprint_color(b" (\0".as_ptr(), COLOR_LIGHT_RED);
    itoa(victim_pages as i32, buf.as_mut_ptr());
    kprint_color(buf.as_ptr(), COLOR_LIGHT_RED);
    kprint_color(b" pages)\n\0".as_ptr(), COLOR_LIGHT_RED);

    task_reap();
    0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn oom_get_stats() -> OomStats {
    G_STATS
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn oom_print_stats() {
    let mut buf = [0u8; 16];
    kprint(b"[OOM] === OOM Killer Statistics ===\n\0".as_ptr());
    kprint(b"  oom_kills:       \0".as_ptr());
    itoa(G_STATS.oom_kills as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"\n\0".as_ptr());
    kprint(b"  pages_reclaimed: \0".as_ptr());
    itoa(G_STATS.pages_reclaimed as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"\n\0".as_ptr());
    kprint(b"  last_killed_pid: \0".as_ptr());
    itoa(G_STATS.last_killed_pid as i32, buf.as_mut_ptr());
    kprint(buf.as_ptr());
    kprint(b"\n\0".as_ptr());
}