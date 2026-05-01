#ifndef SC_HELPER_H
#define SC_HELPER_H

#include "kernel.h"
#include "vfs.h"
#include "task.h"
#include "mod.h"

extern uint32_t timer_ticks_get(void);
extern void schedule(void);

int  alloc_fd(vfs_node_t* node);
int  fd_read_ready(vfs_node_t* node);
int  fd_write_ready(vfs_node_t* node);
void deliver_pending_signal(struct task_struct* t, struct syscall_frame* regs);
void _fill_stat(struct vfs_node* node, uint32_t* ubuf);
uint32_t _vfs_type_to_mode(uint32_t type);
void _kstrcpy(char* dst, const char* src, int max);

#endif /* SC_HELPER_H */
