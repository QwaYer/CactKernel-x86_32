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

// SOCKCTL_* node-ioctl dispatcher (new ABI) — called by sys_ioctl on socket fds.
int sock_ioctl_dispatch(vfs_node_t *node, uint32_t cmd, void *arg);

#endif
