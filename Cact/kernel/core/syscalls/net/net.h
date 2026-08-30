#ifndef SC_NET_H
#define SC_NET_H

// Kernel-internal net.h defines skb_t before tcp.h/udp.h/socket.h use it.
#include <net.h>

#include "kernel.h"
#include "task.h"
#include "vfs.h"
#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "mod.h"

// setsockopt/getsockopt argument structs
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

typedef struct {
    uint32_t ip_host;
    uint32_t netmask_host;
    uint32_t gateway_host;
    uint32_t dns_host;
    uint32_t dhcp_server_host;
    uint32_t lease_s;
    uint32_t t1_s;
    uint32_t t2_s;
} netcfg_args_t;

// Socket syscalls
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
int sys_ping_echo(struct syscall_frame* regs);
int sys_netcfg_set(struct syscall_frame* regs);
int sys_dns_resolve(struct syscall_frame* regs);

#endif 