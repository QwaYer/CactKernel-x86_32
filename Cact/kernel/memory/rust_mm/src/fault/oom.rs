//! Out-of-memory killer: scores user tasks by resident page count and signals a victim.
//!
//! Runs with the scheduler lock held while walking the global task list.

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
    // SAFETY: `t` is a valid `TaskStruct` pointer from the task list (the caller
    // walks it under the scheduler lock); this shared borrow is consumed by the
    // checks below.
    let t = unsafe { &*t };
    if t.is_kernel != 0 {
        return 0;
    }
    if t.pid <= 1 {
        return 0;
    }
    if t.state == TASK_ZOMBIE {
        return 0;
    }
    if t.proc.is_null() {
        return 0;
    }
    // SAFETY: `t.proc` is non-null (checked above) and points at the task's live
    // `ProcMeta`, which the task list owns, so this field read is in bounds.
    unsafe { (*t.proc).mm.count }
}

#[unsafe(no_mangle)]
pub extern "C" fn oom_kill() -> i32 {
    // SAFETY: `scheduler_lock` is the C scheduler spinlock; `get()` hands out its
    // stable address.  This is the only lock the task-list walk needs and it is
    // not held yet at this point.
    lock_acquire(unsafe { scheduler_lock.get() });

    // SAFETY: task_list_head is a valid kernel global.
    let head = unsafe { *task_list_head.get() };
    if head.is_null() {
        // SAFETY: matching release of `scheduler_lock`, acquired above.
        lock_release(unsafe { scheduler_lock.get() });
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
        // SAFETY: `t` is a node of the task list, traversed while `scheduler_lock` is
        // held.
        t = unsafe { (*t).next };
        count += 1;
    }

    if victim.is_null() || best_score == 0 {
        // SAFETY: release of `scheduler_lock`, still held from the acquire above.
        lock_release(unsafe { scheduler_lock.get() });
        // SAFETY: printk_color takes a valid string.
        unsafe { printk_color(c"[OOM] no killable process found\n".as_ptr() as *const u8, COLOR_LIGHT_RED); }
        return -1;
    }

    // SAFETY: victim is valid.
    let victim_pid = unsafe { (*victim).pid };
    // SAFETY: `victim` was selected from the task list and its `proc` field is
    // owned by the task; `scheduler_lock` is still held here.
    let victim_proc = unsafe { (*victim).proc };
    // SAFETY: `victim_proc` is that task's live `ProcMeta`, so this field read is
    // in bounds.
    let victim_pages = unsafe { (*victim_proc).mm.count };

    unsafe extern "C" {
        fn task_signal_locked(pid: u32, signal: u32);
    }
    // SAFETY: task_signal_locked is a C function; we hold the scheduler lock.
    unsafe { task_signal_locked(victim_pid, SIGKILL); }

    // SAFETY: matching release of `scheduler_lock` before returning.
    lock_release(unsafe { scheduler_lock.get() });

    // SAFETY: `G_STATS` is the OOM statistics block; `oom_kill` is its only writer.
    // It is not serialised against a concurrent OOM kill on another CPU, but the
    // fields are best-effort counters, so a lost update is harmless.
    let stats = unsafe { KStatic::get_mut(G_STATS.as_ptr()) };
    stats.oom_kills += 1;
    stats.pages_reclaimed += victim_pages;
    stats.last_killed_pid = victim_pid;

    let mut buf = [0u8; 16];
    // SAFETY: `printk_color` takes a NUL-terminated static string; the literal
    // below is one.
    unsafe { printk_color(c"\n[OOM] Killed pid=".as_ptr() as *const u8, COLOR_LIGHT_RED) };
    // SAFETY: `buf` is a live 16-byte stack array, so `itoa` writes in bounds.
    unsafe { itoa(victim_pid as i32, buf.as_mut_ptr()) };
    // SAFETY: `buf` was just made a NUL-terminated string by `itoa`.
    unsafe { printk_color(buf.as_ptr(), COLOR_LIGHT_RED) };
    // SAFETY: NUL-terminated static string, as above.
    unsafe { printk_color(c" (".as_ptr() as *const u8, COLOR_LIGHT_RED) };
    // SAFETY: `buf` is a live 16-byte stack array, so `itoa` writes in bounds.
    unsafe { itoa(victim_pages as i32, buf.as_mut_ptr()) };
    // SAFETY: `buf` was just made a NUL-terminated string by `itoa`.
    unsafe { printk_color(buf.as_ptr(), COLOR_LIGHT_RED) };
    // SAFETY: NUL-terminated static string, as above.
    unsafe { printk_color(c" pages)\n".as_ptr() as *const u8, COLOR_LIGHT_RED) };

    // SAFETY: task_reap is a kernel FFI entry point.
    unsafe { task_reap(); }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn oom_get_stats() -> OomStats {
    // SAFETY: read-only snapshot of the OOM counters; no lock is needed to
    // observe a possibly-slightly-stale counter value.
    *unsafe { KStatic::get_mut(G_STATS.as_ptr()) }
}

#[unsafe(no_mangle)]
pub extern "C" fn oom_print_stats() {
    // SAFETY: read-only snapshot of `G_STATS` for printing.
    let stats = *unsafe { KStatic::get_mut(G_STATS.as_ptr()) };
    kprint_str(c"[OOM] === OOM Killer Statistics ===\n".as_ptr() as *const u8);
    kprint_str(c"  oom_kills:       ".as_ptr() as *const u8);
    kprint_int(stats.oom_kills as i32);
    kprint_str(c"\n".as_ptr() as *const u8);
    kprint_str(c"  pages_reclaimed: ".as_ptr() as *const u8);
    kprint_int(stats.pages_reclaimed as i32);
    kprint_str(c"\n".as_ptr() as *const u8);
    kprint_str(c"  last_killed_pid: ".as_ptr() as *const u8);
    kprint_int(stats.last_killed_pid as i32);
    kprint_str(c"\n".as_ptr() as *const u8);
}
