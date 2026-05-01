#ifndef SC_NET_H
#define SC_NET_H

#include "kernel.h"
#include "task.h"
#include "vfs.h"
#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "mod.h"
/* Ядерный net.h (ntohs/htonl/MY_IP) включается в net.c через <net.h>,
 * чтобы избежать именного конфликта с этим файлом (net/net.h). */

typedef struct {
    int         fd;
    int         level;
    int         optname;
    const void* optval;
    uint32_t    optlen;
} setsockopt_args_t;

typedef struct {
    int       fd;
    int       level;
    int       optname;
    void*     optval;
    uint32_t* optlen;
} getsockopt_args_t;

int sys_socket(struct syscall_frame* regs);
int sys_bind(struct syscall_frame* regs);
int sys_connect(struct syscall_frame* regs);
int sys_listen(struct syscall_frame* regs);
int sys_accept(struct syscall_frame* regs);
int sys_send(struct syscall_frame* regs);
int sys_recv(struct syscall_frame* regs);
int sys_sendto(struct syscall_frame* regs);
int sys_recvfrom(struct syscall_frame* regs);
int sys_shutdown(struct syscall_frame* regs);
int sys_setsockopt(struct syscall_frame* regs);
int sys_getsockopt(struct syscall_frame* regs);

#endif /* SC_NET_H */
