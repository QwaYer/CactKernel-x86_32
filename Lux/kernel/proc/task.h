#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "gdt.h"
#include "vfs.h"
#include "proc_mm.h"
#include "sync.h"
#include "mmap.h"
#include "dynlink.h"   

#define USER_CODE_SEL 0x1B
#define USER_DATA_SEL 0x23
#define MAX_FD        256

#define SIGKILL  (1u << 0)
#define SIGTERM  (1u << 1)
#define SIGSTOP  (1u << 2)
#define SIGCONT  (1u << 3)
#define SIGPIPE  (1u << 4)
#define SIGALRM  (1u << 5)
#define SIGSEGV  (1u << 6)
#define SIGFPE   (1u << 7)

#define NSIG     8

#define SIG_DFL  ((uint32_t)0)
#define SIG_IGN  ((uint32_t)1)

/* Signals that cannot be blocked or caught */
#define SIG_UNCATCHABLE  (SIGKILL | SIGSTOP)


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

struct task_struct {
    uint32_t esp;
    uint32_t pid;
    task_state state;
    uint8_t  is_kernel;
    void* stack_base;
    void* ustack_phys;
    uint32_t ustack_virt;
    uint32_t* page_directory;

    struct task_struct* next;
    struct task_struct* queue_next;

    uint32_t pending_signals;
    uint32_t signal_mask;           /* blocked signals (sigprocmask) */
    uint32_t saved_signal_mask;     /* mask saved across sigsuspend */
    uint8_t  in_sigsuspend;         /* task is sleeping in sigsuspend */
    uint32_t signal_handlers[NSIG];
    uint32_t sigreturn_trampoline;

    /* alarm / setitimer (ITIMER_REAL) */
    uint32_t alarm_ticks;           /* absolute tick when SIGALRM fires (0=none) */
    uint32_t itimer_value;          /* absolute tick for next interval fire (0=none) */
    uint32_t itimer_interval;       /* reload interval in ticks (0=one-shot) */
    struct vfs_node* fd_table[MAX_FD];
    uint32_t fd_offset[MAX_FD];
    uint32_t fd_flags[MAX_FD];    /* file status flags: O_RDONLY/WRONLY/RDWR, O_NONBLOCK, O_APPEND */
    uint32_t fd_cloexec[MAX_FD];  /* FD_CLOEXEC per descriptor (1 = close-on-exec) */
    proc_page_tracker_t mm;

    mmap_table_t mmap_table;
    dyn_ctx_t* dyn_ctx;

    uint32_t parent_pid;
    int      exit_code;
    uint32_t wait_for_pid;

    uint32_t brk_start;
    uint32_t brk_current;

    uint32_t sleep_until;

    char cwd[256];
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

void task_init();
struct task_struct* create_task(void (*entry_point)());
struct task_struct* create_user_task(void* entry_point);
struct task_struct* create_elf_task(char* path);
struct task_struct* task_fork(struct context_frame* regs);
int task_exec(char* path, char** argv, char** envp, struct context_frame* regs);
void task_kill(uint32_t pid);
void task_signal(uint32_t pid, uint32_t signal);
void task_handle_signals(struct task_struct* t);
int  task_sigaction(struct task_struct* t, uint32_t signum, uint32_t handler);
void task_setup_sigreturn(struct task_struct* t);
int  task_sigprocmask(struct task_struct* t, int how, const uint32_t* set, uint32_t* oldset);
void task_check_timers(void);
void task_reap();
void schedule();
int init_scheduler();
void list_tasks();

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

extern void terminal_task();
extern void switch_to(uint32_t* old_esp, uint32_t new_esp);

#endif