#include "net.h"
#include <net.h>
#include "validate.h"
#include "helper.h"
#include "rust_net_ffi.h"
#include "klib.h"
#include "ioctl_abi.h"

// SOCKCTL_* — the new node-ioctl ABI for socket control.  The ioctl() is
// issued on the socket fd itself (like bind/connect are today), so the node
// is already resolved by the syscall layer.  Once the legacy net syscalls
// are dropped, bind/connect/listen/accept/shutdown/sockopt/sendto/recvfrom
// are all reached through this single entry point.
// =========================================================================

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EFAULT
#define EFAULT 14
#endif

int sock_ioctl_dispatch(vfs_node_t *node, uint32_t cmd, void *arg) {
    if (!node || node->type != VFS_SOCKET) return -1;
    ksock_t *ks = ksock_from_node(node);
    if (!ks) return -1;

    switch (cmd) {
    case CACT_SOCKCTL_BIND: {
        cact_sockaddr_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        uint16_t port = ntohs(a.addr.port);
        if (ks->kind == KS_TCP) {
            tcp_socket_t *s;
            if (ks->proto_idx >= 0 && ks->proto_idx < TCP_MAX_SOCKETS)
                s = &tcp_sockets[ks->proto_idx];
            else return -1;
            s->local_port = port;
            s->local_ip   = htonl(rust_net_get_ip_host());
            return 0;
        }
        if (ks->kind == KS_UDP) {
            udp_sock_t *s = udp_sock_find_by_port(port);
            if (s && s != &udp_socks[ks->proto_idx] && !ks->so_reuseaddr) return -1;
            udp_socks[ks->proto_idx].local_port = port;
            udp_socks[ks->proto_idx].local_ip   = ntohl(a.addr.addr);
            return 0;
        }
        return -1;
    }

    case CACT_SOCKCTL_CONNECT: {
        cact_sockaddr_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (ks->kind != KS_TCP) return -1;
        return tcp_connect(ks->proto_idx, a.addr.addr, ntohs(a.addr.port));
    }

    case CACT_SOCKCTL_LISTEN: {
        if (ks->kind != KS_TCP) return -1;
        extern tcp_socket_t tcp_sockets[];
        tcp_socket_t *s = &tcp_sockets[ks->proto_idx];
        return tcp_listen(ks->proto_idx, s->local_port);
    }

    case CACT_SOCKCTL_ACCEPT: {
        cact_accept_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (!current_task) return -1;

        struct sockaddr_in peer;
        memset(&peer, 0, sizeof(peer));
        vfs_node_t *conn = ksock_tcp_accept(node, &peer);
        if (!conn) return -1;

        a.peer.addr = peer.sin_addr;
        a.peer.port = peer.sin_port;
        if (copy_to_user(arg, &a, sizeof(a)) != 0) {
            close_vfs(conn);
            return -EFAULT;
        }
        return alloc_fd(conn);
    }

    case CACT_SOCKCTL_SHUTDOWN: {
        uint32_t how;
        if (!arg) return -EINVAL;
        if (copy_from_user(&how, arg, sizeof(how)) != 0) return -EFAULT;
        return ksock_shutdown(node, (int)how);
    }

    case CACT_SOCKCTL_SETSOCKOPT: {
        cact_sockopt_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

        if (a.level == SOL_SOCKET) {
            switch (a.optname) {
            case SO_REUSEADDR:
                ks->so_reuseaddr = (uint8_t)!!a.val;
                return 0;
            case SO_KEEPALIVE:
                ks->so_keepalive = (uint8_t)!!a.val;
                if (ks->kind == KS_TCP && ks->proto_idx >= 0 &&
                    ks->proto_idx < TCP_MAX_SOCKETS)
                    tcp_sockets[ks->proto_idx].keepalive = ks->so_keepalive;
                return 0;
            case SO_ERROR:
                return -1;
            }
        } else if (a.level == IPPROTO_TCP) {
            switch (a.optname) {
            case TCP_NODELAY:
                ks->tcp_nodelay = (uint8_t)!!a.val;
                if (ks->kind == KS_TCP && ks->proto_idx >= 0 &&
                    ks->proto_idx < TCP_MAX_SOCKETS)
                    tcp_sockets[ks->proto_idx].nodelay = ks->tcp_nodelay;
                return 0;
            }
        }
        return -1;
    }

    case CACT_SOCKCTL_GETSOCKOPT: {
        cact_sockopt_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

        a.val_out = 0;
        int ok = 0;
        if (a.level == SOL_SOCKET) {
            switch (a.optname) {
            case SO_REUSEADDR: a.val_out = ks->so_reuseaddr; ok = 1; break;
            case SO_KEEPALIVE: a.val_out = ks->so_keepalive; ok = 1; break;
            case SO_ERROR:
                a.val_out = (uint32_t)ks->so_error;
                ks->so_error = 0;
                ok = 1;
                break;
            }
        } else if (a.level == IPPROTO_TCP) {
            if (a.optname == TCP_NODELAY) {
                a.val_out = ks->tcp_nodelay;
                ok = 1;
            }
        }
        if (!ok) return -1;
        return copy_to_user(arg, &a, sizeof(a));
    }

    case CACT_SOCKCTL_SENDTO: {
        cact_sendto_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (!validate_user_ptr(a.buf, a.len)) return -EFAULT;
        if (a.len > UINT16_MAX) return -1;

        if (ks->kind == KS_TCP)
            return tcp_send(ks->proto_idx, (uint8_t *)a.buf, (uint16_t)a.len);
        if (ks->kind == KS_UDP)
            return udp_sock_send(ks->proto_idx, a.dst.addr, ntohs(a.dst.port),
                                 (const uint8_t *)a.buf, (uint16_t)a.len);
        return -1;
    }

    case CACT_SOCKCTL_RECVFROM: {
        cact_recvfrom_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (!validate_user_ptr(a.buf, a.len)) return -EFAULT;
        if (a.len > UINT16_MAX) return -1;

        int ret = -1;
        if (ks->kind == KS_TCP) {
            ret = tcp_recv(ks->proto_idx, (uint8_t *)a.buf, (uint16_t)a.len);
            if (ret > 0) {
                tcp_socket_t *s = &tcp_sockets[ks->proto_idx];
                a.src.addr = s->remote_ip;
                a.src.port = htons(s->remote_port);
            }
        } else if (ks->kind == KS_UDP) {
            uint32_t src_ip  = 0;
            uint16_t src_port = 0;
            ret = udp_sock_recv(ks->proto_idx, (uint8_t *)a.buf, (uint16_t)a.len,
                                &src_ip, &src_port);
            if (ret > 0) {
                a.src.addr = htonl(src_ip);
                a.src.port = htons(src_port);
            }
        }
        if (ret < 0) return ret;
        return copy_to_user(arg, &a, sizeof(a));
    }

    default:
        return -EINVAL;
    }
}
