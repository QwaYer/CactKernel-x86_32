#ifndef SC_PATH_H
#define SC_PATH_H

#include "kernel.h"
#include "task.h"
#include "vfs.h"
#include "mod.h"

// Directory entry structure for getdents()
struct cact_dirent {
    uint32_t d_ino;
    char     d_name[124];
};

// Path manipulation syscalls
int sys_create(char* name);
int sys_mkdir(char* pathname);
int sys_rmdir(char* pathname);
int sys_delete(char* name);
int sys_unlink(char* path);
int sys_rename(char* oldpath, char* newpath);
int sys_link(struct syscall_frame* regs);
int sys_symlink(struct syscall_frame* regs);
int sys_readlink(struct syscall_frame* regs);
int sys_getdents(struct syscall_frame* regs);
int sys_chdir(struct syscall_frame* regs);
int sys_getcwd(struct syscall_frame* regs);
int sys_chroot(char* path);

#endif /* SC_PATH_H */