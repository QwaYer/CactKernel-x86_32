#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "gdt.h"
#include "vfs.h"
#include "proc_mm.h"
#include "sync.h"

#define USER_CODE_SEL 0x1B
#define USER_DATA_SEL 0x23
#define MAX_FD        256

/* Сигналы — битовая маска */
#define SIGKILL  (1 << 0)   /* немедленная смерть, не перехватывается */
#define SIGTERM  (1 << 1)   /* вежливая просьба завершиться */
#define SIGSTOP  (1 << 2)   /* приостановить процесс */
#define SIGCONT  (1 << 3)   /* продолжить после SIGSTOP */

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_ZOMBIE
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
    uint32_t pending_signals;          /* битовая маска ожидающих сигналов */
    struct vfs_node* fd_table[MAX_FD];
    proc_page_tracker_t mm;            /* трекер физических страниц ELF     */
};

void task_init();
struct task_struct* create_task(void (*entry_point)());
struct task_struct* create_user_task(void* entry_point);
struct task_struct* create_elf_task(char* path);
struct task_struct* task_fork(struct context_frame* regs);
int task_exec(char* path, struct context_frame* regs);
void task_kill(uint32_t pid);
void task_signal(uint32_t pid, uint32_t signal);
void task_handle_signals(struct task_struct* t);
void task_reap();
void schedule();
int init_scheduler();
uint32_t* vmm_create_address_space();
void vmm_free_address_space(uint32_t* pd);

extern struct task_struct* volatile current_task;
extern struct task_struct* volatile task_list_head;
extern uint32_t next_pid;
extern spinlock_t scheduler_lock;

extern void terminal_task();
extern void switch_to(uint32_t* old_esp, uint32_t new_esp);

#endif