#ifndef SC_FILE_H
#define SC_FILE_H

#include "kernel.h"
#include "task.h"
#include "vfs.h"
#include "mod.h"

int sys_stat(struct syscall_frame* regs);
int sys_fstat(struct syscall_frame* regs);
int sys_access(char* path, int mode);
int sys_chmod(char* path, uint32_t mode);
int sys_chown(char* path, uint32_t new_uid, uint32_t new_gid);
int sys_umask(uint32_t mask);
int sys_truncate(char* path, uint32_t length);
int sys_ftruncate(int fd, uint32_t length);
int sys_sync(void);
int sys_fsync(int fd);
int sys_mknod(char* path, uint32_t mode, uint32_t dev);

#endif /* SC_FILE_H */
