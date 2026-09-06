#ifndef UNIX_SOCK_H
#define UNIX_SOCK_H

#include "vfs.h"

// unix_sock.h — AF_UNIX (local) stream sockets as VFS socket nodes.
//
// Like TCP/UDP sockets, an AF_UNIX socket is a VFS node of type VFS_SOCKET;
// creation goes through /dev/net (CACT_NETCTL_SOCKET) and everything else
// through the SOCKCTL_* ioctls.  The data path (read/write) is plain
// read()/write() on the socket fd, poll() reports readiness.
//
// This is a kernel-C implementation (unlike the Rust TCP/UDP stack) and only
// SOCK_STREAM is supported for now.

// one-time init of the bound-name registry (called from devfs_init)
void unix_sock_init(void);

// create an unbound AF_UNIX SOCK_STREAM socket node, or NULL on error
vfs_node_t *unix_sock_create(int type, int protocol);

// 1 if this VFS node is an AF_UNIX socket owned by this module
int unix_sock_is_node(vfs_node_t *node);

// SOCKCTL_* dispatcher for AF_UNIX socket fds (called by sock_ioctl_dispatch)
int unix_sock_ioctl(vfs_node_t *node, uint32_t cmd, void *arg);

// socketpair(AF_UNIX, type, 0): install two connected fds in current task.
// Returns 0 on success with fds[0]/fds[1] filled, or a negative errno.
int unix_socketpair(int type, int fds[2]);

#endif
