use crate::ffi::*;
use crate::safe::{KStatic, lock_acquire, lock_release, kprint_str, kprint_int};

static G_STATS: KStatic<OomStats> = KStatic::new(OomStats {
    oom_kills: 0,
    pages_reclaimed: 0,
    last_killed_pid: 0,
});

fn oom_score(t: *mut TaskStruct) -> u32 {
    if t.is_null() {
        return 0;
    }
    // SAFETY: t is a valid TaskStruct pointer from the task list.
    unsafe {
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
}

#[unsafe(no_mangle)]
pub extern "C" fn oom_kill() -> i32 {
    lock_acquire(&raw mut scheduler_lock as *mut IrqSpinlock);

    // SAFETY: task_list_head is a valid kernel global.
    let head = unsafe { task_list_head };
    if head.is_null() {
        lock_release(&raw mut scheduler_lock as *mut IrqSpinlock);
        return -1;
    }

    let mut victim: *mut TaskStruct = core::ptr::null_mut();
    let mut best_score: u32 = 0;

    // SAFETY: walking the task list while holding scheduler_lock.
    let mut t = head;
    let mut count = 0;
    while !t.is_null() && count < 256 {
        let score = oom_score(t);
        if score > best_score {
            best_score = score;
            victim = t;
        }
        t = unsafe { (*t).next };
        count += 1;
    }

    if victim.is_null() || best_score == 0 {
        lock_release(&raw mut scheduler_lock as *mut IrqSpinlock);
        // SAFETY: kprint_color takes a valid string.
        unsafe { kprint_color(b"[OOM] no killable process found\n\0".as_ptr(), COLOR_LIGHT_RED); }
        return -1;
    }

    // SAFETY: victim is valid.
    let victim_pid = unsafe { (*victim).pid };
    let victim_pages = unsafe { (*victim).mm.count };

    extern "C" {
        fn task_signal_locked(pid: u32, signal: u32);
    }
    // SAFETY: task_signal_locked is a C function; we hold the scheduler lock.
    unsafe { task_signal_locked(victim_pid, SIGKILL); }

    lock_release(&raw mut scheduler_lock as *mut IrqSpinlock);

    let stats = G_STATS.get_mut();
    stats.oom_kills += 1;
    stats.pages_reclaimed += victim_pages;
    stats.last_killed_pid = victim_pid;

    let mut buf = [0u8; 16];
    // SAFETY: kprint_color takes valid strings.
    unsafe {
        kprint_color(b"\n[OOM] Killed pid=\0".as_ptr(), COLOR_LIGHT_RED);
        itoa(victim_pid as i32, buf.as_mut_ptr());
        kprint_color(buf.as_ptr(), COLOR_LIGHT_RED);
        kprint_color(b" (\0".as_ptr(), COLOR_LIGHT_RED);
        itoa(victim_pages as i32, buf.as_mut_ptr());
        kprint_color(buf.as_ptr(), COLOR_LIGHT_RED);
        kprint_color(b" pages)\n\0".as_ptr(), COLOR_LIGHT_RED);
    }

    // SAFETY: task_reap is a kernel FFI entry point.
    unsafe { task_reap(); }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn oom_get_stats() -> OomStats {
    *G_STATS.get_mut()
}

#[unsafe(no_mangle)]
pub extern "C" fn oom_print_stats() {
    let stats = *G_STATS.get_mut();
    kprint_str(b"[OOM] === OOM Killer Statistics ===\n\0".as_ptr());
    kprint_str(b"  oom_kills:       \0".as_ptr());
    kprint_int(stats.oom_kills as i32);
    kprint_str(b"\n\0".as_ptr());
    kprint_str(b"  pages_reclaimed: \0".as_ptr());
    kprint_int(stats.pages_reclaimed as i32);
    kprint_str(b"\n\0".as_ptr());
    kprint_str(b"  last_killed_pid: \0".as_ptr());
    kprint_int(stats.last_killed_pid as i32);
    kprint_str(b"\n\0".as_ptr());
}
