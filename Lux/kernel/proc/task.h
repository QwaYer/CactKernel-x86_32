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

typedef uint32_t uid_t;
typedef uint32_t gid_t;

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
    uint32_t esp;                          /* offset  0 */
    uint32_t pid;                          /* offset  4 */
    task_state state;                      /* offset  8 */
    uint8_t  is_kernel;                    /* offset 12 */
    uint8_t  _pad0[3];                     /* offset 13 — alignment to 16 */
    void* stack_base;                      /* offset 16 */
    void* ustack_phys;                     /* offset 20 */
    uint32_t ustack_virt;                  /* offset 24 */
    uint32_t* page_directory;              /* offset 28 */

    struct task_struct* next;              /* offset 32 */
    struct task_struct* queue_next;         /* offset 36 */

    /* MLFQ fields (managed by Rust scheduler) */
    uint32_t priority;                     /* offset 40 */
    uint32_t time_slice;                   /* offset 44 */
    uint32_t ticks_used;                   /* offset 48 */

    uint32_t pending_signals;
    uint32_t signal_mask;
    uint32_t saved_signal_mask;
    uint8_t  in_sigsuspend;
    uint8_t  _pad1[3];                     /* alignment */
    uint32_t signal_handlers[NSIG];
    uint32_t sigreturn_trampoline;

    uint32_t alarm_ticks;
    uint32_t itimer_value;
    uint32_t itimer_interval;
    struct vfs_node* fd_table[MAX_FD];
    uint32_t fd_offset[MAX_FD];
    uint32_t fd_flags[MAX_FD];
    uint32_t fd_cloexec[MAX_FD];
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

    uid_t uid;
    gid_t gid;
    uid_t euid;
    gid_t egid;

    task_shm_attach_t shm_attachments[TASK_SHM_MAX];

    struct task_struct* wait_next;          /* timer-wheel link */
};

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