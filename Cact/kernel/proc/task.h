#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "elf.h"
#include "gdt.h"
#include "vfs.h"
#include "proc_mm.h"
#include "sync.h"
#include "mmap.h"
#include "shm.h"

#define USER_CODE_SEL 0x1B
#define USER_DATA_SEL 0x23
#define MAX_FD        256

#define SIGKILL  (1u << 0)
#define SIGTERM  (1u << 1)
#define SIGSTOP  (1u << 2)
#define SIGCONT  (1u << 3)
#define SIGPIPE  (1u << 4)
#define SIGALRM  (1u << 5)
#define SIGCHLD  (1u << 6)
#define SIGFPE   (1u << 7)
#define SIGSEGV  (1u << 8)
#define SIGWINCH (1u << 9)
#define SIGHUP   (1u << 10)
#define SIGINT   (1u << 11)
#define SIGQUIT  (1u << 12)

#define NSIG     13

#define SIG_DFL  ((uint32_t)0)
#define SIG_IGN  ((uint32_t)1)

#define SIG_UNCATCHABLE  (SIGKILL | SIGSTOP)

#define KERNEL_STACK_SIZE 16384
#define USER_STACK_PAGES  4
#define USER_STACK_BYTES  (USER_STACK_PAGES * 4096)

/* Command-name registry (task_comm.c): the value behind /proc/<pid>/comm. */
#define TASK_COMM_LEN   16
#define TASK_COMM_SLOTS 128
void task_comm_set     (uint32_t pid, const char *name);
void task_comm_set_path(uint32_t pid, const char *path);
void task_comm_inherit (uint32_t pid, uint32_t parent_pid);
int  task_comm_get     (uint32_t pid, char *out, int cap);
#ifndef KERNEL_BASE
#define KERNEL_BASE       0xC0000000
#endif

#define MLFQ_LEVEL_RT          0
#define MLFQ_LEVEL_INTERACTIVE 1
#define MLFQ_LEVEL_NORMAL      2
#define MLFQ_LEVEL_BACKGROUND  3

typedef struct {
    file_t *files[MAX_FD];
} task_fd_table_t;

/* Signal-frame payloads, layout-identical to the libc's siginfo_t
 * (<signal.h>) and ucontext_t (<ucontext.h>).  The kernel builds them into the
 * frame so a handler installed with SA_SIGINFO is entered as
 * void handler(int, siginfo_t *, void *). */
typedef struct {
    int32_t  si_signo;
    int32_t  si_code;
    uint32_t si_addr;
    /* sigqueue()'s value (POSIX's union sigval): a sigwaitinfo()/sigtimedwait()
     * or an SA_SIGINFO handler reads it back as si_value. */
    uint32_t si_value;
} cact_siginfo_t;

typedef struct {
    uint32_t gregs[19];             /* REG_* order, see the libc's <ucontext.h> */
} cact_mcontext_t;

typedef struct {
    uint32_t        uc_link;
    uint32_t        uc_flags;
    cact_mcontext_t uc_mcontext;
} cact_ucontext_t;

/* gregs[] indices — the libc's REG_* order. */
enum {
    SF_REG_GS = 0, SF_REG_FS, SF_REG_ES, SF_REG_DS,
    SF_REG_EDI, SF_REG_ESI, SF_REG_EBP, SF_REG_ESP,
    SF_REG_EBX, SF_REG_EDX, SF_REG_ECX, SF_REG_EAX,
    SF_REG_TRAPNO, SF_REG_ERR, SF_REG_EIP, SF_REG_CS,
    SF_REG_EFL, SF_REG_UESP, SF_REG_SS
};

/* The frame sits at the handler's ESP: [ESP] = ret_addr, [ESP+4] = signum,
 * [ESP+8] = siginfo *, [ESP+12] = ucontext *.  sigreturn() reads the whole
 * struct back from that address, so the saved registers and the payloads live
 * in the same object. */
typedef struct {
    uint32_t ret_addr;
    uint32_t signum;              /* handler argument 1 */
    uint32_t siginfo_ptr;         /* handler argument 2 */
    uint32_t ucontext_ptr;        /* handler argument 3 */
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t eip;
    uint32_t eflags;
    uint32_t saved_mask;          /* signal_mask to put back in sigreturn */
    cact_siginfo_t siginfo;
    cact_ucontext_t ucontext;
} signal_frame_t;

/* The handler ABI (and sigreturn's read-back) depends on this exact layout:
 * [ESP+4] = signum, [ESP+8] = siginfo, [ESP+12] = ucontext, the register block
 * from offset 16, and the payloads at 60/72.  Compile-time check so the pic
 * cannot move without the frame code being revisited. */
typedef char _signal_frame_layout_check[
    (__builtin_offsetof(signal_frame_t, signum)       == 4  &&
     __builtin_offsetof(signal_frame_t, siginfo_ptr)  == 8  &&
     __builtin_offsetof(signal_frame_t, ucontext_ptr) == 12 &&
     __builtin_offsetof(signal_frame_t, eax)          == 16 &&
     __builtin_offsetof(signal_frame_t, eip)          == 48 &&
     __builtin_offsetof(signal_frame_t, saved_mask)   == 56 &&
     __builtin_offsetof(signal_frame_t, siginfo)      == 60 &&
     __builtin_offsetof(signal_frame_t, ucontext)     == 76 &&
     sizeof(signal_frame_t)                           == 160) ? 1 : -1];

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_ZOMBIE,
    TASK_WAITING,
    TASK_STOPPED
} task_state;

struct context_frame {
    uint32_t es;
    uint32_t ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip, cs, eflags;
    uint32_t useresp, ss;
};

typedef uint32_t uid_t;
typedef uint32_t gid_t;

/* Refcounted thread-group state (mirrors Rust `ProcShared`).  A process is a
   thread group: the leader and every thread created by CACT_PROCCTL_THREAD_CREATE
   point at one of these.  The address space, fd table and mmap table are shared
   by pointer through each ProcMeta; the value-typed fields below are released
   once, when `refcount` reaches zero in reap_task_free(). */
typedef struct proc_shared {
    uint32_t            refcount;
    uint32_t            brk_start;
    uint32_t            brk_current;
    uint32_t            tgid;
    proc_page_tracker_t mm;
} proc_shared_t;

typedef struct proc_metadata {
    void*    stack_base;
    void*    ustack_phys;
    uint32_t ustack_virt;
    void*    ustack_phys_extra[3];

    uint32_t pending_signals;
    uint32_t signal_mask;
    uint32_t saved_signal_mask;
    uint8_t  in_sigsuspend;
    uint32_t signal_handlers[NSIG];
    uint32_t sigreturn_trampoline;

    uint32_t alarm_ticks;
    uint32_t itimer_value;
    uint32_t itimer_interval;

    task_fd_table_t *fds;
    proc_page_tracker_t mm;
    mmap_table_t *mmap_table;

    uint32_t parent_pid;
    int      exit_code;
    uint32_t wait_for_pid;

    uint32_t brk_start;
    uint32_t brk_current;

    uint32_t sleep_until;

    char cwd[256];

    uid_t uid;
    gid_t gid;
    uid_t euid;
    gid_t egid;

    task_shm_attach_t shm_attachments[TASK_SHM_MAX];

    struct task_struct* wait_next;

    uint32_t pgid;
    uint32_t sid;
    uint32_t umask;
    vfs_node_t *root;

    // Cached symbol table from the main executable (for crash traces)
    uint32_t   exec_base;
    Elf32_Sym* exec_symtab;
    char*      exec_strtab;
    int        exec_symtab_count;

    /* --- threads (appended; existing offsets above are unchanged) --- */
    uint32_t   tgid;         /* leader pid; == pid for a process leader */
    uint8_t    is_thread;    /* 1 for a CACT_PROCCTL_THREAD_CREATE thread */
    uint8_t    _pad_thread[3];
    proc_shared_t* shared;   /* thread-group state (NULL for a kernel task) */
    uint32_t   clr_tid;      /* join futex word cleared on thread exit (0 = none) */
    uint8_t    intr_wait;    /* 1 while blocked in an interruptible futex wait */
    uint8_t    _pad3[3];

    /* --- sigaltstack (appended; offsets above are unchanged) --- */
    uint32_t   altstack_sp;    /* base of the alternate signal stack (0 = none) */
    uint32_t   altstack_size;  /* its size in bytes */
    uint32_t   altstack_flags; /* CACT_SS_DISABLE while switched off */
    uint32_t   signal_flags[NSIG]; /* sa_flags per signal (CACT_SA_ONSTACK, ...) */

    /* --- TLS (appended; offsets above are unchanged) ---
     * The task's %gs base: what ring 3 sees as its thread pointer.  The
     * scheduler loads it on every switch (see the TLS descriptor in gdt.c /
     * smp.rs).  Set by CACT_PROCCTL_SET_TLS, by CACT_PROCCTL_THREAD_CREATE's
     * tls argument, and cleared on exec(). */
    uint32_t   tls_base;

    /* --- signal dispositions (appended; offsets above are unchanged) ---
     * The mask each handler wants blocked while it runs (sa_mask), in the same
     * order as signal_handlers/signal_flags. */
    uint32_t   signal_masks[NSIG];

    /* --- signal sources (appended; offsets above are unchanged) ---
     * What each pending signal came from (si_code) and, for a hardware fault,
     * the address that faulted (CR2), so an SA_SIGINFO handler gets a truthful
     * siginfo_t.  Reset to SI_USER after each delivery. */
    uint32_t   signal_codes[NSIG];
    uint32_t   signal_addr;
    /* POSIX sigwait(): the signal set this task is parked on, 0 when it is not
     * waiting — a signal whose bit is in it wakes the waiter so it can *consume*
     * the signal instead of having it delivered to a handler. */
    uint32_t   sigwait_set;
    /* The value the pending signal carries (sigqueue(), POSIX's union sigval). */
    uint32_t   signal_value;
} proc_metadata_t;

/* The two tail fields the Rust mirrors and the libc agree on; a drift here used
 * to be found by the Rust asserts at boot, and is now a build error instead. */
typedef char _proc_metadata_tail_check[
    (__builtin_offsetof(proc_metadata_t, sigwait_set)   == 788 &&
     __builtin_offsetof(proc_metadata_t, signal_value)  == 792 &&
     sizeof(proc_metadata_t)                            == 796) ? 1 : -1];

struct task_struct {
    uint32_t esp;
    uint32_t* page_directory;
    void* fpu_context_ptr;
    uint32_t pid;
    task_state state;
    uint8_t  is_kernel;
    struct task_struct* next;
    struct task_struct* queue_next;
    uint32_t priority;
    uint32_t time_slice;
    uint32_t ticks_used;
    struct proc_metadata* proc;
};

typedef struct sched_queue {
    struct task_struct* head;
    struct task_struct* tail;
    uint32_t            count;
} sched_queue_t;

static inline void sched_queue_init(sched_queue_t* q) {
    q->head = q->tail = 0;
    q->count = 0;
}

static inline void sched_queue_push(sched_queue_t* q, struct task_struct* t) {
    t->queue_next = 0;
    if (!q->tail) {
        q->head = q->tail = t;
    } else {
        q->tail->queue_next = t;
        q->tail = t;
    }
    q->count++;
}

static inline struct task_struct* sched_queue_pop(sched_queue_t* q) {
    if (!q->head) return 0;
    struct task_struct* t = q->head;
    q->head = t->queue_next;
    if (!q->head) q->tail = 0;
    t->queue_next = 0;
    q->count--;
    return t;
}

static inline void sched_queue_remove(sched_queue_t* q, struct task_struct* t) {
    struct task_struct* prev = 0;
    struct task_struct* cur  = q->head;
    while (cur) {
        if (cur == t) {
            if (prev) prev->queue_next = cur->queue_next;
            else      q->head          = cur->queue_next;
            if (q->tail == cur)
                q->tail = prev;
            cur->queue_next = 0;
            q->count--;
            return;
        }
        prev = cur;
        cur  = cur->queue_next;
    }
}

extern sched_queue_t ready_queue;
extern sched_queue_t sleep_queue;
extern sched_queue_t zombie_queue;
extern sched_queue_t wait_queue;

void mlfq_wake_task(struct task_struct* task);

void task_init();
struct task_struct* create_task(void (*entry_point)());
struct task_struct* create_user_task(void* entry_point);
struct task_struct* create_elf_task(char* path);
struct task_struct* task_fork(struct context_frame* regs);
int task_exec(char* path, char** argv, char** envp, struct context_frame* regs);
void task_kill(uint32_t pid);
void task_signal(uint32_t pid, uint32_t signal);
/* Same, but records the signal's source (si_code) and fault address first. */
void task_signal_code(uint32_t pid, uint32_t signal, uint32_t code, uint32_t addr);
void task_signal_locked(uint32_t pid, uint32_t signal);
/* Pending, unmasked signal on the *current* task (0 = none).  Blocking waits poll
   this so they can return -EINTR instead of delaying the signal's delivery. */
uint32_t task_signal_pending_current(void);
void task_handle_signals(struct task_struct* t);
int  task_sigaction(struct task_struct* t, uint32_t signum, uint32_t handler,
                    uint32_t flags, uint32_t mask);
void task_setup_sigreturn(struct task_struct* t);
int  task_sigprocmask(struct task_struct* t, int how, const uint32_t* set, uint32_t* oldset);
/* Park the *current* task on signal set `set`, atomically with the pending
   check: returns 1 without parking when one of `set` is already pending, 2 when
   the task is Zombie (it must not sleep), 0 when it parked (a signal whose bit
   is in `set`, or nothing — the caller re-checks and parks again).  The lock it
   shares with task_signal_locked is what makes a signal arriving in between
   impossible to lose. */
uint32_t task_park_sigwait(uint32_t set);
/* Send `signal` to `pid` carrying `value` (POSIX sigqueue()); the value is
   handed back by sigwaitinfo()/sigtimedwait() or an SA_SIGINFO handler. */
void task_signal_value(uint32_t pid, uint32_t signal, uint32_t value);
void task_check_timers(void);
void task_reap();
void schedule();
int init_scheduler();
void task_set_state(struct task_struct* t, task_state old_state, task_state new_state);
/* Atomically park the *current* task in `blocked_state` (TASK_SLEEPING/
   TASK_WAITING) unless a terminating signal already marked it Zombie, in which
   case it must NOT park (that would resurrect it).  Returns 0 if parked, 1 if
   Zombie. */
int task_park_state(uint32_t blocked_state);

/* --- threads (sched/src/task_thread.rs, syscall/process/thread.c) --- */

/* Create a thread in the calling task's group: ring-3 entry at `entry` with the
 * user stack pointer `user_esp`.  `clr_tid` is a user word the kernel zeroes
 * (and futex-wakes) when the thread exits, or 0.  Returns the new tid (>0) or a
 * negative error. */
int  sched_create_thread(void* entry, uint32_t user_esp, uint32_t tls,
                         uint32_t set_tid, uint32_t clr_tid);
/* Set the calling task's `%gs` base (CACT_PROCCTL_SET_TLS) and load it on this
 * core; 0 clears it.  Returns 0 or a negative error. */
int  sched_set_tls(uint32_t base);
/* Wake up to `count` futex waiters on `uaddr` in address space `pd` (kernel FFI
 * used by the scheduler's thread-exit path). */
int  cact_futex_wake_addr(uint32_t* pd, uint32_t* uaddr, int count);
/* Send `signal` to every task of the calling task's group except `except_pid`. */
void task_signal_group(uint32_t except_pid, uint32_t signal);
void task_signal_pgid(uint32_t pgid, uint32_t signal);
/* Ioctl helpers for /proc/self/ctl (CACT_PROCCTL_THREAD_*); `arg` is the raw
 * userspace pointer the ioctl received. */
int  proc_thread_create(const void* arg);
int  proc_thread_exit(uint32_t code);
int  proc_thread_gettid(void);
int  proc_futex(const void* arg);
/* CACT_PROCCTL_SET_TLS: validate `base` and install it as the caller's %gs. */
int  proc_set_tls(uint32_t base);

struct task_struct* create_task_with_entry(void*                entry,
                                             uint32_t*            pd,
                                             proc_page_tracker_t* tracker);

uint32_t* vmm_create_address_space();
void vmm_free_address_space(uint32_t* pd);

/* Each core keeps its own running-task pointer; this resolves to the calling
 * CPU's slot (sched/src/percpu.rs). */
struct task_struct** cact_current_task_slot(void);
#define current_task (*cact_current_task_slot())

extern struct task_struct* volatile task_list_head;
extern uint32_t next_pid;
extern irq_spinlock_t scheduler_lock;

extern void switch_to(uint32_t* old_esp, uint32_t new_esp);

#endif
