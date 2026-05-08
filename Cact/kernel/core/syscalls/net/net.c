#include "net.h"
#include <net.h>       // kernel-internal ntohs/htonl/ntohl/htons/MY_IP
#include "validate.h"
#include "helper.h"
#include "rust_net_ffi.h"

// socket() — create a new socket, allocate an fd for it
int sys_socket(struct syscall_frame* regs) {
    int domain   = (int)regs->ebx;
    int type     = (int)regs->ecx;
    int protocol = (int)regs->edx;
    if (!current_task) return -1;

    vfs_node_t* node = ksock_create(domain, type, protocol);
    if (!node) return -1;

    int fd = alloc_fd(node);   // find free fd, increment VFS refcount
    if (fd < 0) {
        close_vfs(node);
        return -1;
    }
    return fd;
}

// bind() — assign a local address and port to a socket
int sys_bind(struct syscall_frame* regs) {
    int fd = (int)regs->ebx;
    struct sockaddr_in* addr = (struct sockaddr_in*)regs->ecx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(addr, sizeof(struct sockaddr_in))) return -1;

    vfs_node_t* node = current_task->fds->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t* ks = ksock_from_node(node);
    if (!ks) return -1;

    uint16_t port = ntohs(addr->sin_port);

    if (ks->kind == KS_TCP) {
        tcp_socket_t* s = 0;
        if (ks->proto_idx >= 0 && ks->proto_idx < TCP_MAX_SOCKETS)
            s = &tcp_sockets[ks->proto_idx];
        if (!s) return -1;
        s->local_port = port;
        s->local_ip   = htonl(rust_net_get_ip_host());
        return 0;
    }

    if (ks->kind == KS_UDP) {
        udp_sock_t* s = udp_sock_find_by_port(port);
        if (s && s != &udp_socks[ks->proto_idx] && !ks->so_reuseaddr) return -1;
        udp_socks[ks->proto_idx].local_port = port;
        udp_socks[ks->proto_idx].local_ip   = ntohl(addr->sin_addr);
        return 0;
    }

    return -1;
}

// connect() — initiate a TCP connection to a remote address
int sys_connect(struct syscall_frame* regs) {
    int fd = (int)regs->ebx;
    struct sockaddr_in* addr = (struct sockaddr_in*)regs->ecx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(addr, sizeof(struct sockaddr_in))) return -1;

    vfs_node_t* node = current_task->fds->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t* ks = ksock_from_node(node);
    if (!ks || ks->kind != KS_TCP) return -1;

    uint32_t dst_ip   = addr->sin_addr;
    uint16_t dst_port = ntohs(addr->sin_port);
    return tcp_connect(ks->proto_idx, dst_ip, dst_port);
}

// listen() — mark a TCP socket as passive, ready to accept connections
int sys_listen(struct syscall_frame* regs) {
    int fd = (int)regs->ebx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;

    vfs_node_t* node = current_task->fds->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t* ks = ksock_from_node(node);
    if (!ks || ks->kind != KS_TCP) return -1;

    extern tcp_socket_t tcp_sockets[];
    tcp_socket_t* s = &tcp_sockets[ks->proto_idx];
    return tcp_listen(ks->proto_idx, s->local_port);
}

// accept() — accept an incoming TCP connection, return a new fd
int sys_accept(struct syscall_frame* regs) {
    int fd = (int)regs->ebx;
    struct sockaddr_in* peer_addr    = (struct sockaddr_in*)regs->ecx;
    uint32_t*           peer_addrlen = (uint32_t*)regs->edx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (peer_addr && !validate_user_ptr(peer_addr, sizeof(struct sockaddr_in)))
        return -1;
    if (peer_addrlen && !validate_user_ptr(peer_addrlen, sizeof(uint32_t)))
        return -1;

    vfs_node_t* listen_node = current_task->fds->fd_table[fd];
    if (!listen_node || listen_node->type != VFS_SOCKET) return -1;

    vfs_node_t* conn_node = ksock_tcp_accept(listen_node, peer_addr);
    if (!conn_node) return -1;

    if (peer_addrlen) *peer_addrlen = sizeof(struct sockaddr_in);

    int new_fd = alloc_fd(conn_node);
    if (new_fd < 0) {
        close_vfs(conn_node);
        return -1;
    }
    return new_fd;
}

// send() — send data on a connected socket
int sys_send(struct syscall_frame* regs) {
    int      fd  = (int)regs->ebx;
    char*    buf = (char*)regs->ecx;
    uint32_t len = regs->edx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(buf, len)) return -1;

    vfs_node_t* node = current_task->fds->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    return write_vfs(node, 0, len, buf);
}

// recv() — receive data from a connected socket
int sys_recv(struct syscall_frame* regs) {
    int      fd  = (int)regs->ebx;
    char*    buf = (char*)regs->ecx;
    uint32_t len = regs->edx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(buf, len)) return -1;

    vfs_node_t* node = current_task->fds->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    return read_vfs(node, 0, len, buf);
}

// sendto() — send a UDP datagram to a specific address
int sys_sendto(struct syscall_frame* regs) {
    sendto_args_t* args = (sendto_args_t*)regs->ebx;
    if (!validate_user_ptr(args, sizeof(sendto_args_t))) return -1;
    if (!current_task) return -1;

    int         fd   = args->fd;
    const void* buf  = args->buf;
    uint32_t    len  = args->len;
    const struct sockaddr_in* dest = args->dest;

    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(buf, len)) return -1;
    if (!validate_user_ptr(dest, sizeof(struct sockaddr_in))) return -1;

    vfs_node_t* node = current_task->fds->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t* ks = ksock_from_node(node);
    if (!ks) return -1;

    if (ks->kind == KS_TCP)
        return tcp_send(ks->proto_idx, (uint8_t*)buf, (uint16_t)len);

    if (ks->kind == KS_UDP) {
        uint32_t dst_ip   = dest->sin_addr;
        uint16_t dst_port = ntohs(dest->sin_port);
        return udp_sock_send(ks->proto_idx, dst_ip, dst_port,
                             (const uint8_t*)buf, (uint16_t)len);
    }
    return -1;
}

// recvfrom() — receive a UDP datagram with source address
int sys_recvfrom(struct syscall_frame* regs) {
    recvfrom_args_t* args = (recvfrom_args_t*)regs->ebx;
    if (!validate_user_ptr(args, sizeof(recvfrom_args_t))) return -1;
    if (!current_task) return -1;

    int      fd      = args->fd;
    void*    buf     = args->buf;
    uint32_t len     = args->len;
    struct sockaddr_in* src     = args->src;
    uint32_t*           addrlen = args->addrlen;

    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(buf, len)) return -1;
    if (src     && !validate_user_ptr(src,     sizeof(struct sockaddr_in))) return -1;
    if (addrlen && !validate_user_ptr(addrlen, sizeof(uint32_t)))           return -1;

    vfs_node_t* node = current_task->fds->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t* ks = ksock_from_node(node);
    if (!ks) return -1;

    int ret = -1;
    if (ks->kind == KS_TCP) {
        ret = tcp_recv(ks->proto_idx, (uint8_t*)buf, (uint16_t)len);
        if (ret > 0 && src) {
            tcp_socket_t* s = &tcp_sockets[ks->proto_idx];
            src->sin_family = AF_INET;
            src->sin_port   = htons(s->remote_port);
            src->sin_addr   = s->remote_ip;
            if (addrlen) *addrlen = sizeof(struct sockaddr_in);
        }
    } else if (ks->kind == KS_UDP) {
        uint32_t src_ip   = 0;
        uint16_t src_port = 0;
        ret = udp_sock_recv(ks->proto_idx, (uint8_t*)buf, (uint16_t)len,
                            &src_ip, &src_port);
        if (ret > 0 && src) {
            src->sin_family = AF_INET;
            src->sin_port   = htons(src_port);
            src->sin_addr   = htonl(src_ip);
            if (addrlen) *addrlen = sizeof(struct sockaddr_in);
        }
    }
    return ret;
}

// shutdown() — shut down part or all of a socket connection
int sys_shutdown(struct syscall_frame* regs) {
    int fd  = (int)regs->ebx;
    int how = (int)regs->ecx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;

    vfs_node_t* node = current_task->fds->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    return ksock_shutdown(node, how);
}

// setsockopt() — set socket options
int sys_setsockopt(struct syscall_frame* regs) {
    setsockopt_args_t* args = (setsockopt_args_t*)regs->ebx;
    if (!validate_user_ptr(args, sizeof(setsockopt_args_t))) return -1;
    if (!current_task) return -1;

    int         fd      = args->fd;
    int         level   = args->level;
    int         optname = args->optname;
    const void* optval  = args->optval;
    uint32_t    optlen  = args->optlen;

    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!optval || optlen < sizeof(int)) return -1;
    if (!validate_user_ptr(optval, optlen)) return -1;

    vfs_node_t* node = current_task->fds->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t* ks = ksock_from_node(node);
    if (!ks) return -1;

    int ival = *(const int*)optval;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:
            ks->so_reuseaddr = (uint8_t)!!ival;
            return 0;
        case SO_KEEPALIVE:
            ks->so_keepalive = (uint8_t)!!ival;
            if (ks->kind == KS_TCP && ks->proto_idx >= 0 &&
                ks->proto_idx < TCP_MAX_SOCKETS)
                tcp_sockets[ks->proto_idx].keepalive = ks->so_keepalive;
            return 0;
        case SO_ERROR:
            return -1;
        }
    } else if (level == IPPROTO_TCP) {
        switch (optname) {
        case TCP_NODELAY:
            ks->tcp_nodelay = (uint8_t)!!ival;
            if (ks->kind == KS_TCP && ks->proto_idx >= 0 &&
                ks->proto_idx < TCP_MAX_SOCKETS)
                tcp_sockets[ks->proto_idx].nodelay = ks->tcp_nodelay;
            return 0;
        }
    }
    return -1;
}

// getsockopt() — get socket options
int sys_getsockopt(struct syscall_frame* regs) {
    getsockopt_args_t* args = (getsockopt_args_t*)regs->ebx;
    if (!validate_user_ptr(args, sizeof(getsockopt_args_t))) return -1;
    if (!current_task) return -1;

    int       fd      = args->fd;
    int       level   = args->level;
    int       optname = args->optname;
    void*     optval  = args->optval;
    uint32_t* optlen  = args->optlen;

    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!optval || !optlen) return -1;
    if (!validate_user_ptr(optlen, sizeof(uint32_t))) return -1;
    if (*optlen < sizeof(int)) return -1;
    if (!validate_user_ptr(optval, *optlen)) return -1;

    vfs_node_t* node = current_task->fds->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t* ks = ksock_from_node(node);
    if (!ks) return -1;

    int ival = 0;
    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR: ival = ks->so_reuseaddr; break;
        case SO_KEEPALIVE: ival = ks->so_keepalive; break;
        case SO_ERROR:
            ival = ks->so_error;
            ks->so_error = 0;   // clear after read
            break;
        default: return -1;
        }
    } else if (level == IPPROTO_TCP) {
        switch (optname) {
        case TCP_NODELAY: ival = ks->tcp_nodelay; break;
        default: return -1;
        }
    } else {
        return -1;
    }

    *(int*)optval = ival;
    *optlen = sizeof(int);
    return 0;
}

// ping_echo() — send one ICMP echo request through Rust net stack
// ebx: dst IPv4 in host byte order, ecx: id, edx: seq
int sys_ping_echo(struct syscall_frame* regs) {
    uint32_t dst_ip_h = (uint32_t)regs->ebx;
    uint16_t id       = (uint16_t)regs->ecx;
    uint16_t seq      = (uint16_t)regs->edx;
    return rust_net_ping_echo_host(dst_ip_h, id, seq);
}

// netcfg_set() — apply runtime IPv4 config in Rust net stack
// ebx: pointer to netcfg_args_t in userspace
int sys_netcfg_set(struct syscall_frame* regs) {
    netcfg_args_t* args = (netcfg_args_t*)regs->ebx;
    if (!validate_user_ptr(args, sizeof(netcfg_args_t))) return -1;
    return rust_net_set_ipv4_config(args->ip_host,
                                    args->netmask_host,
                                    args->gateway_host,
                                    args->dns_host);
}