#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "gdt.h"
#include "vfs.h"
#include "proc_mm.h"
#include "sync.h"
#include "mmap.h"
#include "dynlink.h"
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

#define KERNEL_STACK_SIZE 4096
#define USER_STACK_PAGES  4
#define USER_STACK_BYTES  (USER_STACK_PAGES * 4096)
#define KERNEL_BASE       0xC0000000

#define MLFQ_LEVEL_RT          0
#define MLFQ_LEVEL_INTERACTIVE 1
#define MLFQ_LEVEL_NORMAL      2
#define MLFQ_LEVEL_BACKGROUND  3

typedef struct {
    file_t *files[MAX_FD];
} task_fd_table_t;

typedef struct {
    uint32_t ret_addr;
    uint32_t signum;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t eip;
    uint32_t eflags;
} signal_frame_t;

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_ZOMBIE,
    TASK_WAITING
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
    dyn_ctx_t* dyn_ctx;

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
} proc_metadata_t;

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
void task_signal_locked(uint32_t pid, uint32_t signal);
void task_handle_signals(struct task_struct* t);
int  task_sigaction(struct task_struct* t, uint32_t signum, uint32_t handler);
void task_setup_sigreturn(struct task_struct* t);
int  task_sigprocmask(struct task_struct* t, int how, const uint32_t* set, uint32_t* oldset);
void task_check_timers(void);
void task_reap();
void schedule();
int init_scheduler();
void task_set_state(struct task_struct* t, task_state old_state, task_state new_state);

struct task_struct* create_task_with_entry(void*                entry,
                                             uint32_t*            pd,
                                             proc_page_tracker_t* tracker);

struct task_struct* create_task_dynamic(void*                entry,
                                         uint32_t*            pd,
                                         proc_page_tracker_t* tracker,
                                         dyn_ctx_t*           ctx);

uint32_t* vmm_create_address_space();
void vmm_free_address_space(uint32_t* pd);

extern struct task_struct* volatile current_task;
extern struct task_struct* volatile task_list_head;
extern uint32_t next_pid;
extern irq_spinlock_t scheduler_lock;

extern void switch_to(uint32_t* old_esp, uint32_t new_esp);

#endif
