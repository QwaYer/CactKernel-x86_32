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
spinlock_t          scheduler_lock;

sched_queue_t ready_queue;
sched_queue_t sleep_queue;
sched_queue_t zombie_queue;

static volatile int schedule_in_progress = 0; // reentrancy guard

extern void vmm_fork_address_space(uint32_t* src_pd, uint32_t* dst_pd);


static sched_queue_t* queue_for_state(task_state s) {
    switch (s) {
        case TASK_READY:    return &ready_queue;
        case TASK_SLEEPING: return &sleep_queue;
        case TASK_ZOMBIE:   return &zombie_queue;
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
    spinlock_init(&scheduler_lock);
    sched_queue_init(&ready_queue);
    sched_queue_init(&sleep_queue);
    sched_queue_init(&zombie_queue);
    kprint("[SCHED] task_init done\n"); // [DBG]
}


struct task_struct* create_task(void (*entry_point)()) {
    struct task_struct* t = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!t) return 0;

    uint32_t* stack = (uint32_t*)kalloc();
    if (!stack) { kfree_heap(t); return 0; }

    uint32_t* esp = (uint32_t*)((uint32_t)stack + 4096);

    // iret frame for kernel task: eflags, cs, eip
    *(--esp) = 0x00000202;           // eflags (IF=1)
    *(--esp) = 0x08;                 // cs
    *(--esp) = (uint32_t)entry_point;// eip
    // pusha: eax ecx edx ebx esp ebp esi edi
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    // ds, es
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
    proc_tracker_init(&t->mm);

    // [DBG]
    kprint("[SCHED] create_task pid=");
    char buf[16]; itoa((int)t->pid, buf); kprint(buf); kprint("\n");

    spinlock_acquire(&scheduler_lock);
    task_list_add(t);
    sched_queue_push(&ready_queue, t);
    spinlock_release(&scheduler_lock);
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

    // iret frame for ring3: ss, useresp, eflags, cs, eip
    *(--esp) = 0x23;                          // ss (user data)
    *(--esp) = ustack_virt + 4096 - 4;       // useresp
    *(--esp) = 0x00000202;                    // eflags (IF=1)
    *(--esp) = 0x1B;                          // cs (user code)
    *(--esp) = (uint32_t)entry_point;         // eip
    // pusha order: eax ecx edx ebx esp ebp esi edi (8 regs)
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    // ds, es
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
    for (int i = 0; i < NSIG; i++) t->signal_handlers[i] = SIG_DFL;
    proc_tracker_init(&t->mm);
    for (int i = 0; i < MAX_FD; i++) t->fd_table[i] = 0;

    if (add_to_list) {
        spinlock_acquire(&scheduler_lock);
        task_list_add(t);
        sched_queue_push(&ready_queue, t);
        spinlock_release(&scheduler_lock);
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
    __asm__ __volatile__("cli");

    struct task_struct* t = create_user_task_internal(entry, 0);
    if (!t) { __asm__ __volatile__("sti"); return 0; }

    t->mm = *tracker;
    // tracker ownership transferred — caller must not free

    t->page_directory = pd;

    // fix eip in the stack frame — entry is at offset 10 from bottom
    // stack layout (bottom to top): es ds edi esi ebp esp_dummy ebx edx ecx eax eip cs eflags useresp ss
    // esp points at es, so eip = esp[10]
    uint32_t* stk = (uint32_t*)t->esp;
    stk[10] = (uint32_t)entry;

    // [DBG]
    kprint("[SCHED] create_task_with_entry pid=");
    char buf[16]; itoa((int)t->pid, buf); kprint(buf);
    kprint(" entry=0x"); kprint_hex((uint32_t)entry); kprint("\n");

    vmm_map(pd, t->ustack_virt, (uint32_t)t->ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);

    tss_entry.esp0 = (uint32_t)t->stack_base + 4096;

    task_setup_sigreturn(t);

    spinlock_acquire(&scheduler_lock);
    task_list_add(t);
    sched_queue_push(&ready_queue, t);
    spinlock_release(&scheduler_lock);

    __asm__ __volatile__("sti");
    return t;
}

struct task_struct* create_task_dynamic(void*                entry,
                                         uint32_t*            pd,
                                         proc_page_tracker_t* tracker,
                                         dyn_ctx_t*           ctx)
{
    struct task_struct* t = create_task_with_entry(entry, pd, tracker);
    if (!t) return 0;

    spinlock_acquire(&scheduler_lock);
    t->dyn_ctx = ctx;
    spinlock_release(&scheduler_lock);

    return t;
}


struct task_struct* create_elf_task(char* path) {
    __asm__ __volatile__("cli");

    uint32_t* pd = vmm_create_address_space();
    if (!pd) { __asm__ __volatile__("sti"); return 0; }

    struct task_struct* t = create_user_task_internal((void*)0, 0);
    if (!t) { kfree_page(pd); __asm__ __volatile__("sti"); return 0; }

    proc_tracker_init(&t->mm);
    void* entry_point = load_elf(path, pd, &t->mm);
    if (!entry_point) {
        kprint("[SCHED] ELF load failed for: "); kprint(path); kprint("\n"); // [DBG]
        kfree_page(t->stack_base);
        kfree_page(t->ustack_phys);
        kfree_heap(t);
        kfree_page(pd);
        __asm__ __volatile__("sti");
        return 0;
    }

    t->page_directory = pd;
    t->dyn_ctx        = 0;

    uint32_t* stk = (uint32_t*)t->esp;
    stk[10] = (uint32_t)entry_point;

    vmm_map(pd, t->ustack_virt, (uint32_t)t->ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);

    task_setup_sigreturn(t);

    tss_entry.esp0 = (uint32_t)t->stack_base + 4096;

    spinlock_acquire(&scheduler_lock);
    task_list_add(t);
    sched_queue_push(&ready_queue, t);
    spinlock_release(&scheduler_lock);

    __asm__ __volatile__("sti");
    return t;
}


int init_scheduler() {
    current_task = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!current_task) {
        kprint("[SCHED] FATAL: cannot alloc idle task\n"); // [DBG]
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
    proc_tracker_init(&current_task->mm);
    task_list_head = current_task;

    kprint("[SCHED] init_scheduler: idle pid=0 ok\n"); // [DBG]

    create_task(terminal_task);
    return 0;
}

static void task_reap_internal() {
    struct task_struct* t;
    while ((t = sched_queue_pop(&zombie_queue)) != 0) {
        // [DBG]
        kprint("[SCHED] reaping pid=");
        char buf[16]; itoa((int)t->pid, buf); kprint(buf); kprint("\n");

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
    spinlock_acquire(&scheduler_lock);
    task_reap_internal();
    spinlock_release(&scheduler_lock);
}

// public api
void schedule() {
    // reentrancy guard — if timer fires while we're already in schedule(), bail
    if (__sync_lock_test_and_set(&schedule_in_progress, 1)) {
        return;
    }

    spinlock_acquire(&scheduler_lock);

    if (!task_list_head || !current_task) {
        spinlock_release(&scheduler_lock);
        __sync_lock_release(&schedule_in_progress);
        return;
    }

    task_reap_internal();
    task_handle_signals(current_task);

    if (current_task->state == TASK_ZOMBIE) {
        sched_queue_push(&zombie_queue, current_task);
        struct task_struct* next = sched_queue_pop(&ready_queue);
        if (!next) {
            // CRITICAL: no runnable task — we CANNOT continue as zombie
            // Force state back to RUNNING so we at least don't crash
            // The idle loop (pid 0) should always be in the list
            kprint_color("[SCHED] WARN: zombie but no ready task!\n", 12); // [DBG]
            current_task->state = TASK_RUNNING;
            sched_queue_pop(&zombie_queue); // remove ourselves back
            spinlock_release(&scheduler_lock);
            __sync_lock_release(&schedule_in_progress);
            return;
        }
        current_task = next;
        current_task->state = TASK_RUNNING;
        goto do_switch;
    }

    if (current_task->state == TASK_SLEEPING) {
        sched_queue_push(&sleep_queue, current_task);
        struct task_struct* next = sched_queue_pop(&ready_queue);
        if (!next) {
            // no runnable task — stay current (sleeping task becomes running again)
            kprint_color("[SCHED] WARN: sleeping but no ready task!\n", 14); // [DBG]
            sched_queue_remove(&sleep_queue, current_task);
            current_task->state = TASK_RUNNING;
            spinlock_release(&scheduler_lock);
            __sync_lock_release(&schedule_in_progress);
            return;
        }
        current_task = next;
        current_task->state = TASK_RUNNING;
        goto do_switch;
    }

    {
        struct task_struct* next = sched_queue_pop(&ready_queue);
        if (!next) {
            // only one task (us) — no switch needed
            spinlock_release(&scheduler_lock);
            __sync_lock_release(&schedule_in_progress);
            return;
        }
        current_task->state = TASK_READY;
        sched_queue_push(&ready_queue, current_task);
        current_task = next;
        current_task->state = TASK_RUNNING;
    }

do_switch:
    if (current_task->page_directory)
        switch_paging(current_task->page_directory);
    else {
        extern uint32_t page_directory[1024];
        switch_paging(page_directory);
    }
    tss_entry.esp0 = (uint32_t)current_task->stack_base + 4096;
    spinlock_release(&scheduler_lock);
    __sync_lock_release(&schedule_in_progress);
}


void list_tasks() {
    spinlock_acquire(&scheduler_lock);
    if (!task_list_head) { spinlock_release(&scheduler_lock); return; }

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
    spinlock_release(&scheduler_lock);

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
            default:            kprint("sleeping  "); break;
        }
        kprint(tasks[i].is_kernel ? "kernel\n" : "user\n");
    }
}

void task_kill(uint32_t pid) {
    if (pid == 0) {
        kprint("[SCHED] cannot kill pid 0\n"); // [DBG]
        return;
    }
    task_signal(pid, SIGKILL);
}

void task_signal(uint32_t pid, uint32_t signal) {
    spinlock_acquire(&scheduler_lock);
    struct task_struct* t = task_list_head;
    if (!t) { spinlock_release(&scheduler_lock); return; }

    do {
        if (t->pid == pid) {
            t->pending_signals |= signal;

            if ((signal & SIGKILL) && t->state == TASK_SLEEPING) {
                sched_queue_remove(&sleep_queue, t);
                t->state = TASK_READY;
                sched_queue_push(&ready_queue, t);
            }
            break;
        }
        t = t->next;
    } while (t != task_list_head);

    spinlock_release(&scheduler_lock);
}

void task_handle_signals(struct task_struct* t) {
    if (!t || !t->pending_signals) return;

    if (t->pending_signals & SIGKILL) {
        t->pending_signals = 0;
        t->state = TASK_ZOMBIE;
        // [DBG]
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
    if (signum == 0) return -1; // SIGKILL cannot be caught
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

    // mov eax, 119 (sigreturn syscall); int 0x80; hlt
    p[0] = 0xB8; p[1] = 0x77; p[2] = 0x00; p[3] = 0x00; p[4] = 0x00;
    p[5] = 0xCD; p[6] = 0x80;
    p[7] = 0xF4;

    vmm_map(t->page_directory, tramp_vaddr, (uint32_t)phys,
            PAGE_USER | PAGE_PRESENT);
    t->sigreturn_trampoline = tramp_vaddr;
}


struct task_struct* task_fork(struct context_frame* regs) {
    __asm__ __volatile__("cli");

    struct task_struct* parent = (struct task_struct*)current_task;
    if (!parent) { __asm__ __volatile__("sti"); return 0; }

    uint32_t* child_pd = vmm_create_address_space();
    if (!child_pd) { __asm__ __volatile__("sti"); return 0; }

    struct task_struct* child = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!child) { kfree_page(child_pd); __asm__ __volatile__("sti"); return 0; }

    uint32_t* kstack = (uint32_t*)kalloc();
    if (!kstack) { kfree_heap(child); kfree_page(child_pd); __asm__ __volatile__("sti"); return 0; }

    uint32_t* ustack_phys = (uint32_t*)kalloc();
    if (!ustack_phys) { kfree_page(kstack); kfree_heap(child); kfree_page(child_pd); __asm__ __volatile__("sti"); return 0; }

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
    for (int i = 0; i < NSIG; i++) child->signal_handlers[i] = parent->signal_handlers[i];
    proc_tracker_init(&child->mm);
    mmap_table_init(&child->mmap_table);

    if (parent->page_directory) {
        vmm_fork_address_space(parent->page_directory, child_pd);
    }

    vmm_map(child_pd, child->ustack_virt, (uint32_t)ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);
    memory_copy(ustack_phys, parent->ustack_phys, PAGE_SIZE);

    for (int i = 0; i < MAX_FD; i++) {
        child->fd_table[i] = parent->fd_table[i];
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

    // [DBG]
    kprint("[SCHED] fork: child pid=");
    char buf[16]; itoa((int)child->pid, buf); kprint(buf); kprint("\n");

    spinlock_acquire(&scheduler_lock);
    task_list_add(child);
    sched_queue_push(&ready_queue, child);
    spinlock_release(&scheduler_lock);

    __asm__ __volatile__("sti");
    return child;
}


#ifndef KERNEL_BASE
#define KERNEL_BASE 0xC0000000U
#endif

int task_exec(char* path, struct context_frame* regs) {
    __asm__ __volatile__("cli");
    (void)regs;

    if (!path || (uint32_t)path >= KERNEL_BASE) {
        __asm__ __volatile__("sti");
        return -1;
    }

    struct task_struct* t = current_task;
    if (!t || t->is_kernel) { __asm__ __volatile__("sti"); return -1; }

    if (t->dyn_ctx) {
        dynlink_unload_all(t->dyn_ctx);
        kfree_heap(t->dyn_ctx);
        t->dyn_ctx = 0;
    }

    uint32_t* new_pd = vmm_create_address_space();
    if (!new_pd) { __asm__ __volatile__("sti"); return -1; }

    proc_page_tracker_t new_mm;
    proc_tracker_init(&new_mm);
    void* entry = load_elf(path, new_pd, &new_mm);
    if (!entry) {
        kfree_page(new_pd);
        __asm__ __volatile__("sti");
        return -1;
    }

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
        }
    }

    vmm_map(new_pd, t->ustack_virt, (uint32_t)t->ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);

    uint8_t* us = (uint8_t*)t->ustack_phys;
    for (uint32_t i = 0; i < PAGE_SIZE; i++) us[i] = 0;

    task_setup_sigreturn(t);

    tss_entry.esp0 = (uint32_t)t->stack_base + 4096;

    switch_paging(new_pd);

    // [DBG]
    kprint("[SCHED] exec pid=");
    char buf[16]; itoa((int)t->pid, buf); kprint(buf);
    kprint(" -> "); kprint(path); kprint("\n");

    __asm__ __volatile__("sti");

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
        : "r"(t->ustack_virt + PAGE_SIZE - 4), "r"((uint32_t)entry)
        : "eax"
    );

    return 0; // never reached
}