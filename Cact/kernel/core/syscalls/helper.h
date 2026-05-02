#ifndef SC_HELPER_H
#define SC_HELPER_H

#include "kernel.h"
#include "vfs.h"
#include "task.h"
#include "mod.h"

// External references
extern uint32_t timer_ticks_get(void);
extern void schedule(void);

// Allocate a free fd (3..MAX_FD-1) and install the given VFS node
int  alloc_fd(vfs_node_t* node);

// Non-blocking readiness checks for select/poll
int  fd_read_ready(vfs_node_t* node);
int  fd_write_ready(vfs_node_t* node);

// Deliver one pending signal to the task before returning to userspace
void deliver_pending_signal(struct task_struct* t, struct syscall_frame* regs);

// Stat helpers
void     _fill_stat(struct vfs_node* node, uint32_t* ubuf);
uint32_t _vfs_type_to_mode(uint32_t type);

// Bounded string copy (always null-terminates within 'max' bytes)
void _kstrcpy(char* dst, const char* src, int max);

#endif 