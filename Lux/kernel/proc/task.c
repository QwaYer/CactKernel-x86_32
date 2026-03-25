#include "task.h"
#include "kernel.h"
#include "memory.h"
#include "page_fault.h"
#include "libc.h"
#include "gdt.h"
#include "dynlink.h"
#include "vfs.h"

struct task_struct* volatile current_task   = 0;
struct task_struct* volatile task_list_head = 0;
uint32_t            next_pid                = 1;
irq_spinlock_t      scheduler_lock;

sched_queue_t ready_queue;
sched_queue_t sleep_queue;
sched_queue_t zombie_queue;
sched_queue_t wait_queue;

static volatile int schedule_in_progress = 0;

extern void vmm_fork_address_space(uint32_t* src_pd, uint32_t* dst_pd);
extern uint32_t timer_ticks_get(void);


static sched_queue_t* queue_for_state(task_state s) {
    switch (s) {
        case TASK_READY:    return &ready_queue;
        case TASK_SLEEPING: return &sleep_queue;
        case TASK_ZOMBIE:   return &zombie_queue;
        case TASK_WAITING:  return &wait_queue;
        default:            return 0;
    }
}

void task_set_state(struct task_struct* t, task_state old_state, task_state new_state) {
    if (!t || old_state == new_state) return;
    sched_queue_t* old_q = queue_for_state(old_state);
    if (old_q) sched_queue_remove(old_q, t);
    t->state = new_state;
    sched_queue_t* new_q = queue_for_state(new_state);
    if (new_q) sched_queue_push(new_q, t);
}

static void task_list_add(struct task_struct* t) {
    if (!task_list_head) {
        task_list_head = t;
        t->next = t;
    } else {
        struct task_struct* last = task_list_head;
        while (last->next != task_list_head) last = last->next;
        last->next = t;
        t->next = task_list_head;
    }
}

static void task_list_remove(struct task_struct* t) {
    if (!task_list_head || !t) return;
    if (task_list_head == t) {
        if (t->next == t) { task_list_head = 0; return; }
        task_list_head = t->next;
    }
    struct task_struct* cur = task_list_head;
    while (cur->next != task_list_head && cur->next != t) cur = cur->next;
    if (cur->next == t) cur->next = t->next;
    t->next = 0;
}


void task_init() {
    current_task   = 0;
    task_list_head = 0;
    next_pid       = 1;
    schedule_in_progress = 0;
    irq_spinlock_init(&scheduler_lock);

    sched_queue_init(&ready_queue);
    sched_queue_init(&sleep_queue);
    sched_queue_init(&zombie_queue);
    sched_queue_init(&wait_queue);
}


struct task_struct* create_task(void (*entry_point)()) {
    struct task_struct* t = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!t) return 0;

    uint32_t* stack = (uint32_t*)kalloc();
    if (!stack) { kfree_heap(t); return 0; }

    uint32_t* esp = (uint32_t*)((uint32_t)stack + 4096);

    *(--esp) = 0x00000202;
    *(--esp) = 0x08;
    *(--esp) = (uint32_t)entry_point;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0x10;
    *(--esp) = 0x10;

    t->esp            = (uint32_t)esp;
    t->stack_base     = (void*)stack;
    t->pid            = next_pid++;
    t->state          = TASK_READY;
    t->is_kernel      = 1;
    t->page_directory = 0;
    t->ustack_phys    = 0;
    t->ustack_virt    = 0;
    t->pending_signals= 0;
    t->queue_next     = 0;
    t->dyn_ctx        = 0;
    t->parent_pid     = 0;
    t->exit_code      = 0;
    t->wait_for_pid   = 0;
    t->brk_start      = 0;
    t->brk_current    = 0;
    t->sleep_until    = 0;
    proc_tracker_init(&t->mm);
    for (int i = 0; i < MAX_FD; i++) {
        t->fd_table[i] = 0;
        t->fd_offset[i] = 0;
    }

    irq_spinlock_acquire(&scheduler_lock);
    task_list_add(t);
    sched_queue_push(&ready_queue, t);
    irq_spinlock_release(&scheduler_lock);
    return t;
}


static struct task_struct* create_user_task_internal(void* entry_point, int add_to_list) {
    struct task_struct* t = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!t) return 0;

    uint32_t* kstack = (uint32_t*)kalloc();
    if (!kstack) { kfree_heap(t); return 0; }

    uint32_t* ustack_phys = (uint32_t*)kalloc();
    if (!ustack_phys) { kfree_page(kstack); kfree_heap(t); return 0; }

    uint32_t ustack_virt = 0xBFFFF000;
    t->ustack_phys = ustack_phys;
    t->ustack_virt = ustack_virt;

    uint32_t* esp = (uint32_t*)((uint32_t)kstack + 4096);

    *(--esp) = 0x23;                          // ss
    *(--esp) = ustack_virt + 4096 - 4;       // useresp
    *(--esp) = 0x00000202;                    // eflags
    *(--esp) = 0x1B;                          // cs
    *(--esp) = (uint32_t)entry_point;         // eip
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0x23;
    *(--esp) = 0x23;

    t->esp             = (uint32_t)esp;
    t->stack_base      = (void*)kstack;
    t->pid             = next_pid++;
    t->state           = TASK_READY;
    t->is_kernel       = 0;
    t->page_directory  = 0;
    t->pending_signals = 0;
    t->queue_next      = 0;
    t->dyn_ctx         = 0;
    t->sigreturn_trampoline = 0;
    t->parent_pid      = current_task ? current_task->pid : 0;
    t->exit_code       = 0;
    t->wait_for_pid    = 0;
    t->brk_start       = 0;
    t->brk_current     = 0;
    t->sleep_until     = 0;
    for (int i = 0; i < NSIG; i++) t->signal_handlers[i] = SIG_DFL;
    proc_tracker_init(&t->mm);
    for (int i = 0; i < MAX_FD; i++) {
        t->fd_table[i] = 0;
        t->fd_offset[i] = 0;
    }

    if (add_to_list) {
        irq_spinlock_acquire(&scheduler_lock);
        task_list_add(t);
        sched_queue_push(&ready_queue, t);
        irq_spinlock_release(&scheduler_lock);
    }
    return t;
}

struct task_struct* create_user_task(void* entry_point) {
    return create_user_task_internal(entry_point, 1);
}

struct task_struct* create_task_with_entry(void*                entry,
                                            uint32_t*            pd,
                                            proc_page_tracker_t* tracker)
{
    struct task_struct* t = create_user_task_internal(entry, 0);
    if (!t) return 0;

    t->mm = *tracker;

    t->page_directory = pd;

    uint32_t* stk = (uint32_t*)t->esp;
    stk[10] = (uint32_t)entry;


    vmm_map(pd, t->ustack_virt, (uint32_t)t->ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);

    // compute brk from highest mapped user page (below stack region)
    {
        uint32_t highest = 0;
        for (int i = 1; i < 768; i++) {  // skip entry 0, scan user space
            if (!(pd[i] & PAGE_PRESENT)) continue;
            uint32_t* pt = (uint32_t*)(pd[i] & ~0xFFFu);
            for (int j = 1023; j >= 0; j--) {
                if (pt[j] & PAGE_PRESENT) {
                    uint32_t va = ((uint32_t)i << 22) | ((uint32_t)j << 12);
                    if (va < 0xBF000000u && va + PAGE_SIZE > highest)
                        highest = va + PAGE_SIZE;
                    break;
                }
            }
        }
        t->brk_start   = highest;
        t->brk_current = highest;
    }

    // push argc/argv onto user stack (physical)
    uint32_t ustack_top = t->ustack_virt + PAGE_SIZE;
    uint8_t* phys = (uint8_t*)t->ustack_phys;

    uint32_t sp = ustack_top - 4;

    // argv[0] = NULL
    sp -= 4;
    *(uint32_t*)(phys + (sp - t->ustack_virt)) = 0;
    uint32_t argv_vaddr = sp;

    // argv pointer
    sp -= 4;
    *(uint32_t*)(phys + (sp - t->ustack_virt)) = argv_vaddr;

    // argc = 0
    sp -= 4;
    *(uint32_t*)(phys + (sp - t->ustack_virt)) = 0;

    // patch useresp in iret frame
    stk[13] = sp;

    task_setup_sigreturn(t);

    irq_spinlock_acquire(&scheduler_lock);
    task_list_add(t);
    sched_queue_push(&ready_queue, t);
    irq_spinlock_release(&scheduler_lock);

    return t;
}

struct task_struct* create_task_dynamic(void*                entry,
                                         uint32_t*            pd,
                                         proc_page_tracker_t* tracker,
                                         dyn_ctx_t*           ctx)
{
    struct task_struct* t = create_task_with_entry(entry, pd, tracker);
    if (!t) return 0;

    irq_spinlock_acquire(&scheduler_lock);
    t->dyn_ctx = ctx;
    irq_spinlock_release(&scheduler_lock);

    return t;
}


struct task_struct* create_elf_task(char* path) {
    uint32_t* pd = vmm_create_address_space();
    if (!pd) return 0;

    struct task_struct* t = create_user_task_internal((void*)0, 0);
    if (!t) { kfree_page(pd); return 0; }

    proc_tracker_init(&t->mm);
    void* entry_point = load_elf(path, pd, &t->mm);
    if (!entry_point) {
        kfree_page(t->stack_base);
        kfree_page(t->ustack_phys);
        kfree_heap(t);
        kfree_page(pd);
        return 0;
    }

    t->page_directory = pd;
    t->dyn_ctx        = 0;

    uint32_t* stk = (uint32_t*)t->esp;
    stk[10] = (uint32_t)entry_point;

    vmm_map(pd, t->ustack_virt, (uint32_t)t->ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);

    // compute brk from highest mapped user page
    {
        uint32_t highest = 0;
        for (int i = 1; i < 768; i++) {
            if (!(pd[i] & PAGE_PRESENT)) continue;
            uint32_t* pt = (uint32_t*)(pd[i] & ~0xFFFu);
            for (int j = 1023; j >= 0; j--) {
                if (pt[j] & PAGE_PRESENT) {
                    uint32_t va = ((uint32_t)i << 22) | ((uint32_t)j << 12);
                    if (va < 0xBF000000u && va + PAGE_SIZE > highest)
                        highest = va + PAGE_SIZE;
                    break;
                }
            }
        }
        t->brk_start   = highest;
        t->brk_current = highest;
    }

    // push argc/argv onto user stack (physical)
    uint32_t ustack_top = t->ustack_virt + PAGE_SIZE;
    uint8_t* phys = (uint8_t*)t->ustack_phys;
    uint32_t sp = ustack_top - 4;

    sp -= 4;
    *(uint32_t*)(phys + (sp - t->ustack_virt)) = 0;  // argv[0]=NULL
    uint32_t argv_vaddr = sp;

    sp -= 4;
    *(uint32_t*)(phys + (sp - t->ustack_virt)) = argv_vaddr;  // argv ptr

    sp -= 4;
    *(uint32_t*)(phys + (sp - t->ustack_virt)) = 0;  // argc=0

    stk[13] = sp;  // patch useresp

    task_setup_sigreturn(t);

    irq_spinlock_acquire(&scheduler_lock);
    task_list_add(t);
    sched_queue_push(&ready_queue, t);
    irq_spinlock_release(&scheduler_lock);

    return t;
}


int init_scheduler() {
    current_task = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!current_task) {
        kprint("[SCHED] FATAL: cannot alloc idle task\n");
        return -1;
    }

    current_task->pid             = 0;
    current_task->stack_base      = 0;
    current_task->state           = TASK_RUNNING;
    current_task->is_kernel       = 1;
    current_task->page_directory  = 0;
    current_task->ustack_phys     = 0;
    current_task->ustack_virt     = 0;
    current_task->next            = current_task;
    current_task->pending_signals = 0;
    current_task->queue_next      = 0;
    current_task->dyn_ctx         = 0;
    current_task->parent_pid      = 0;
    current_task->exit_code       = 0;
    current_task->wait_for_pid    = 0;
    current_task->brk_start       = 0;
    current_task->brk_current     = 0;
    current_task->sleep_until     = 0;
    proc_tracker_init(&current_task->mm);
    for (int i = 0; i < MAX_FD; i++) {
        current_task->fd_table[i] = 0;
        current_task->fd_offset[i] = 0;
    }
    task_list_head = current_task;


    create_task(terminal_task);
    return 0;
}

static void task_reap_internal() {
    struct task_struct* t;
    while ((t = sched_queue_pop(&zombie_queue)) != 0) {

        task_list_remove(t);

        for (int i = 0; i < MAX_FD; i++) {
            if (t->fd_table[i]) {
                close_vfs(t->fd_table[i]);
                t->fd_table[i] = 0;
            }
        }

        if (t->dyn_ctx) {
            dynlink_unload_all(t->dyn_ctx);
            kfree_heap(t->dyn_ctx);
            t->dyn_ctx = 0;
        }

        proc_free_pages(&t->mm);
        if (t->stack_base)     kfree_page(t->stack_base);
        if (t->ustack_phys)    kfree_page(t->ustack_phys);
        if (t->page_directory) vmm_free_address_space(t->page_directory);
        kfree_heap(t);
    }
}

void task_reap() {
    irq_spinlock_acquire(&scheduler_lock);
    task_reap_internal();
    irq_spinlock_release(&scheduler_lock);
}

// wake sleeping tasks whose timer expired
static void task_wake_sleepers(void) {
    uint32_t now = timer_ticks_get();
    struct task_struct* cur = sleep_queue.head;
    while (cur) {
        struct task_struct* next = cur->queue_next;
        if (cur->sleep_until != 0 && now >= cur->sleep_until) {
            cur->sleep_until = 0;
            sched_queue_remove(&sleep_queue, cur);
            cur->state = TASK_READY;
            sched_queue_push(&ready_queue, cur);
        }
        cur = next;
    }
}

void schedule() {
    if (__sync_lock_test_and_set(&schedule_in_progress, 1)) {
        return;
    }

    irq_spinlock_acquire(&scheduler_lock);

    if (!task_list_head || !current_task) {
        irq_spinlock_release(&scheduler_lock);
        __sync_lock_release(&schedule_in_progress);
        return;
    }

    task_reap_internal();
    task_wake_sleepers();
    task_handle_signals(current_task);

    if (current_task->state == TASK_ZOMBIE) {
        sched_queue_push(&zombie_queue, current_task);
        struct task_struct* next = sched_queue_pop(&ready_queue);
        if (!next) {
            kprint_color("[SCHED] WARN: zombie but no ready task!\n", 12);
            current_task->state = TASK_RUNNING;
            sched_queue_pop(&zombie_queue);
            irq_spinlock_release(&scheduler_lock);
            __sync_lock_release(&schedule_in_progress);
            return;
        }
        current_task = next;
        current_task->state = TASK_RUNNING;
        irq_spinlock_release(&scheduler_lock);
        __sync_lock_release(&schedule_in_progress);
        return;
    }

    if (current_task->state == TASK_SLEEPING) {
        sched_queue_push(&sleep_queue, current_task);
        struct task_struct* next = sched_queue_pop(&ready_queue);
        if (!next) {
            kprint_color("[SCHED] WARN: sleeping but no ready task!\n", 14);
            sched_queue_remove(&sleep_queue, current_task);
            current_task->state = TASK_RUNNING;
            irq_spinlock_release(&scheduler_lock);
            __sync_lock_release(&schedule_in_progress);
            return;
        }
        current_task = next;
        current_task->state = TASK_RUNNING;
        irq_spinlock_release(&scheduler_lock);
        __sync_lock_release(&schedule_in_progress);
        return;
    }

    if (current_task->state == TASK_WAITING) {
        sched_queue_push(&wait_queue, current_task);
        struct task_struct* next = sched_queue_pop(&ready_queue);
        if (!next) {
            kprint_color("[SCHED] WARN: waiting but no ready task!\n", 14);
            sched_queue_remove(&wait_queue, current_task);
            current_task->state = TASK_RUNNING;
            irq_spinlock_release(&scheduler_lock);
            __sync_lock_release(&schedule_in_progress);
            return;
        }
        current_task = next;
        current_task->state = TASK_RUNNING;
        irq_spinlock_release(&scheduler_lock);
        __sync_lock_release(&schedule_in_progress);
        return;
    }

    {
        struct task_struct* next = sched_queue_pop(&ready_queue);
        if (!next) {
            irq_spinlock_release(&scheduler_lock);
            __sync_lock_release(&schedule_in_progress);
            return;
        }
        current_task->state = TASK_READY;
        sched_queue_push(&ready_queue, current_task);
        current_task = next;
        current_task->state = TASK_RUNNING;
    }

    irq_spinlock_release(&scheduler_lock);
    __sync_lock_release(&schedule_in_progress);
}


void list_tasks() {
    irq_spinlock_acquire(&scheduler_lock);
    if (!task_list_head) { irq_spinlock_release(&scheduler_lock); return; }

    int count = 0;
    struct task_struct* tmp = task_list_head;
    do { count++; tmp = tmp->next; } while (tmp != task_list_head && count < 256);

    struct { uint32_t pid; task_state state; uint8_t is_kernel; } tasks[count];
    tmp = task_list_head;
    for (int i = 0; i < count; i++) {
        tasks[i].pid       = tmp->pid;
        tasks[i].state     = tmp->state;
        tasks[i].is_kernel = tmp->is_kernel;
        tmp = tmp->next;
    }
    irq_spinlock_release(&scheduler_lock);

    kprint("\nPID  STATE     TYPE\n");
    kprint("---  --------  --------\n");
    for (int i = 0; i < count; i++) {
        char buf[16];
        itoa((int)tasks[i].pid, buf);
        kprint(buf);
        int digits = tasks[i].pid < 10 ? 1 : tasks[i].pid < 100 ? 2 : tasks[i].pid < 1000 ? 3 : 4;
        for (int j = digits; j < 5; j++) kprint(" ");
        switch (tasks[i].state) {
            case TASK_RUNNING:  kprint("running   "); break;
            case TASK_READY:    kprint("ready     "); break;
            case TASK_ZOMBIE:   kprint("zombie    "); break;
            case TASK_WAITING:  kprint("waiting   "); break;
            default:            kprint("sleeping  "); break;
        }
        kprint(tasks[i].is_kernel ? "kernel\n" : "user\n");
    }
}

void task_kill(uint32_t pid) {
    if (pid == 0) {
        return;
    }
    task_signal(pid, SIGKILL);
}

void task_signal(uint32_t pid, uint32_t signal) {
    irq_spinlock_acquire(&scheduler_lock);
    struct task_struct* t = task_list_head;
    if (!t) { irq_spinlock_release(&scheduler_lock); return; }

    do {
        if (t->pid == pid) {
            t->pending_signals |= signal;

            if ((signal & SIGKILL) && t->state == TASK_SLEEPING) {
                sched_queue_remove(&sleep_queue, t);
                t->state = TASK_READY;
                sched_queue_push(&ready_queue, t);
            }
            if ((signal & SIGKILL) && t->state == TASK_WAITING) {
                sched_queue_remove(&wait_queue, t);
                t->state = TASK_READY;
                sched_queue_push(&ready_queue, t);
            }
            break;
        }
        t = t->next;
    } while (t != task_list_head);

    irq_spinlock_release(&scheduler_lock);
}

void task_handle_signals(struct task_struct* t) {
    if (!t || !t->pending_signals) return;

    if (t->pending_signals & SIGKILL) {
        t->pending_signals = 0;
        t->state = TASK_ZOMBIE;
        kprint("[SCHED] SIGKILL pid=");
        char buf[16]; itoa((int)t->pid, buf); kprint(buf); kprint("\n");
        return;
    }

    if (t->pending_signals & SIGTERM) {
        t->pending_signals &= ~(uint32_t)SIGTERM;
        uint32_t handler = t->signal_handlers[1];
        if (handler == SIG_DFL || handler == SIG_IGN) {
            t->state = TASK_ZOMBIE;
            return;
        }
    }

    if (t->pending_signals & SIGSTOP) {
        t->pending_signals &= ~(uint32_t)SIGSTOP;
        t->state = TASK_SLEEPING;
        return;
    }

    if (t->pending_signals & SIGCONT) {
        t->pending_signals &= ~(uint32_t)SIGCONT;
        if (t->state == TASK_SLEEPING) {
            t->state = TASK_READY;
        }
    }
}

int task_sigaction(struct task_struct* t, uint32_t signum, uint32_t handler) {
    if (!t || signum >= NSIG) return -1;
    if (signum == 0) return -1;
    t->signal_handlers[signum] = handler;
    return 0;
}

void task_setup_sigreturn(struct task_struct* t) {
    if (!t || !t->page_directory) return;

    uint32_t tramp_vaddr = 0xBFFFE000;
    void* phys = kalloc();
    if (!phys) return;

    uint8_t* p = (uint8_t*)phys;
    for (int i = 0; i < (int)PAGE_SIZE; i++) p[i] = 0;

    p[0] = 0xB8; p[1] = 0x77; p[2] = 0x00; p[3] = 0x00; p[4] = 0x00;
    p[5] = 0xCD; p[6] = 0x80;
    p[7] = 0xF4;

    vmm_map(t->page_directory, tramp_vaddr, (uint32_t)phys,
            PAGE_USER | PAGE_PRESENT);
    t->sigreturn_trampoline = tramp_vaddr;
}


struct task_struct* task_fork(struct context_frame* regs) {
    irq_spinlock_acquire(&scheduler_lock);

    struct task_struct* parent = (struct task_struct*)current_task;
    if (!parent) { irq_spinlock_release(&scheduler_lock); return 0; }

    uint32_t* child_pd = vmm_create_address_space();
    if (!child_pd) { irq_spinlock_release(&scheduler_lock); return 0; }

    struct task_struct* child = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!child) { kfree_page(child_pd); irq_spinlock_release(&scheduler_lock); return 0; }

    uint32_t* kstack = (uint32_t*)kalloc();
    if (!kstack) { kfree_heap(child); kfree_page(child_pd); irq_spinlock_release(&scheduler_lock); return 0; }

    uint32_t* ustack_phys = (uint32_t*)kalloc();
    if (!ustack_phys) { kfree_page(kstack); kfree_heap(child); kfree_page(child_pd); irq_spinlock_release(&scheduler_lock); return 0; }

    child->pid            = next_pid++;
    child->state          = TASK_READY;
    child->is_kernel      = 0;
    child->stack_base     = kstack;
    child->ustack_phys    = ustack_phys;
    child->ustack_virt    = parent->ustack_virt;
    child->page_directory = child_pd;
    child->pending_signals= 0;
    child->queue_next     = 0;
    child->dyn_ctx        = 0;
    child->sigreturn_trampoline = parent->sigreturn_trampoline;
    child->parent_pid     = parent->pid;
    child->exit_code      = 0;
    child->wait_for_pid   = 0;
    child->brk_start      = parent->brk_start;
    child->brk_current    = parent->brk_current;
    child->sleep_until    = 0;
    for (int i = 0; i < NSIG; i++) child->signal_handlers[i] = parent->signal_handlers[i];
    proc_tracker_init(&child->mm);
    mmap_table_init(&child->mmap_table);

    if (parent->page_directory) {
        vmm_fork_address_space(parent->page_directory, child_pd);

        // restore parent user stack to RW — fork already copies stack physically,
        // so CoW on the stack page is unnecessary and causes faults after waitpid
        uint32_t stack_va = parent->ustack_virt;
        uint32_t pdi = PD_INDEX(stack_va);
        uint32_t pti = PT_INDEX(stack_va);
        if (parent->page_directory[pdi] & PAGE_PRESENT) {
            uint32_t* pt = (uint32_t*)(parent->page_directory[pdi] & ~0xFFFu);
            if (pt[pti] & PAGE_PRESENT) {
                pt[pti] = (pt[pti] | PAGE_RW) & ~(uint32_t)PAGE_COW;
            }
        }
        // flush TLB for stack page
        __asm__ __volatile__("invlpg (%0)" :: "r"(stack_va) : "memory");
    }

    vmm_map(child_pd, child->ustack_virt, (uint32_t)ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);
    memory_copy(ustack_phys, parent->ustack_phys, PAGE_SIZE);

    for (int i = 0; i < MAX_FD; i++) {
        child->fd_table[i] = parent->fd_table[i];
        child->fd_offset[i] = parent->fd_offset[i];
        if (child->fd_table[i])
            open_vfs(child->fd_table[i]);
    }

    uint32_t* esp = (uint32_t*)((uint32_t)kstack + PAGE_SIZE);
    *(--esp) = regs->ss;
    *(--esp) = regs->useresp;
    *(--esp) = regs->eflags;
    *(--esp) = regs->cs;
    *(--esp) = regs->eip;
    *(--esp) = 0;           // eax = 0 for child
    *(--esp) = regs->ecx;
    *(--esp) = regs->edx;
    *(--esp) = regs->ebx;
    *(--esp) = 0;           // esp_dummy
    *(--esp) = regs->ebp;
    *(--esp) = regs->esi;
    *(--esp) = regs->edi;
    *(--esp) = regs->ds;
    *(--esp) = regs->es;

    child->esp = (uint32_t)esp;


    task_list_add(child);
    sched_queue_push(&ready_queue, child);
    irq_spinlock_release(&scheduler_lock);

    return child;
}


#ifndef KERNEL_BASE
#define KERNEL_BASE 0xC0000000U
#endif

int task_exec(char* path, struct context_frame* regs) {
    (void)regs;

    if (!path || (uint32_t)path >= KERNEL_BASE) {
        return -1;
    }

    struct task_struct* t = current_task;
    if (!t || t->is_kernel) return -1;

    irq_spinlock_acquire(&scheduler_lock);

    if (t->dyn_ctx) {
        dynlink_unload_all(t->dyn_ctx);
        kfree_heap(t->dyn_ctx);
        t->dyn_ctx = 0;
    }

    uint32_t* new_pd = vmm_create_address_space();
    if (!new_pd) { irq_spinlock_release(&scheduler_lock); return -1; }

    proc_page_tracker_t new_mm;
    proc_tracker_init(&new_mm);

    irq_spinlock_release(&scheduler_lock);

    void* entry = load_elf(path, new_pd, &new_mm);
    if (!entry) {
        kfree_page(new_pd);
        return -1;
    }

    irq_spinlock_acquire(&scheduler_lock);

    if (t->page_directory) {
        proc_free_pages(&t->mm);
        vmm_free_address_space(t->page_directory);
    }

    t->mm = new_mm;
    t->page_directory = new_pd;
    t->pending_signals = 0;
    for (int i = 0; i < NSIG; i++) t->signal_handlers[i] = SIG_DFL;

    mmap_table_init(&t->mmap_table);

    for (int i = 3; i < MAX_FD; i++) {
        if (t->fd_table[i]) {
            close_vfs(t->fd_table[i]);
            t->fd_table[i] = 0;
            t->fd_offset[i] = 0;
        }
    }

    vmm_map(new_pd, t->ustack_virt, (uint32_t)t->ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);

    uint8_t* us = (uint8_t*)t->ustack_phys;
    for (uint32_t i = 0; i < PAGE_SIZE; i++) us[i] = 0;

    // set up brk at end of loaded segments (page-aligned)
    // scan ELF phdrs to find highest vaddr
    {
        extern void* load_elf(char*, uint32_t*, proc_page_tracker_t*);
        // re-read ELF header to find segment end for brk
        vfs_node_t* file = vfs_walk_path(vfs_root, path);
        if (file) {
            extern uint32_t elf_get_brk_start(vfs_node_t* f);
            uint32_t brk = elf_get_brk_start(file);
            t->brk_start   = brk;
            t->brk_current = brk;
        }
    }

    task_setup_sigreturn(t);

    tss_entry.esp0 = (uint32_t)t->stack_base + 4096;

    // push argc/argv onto user stack via physical pointer (before switch_paging)
    uint32_t ustack_top = t->ustack_virt + PAGE_SIZE;
    uint32_t* phys_base = (uint32_t*)t->ustack_phys;

    uint32_t sp = ustack_top - 4;

    // argv[0] = NULL
    sp -= 4;
    *(uint32_t*)((uint8_t*)phys_base + (sp - t->ustack_virt)) = 0;
    uint32_t argv_vaddr = sp;

    // push argv pointer
    sp -= 4;
    *(uint32_t*)((uint8_t*)phys_base + (sp - t->ustack_virt)) = argv_vaddr;

    // push argc
    sp -= 4;
    *(uint32_t*)((uint8_t*)phys_base + (sp - t->ustack_virt)) = 0;

    kprint("[EXEC] pid=");
    char buf[16]; itoa((int)t->pid, buf); kprint(buf);
    kprint(" entry="); hex_to_ascii((uint32_t)entry, buf); kprint(buf);
    kprint(" brk="); hex_to_ascii(t->brk_start, buf); kprint(buf);
    kprint(" sp="); hex_to_ascii(sp, buf); kprint(buf);
    kprint("\n");

    irq_spinlock_release(&scheduler_lock);

    switch_paging(new_pd);

    __asm__ __volatile__(
        "mov $0x23, %%eax\n\t"
        "mov %%eax, %%ds\n\t"
        "mov %%eax, %%es\n\t"
        "pushl $0x23\n\t"
        "pushl %0\n\t"
        "pushf\n\t"
        "orl $0x200, (%%esp)\n\t"
        "pushl $0x1B\n\t"
        "pushl %1\n\t"
        "iret\n\t"
        :
        : "r"(sp), "r"((uint32_t)entry)
        : "eax"
    );

    return 0;
}