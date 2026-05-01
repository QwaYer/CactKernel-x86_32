#include "mm.h"
#include "validate.h"

int sys_brk(struct syscall_frame* regs) {
    uint32_t new_brk = regs->ebx;
    if (!current_task) return -1;

    if (new_brk == 0)
        return (int)current_task->brk_current;

    if (new_brk < current_task->brk_start)
        return -1;

    if (new_brk - current_task->brk_start > 16 * 1024 * 1024)
        return -1;

    uint32_t old_end = (current_task->brk_current + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t new_end = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (new_end > old_end) {
        for (uint32_t va = old_end; va < new_end; va += PAGE_SIZE) {
            void* phys = kalloc();
            if (!phys) return -1;
            uint8_t* p = (uint8_t*)phys;
            for (int i = 0; i < (int)PAGE_SIZE; i++) p[i] = 0;
            vmm_map(current_task->page_directory, va, (uint32_t)phys,
                    PAGE_USER | PAGE_RW | PAGE_PRESENT);
            proc_tracker_add(&current_task->mm, phys);
        }
    }

    current_task->brk_current = new_brk;
    return (int)new_brk;
}

int sys_mmap(struct syscall_frame* regs) {
    mmap_args_t* args = (mmap_args_t*)regs->ebx;
    if (!validate_user_ptr(args, sizeof(mmap_args_t))) return (int)MAP_FAILED;
    if (!current_task) return (int)MAP_FAILED;
    void* result = do_mmap(
        current_task->page_directory,
        current_task->mmap_table,
        args->addr,
        args->length,
        args->prot,
        args->flags,
        args->fd,
        args->offset
    );
    return (int)result;
}

int sys_munmap(struct syscall_frame* regs) {
    uint32_t addr   = regs->ebx;
    uint32_t length = regs->ecx;
    if (!current_task) return -1;
    return do_munmap(
        current_task->page_directory,
        current_task->mmap_table,
        addr, length
    );
}

int sys_mprotect(struct syscall_frame* regs) {
    uint32_t addr   = regs->ebx;
    uint32_t length = regs->ecx;
    int      prot   = (int)regs->edx;
    if (!current_task) return -1;
    return do_mprotect(
        current_task->page_directory,
        current_task->mmap_table,
        addr, length, prot
    );
}
