#include "mm.h"
#include "validate.h"
#include "task.h"

// Heap growth cap.  brk() grows eagerly -- every page is kalloc'd and zeroed
// here, there is no demand paging -- and the window between the ELF image and
// MMAP_BASE is shared with mmap, so the break is deliberately bounded well
// below USER_HEAP_LIMIT.  Do not raise this without first making brk
// demand-faulted.
#define USER_BRK_MAX_GROWTH (16u * 1024u * 1024u)

// The calling task's thread-group state.  brk bounds and the page tracker are
// process-wide (all threads of a group allocate into one heap), so every brk /
// mprotect path goes through here instead of the per-task ProcMeta copies.
static proc_shared_t *cur_shared(void) {
    if (!current_task || !current_task->proc) return 0;
    return current_task->proc->shared;
}

// brk() — change the program break (heap end)
int sys_brk(struct syscall_frame* regs) {
    uint32_t new_brk = regs->ebx;
    if (!current_task) return -1;

    proc_shared_t *ps = cur_shared();
    if (!ps) return -1;

    // brk(0) returns the current break without changing it
    if (new_brk == 0)
        return (int)ps->brk_current;

    // Must not go below the initial break
    if (new_brk < ps->brk_start)
        return -1;

    // Safety limit: see USER_BRK_MAX_GROWTH.
    if (new_brk - ps->brk_start > USER_BRK_MAX_GROWTH)
        return -1;

    uint32_t old_end = (ps->brk_current + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t new_end = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    // Grow heap: allocate and zero new pages
    if (new_end > old_end) {
        for (uint32_t va = old_end; va < new_end; va += PAGE_SIZE) {
            void* phys = kalloc();
            if (!phys) return -1;
            uint8_t* p = (uint8_t*)phys;
            for (int i = 0; i < (int)PAGE_SIZE; i++) p[i] = 0;
            vmm_map(current_task->page_directory, va, (uint32_t)phys,
                    PAGE_USER | PAGE_RW | PAGE_PRESENT);
            proc_tracker_add(&ps->mm, phys);   // track for cleanup on exit
        }
    }

    ps->brk_current = new_brk;
    return (int)new_brk;
}

// mmap() — map files or anonymous memory into the process address space
int sys_mmap(struct syscall_frame* regs) {
    mmap_args_t* args = (mmap_args_t*)regs->ebx;
    if (!validate_user_ptr(args, sizeof(mmap_args_t))) return (int)MAP_FAILED;
    if (!current_task) return (int)MAP_FAILED;

    mmap_args_t args_buf;
    if (copy_from_user(&args_buf, args, sizeof(args_buf)) != 0) return (int)MAP_FAILED;

    void* result = do_mmap(
        current_task->page_directory,
        current_task->proc->mmap_table,
        args_buf.addr,
        args_buf.length,
        args_buf.prot,
        args_buf.flags,
        args_buf.fd,
        args_buf.offset
    );
    return (int)result;
}

// munmap() — unmap a previously mapped region
int sys_munmap(struct syscall_frame* regs) {
    uint32_t addr   = regs->ebx;
    uint32_t length = regs->ecx;
    if (!current_task) return -1;
    return do_munmap(
        current_task->page_directory,
        current_task->proc->mmap_table,
        addr, length
    );
}

// mprotect() — change protection flags on a mapped region
int sys_mprotect(struct syscall_frame* regs) {
    uint32_t addr   = regs->ebx;
    uint32_t length = regs->ecx;
    int      prot   = (int)regs->edx;
    if (!current_task) return -1;
    proc_shared_t *ps = cur_shared();
    if (!ps) return -1;
    uint32_t brk_end = (ps->brk_current + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    return do_mprotect(
        current_task->page_directory,
        current_task->proc->mmap_table,
        addr, length, prot,
        ps->brk_start, brk_end
    );
}