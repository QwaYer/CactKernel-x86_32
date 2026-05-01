#ifndef SC_FD_H
#define SC_FD_H

#include "kernel.h"
#include "task.h"
#include "vfs.h"
#include "mod.h"

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#ifndef O_NONBLOCK
#define O_NONBLOCK 0x0800
#endif

#define SEL_SETSIZE  256
typedef struct { uint32_t w[SEL_SETSIZE / 32]; } sel_fdset_t;

#define SEL_ISSET(fd, s)  (((s)->w[(fd)/32] >> ((fd) % 32)) & 1u)
#define SEL_SET(fd, s)    ((s)->w[(fd)/32] |=  (1u << ((fd) % 32)))
#define SEL_ZERO(s) \
    do { for (int _i = 0; _i < SEL_SETSIZE/32; _i++) (s)->w[_i] = 0; } while (0)

struct timeval_fd {   /* локальный псевдоним для select */
    long tv_sec;
    long tv_usec;
};

typedef struct {
    int              nfds;
    sel_fdset_t*     readfds;
    sel_fdset_t*     writefds;
    sel_fdset_t*     exceptfds;
    struct timeval_fd* timeout;
} sel_args_t;

#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

struct pollfd {
    int   fd;
    short events;
    short revents;
};

int sys_open(char* name, int flags);
int sys_read(int fd, char* buf, unsigned int size);
int sys_write(int fd, char* buf, unsigned int size);
int sys_close(int fd);
int sys_lseek(struct syscall_frame* regs);
int sys_ioctl(struct syscall_frame* regs);
int sys_fcntl(int fd, int cmd, int arg);
int sys_dup(int oldfd);
int sys_dup2(struct syscall_frame* regs);
int sys_pipe(struct syscall_frame* regs);
int sys_select(struct syscall_frame* regs);
int sys_poll(struct syscall_frame* regs);

#endif /* SC_FD_H */
