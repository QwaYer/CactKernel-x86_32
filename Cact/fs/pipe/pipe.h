#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include "vfs.h"
#include "sync.h"

// Pipes are implemented in the Rust VFS core (Cact/fs/rust_vfs/src/pipe.rs);
// the /dev/pipe registration and ioctl stay in C (devfs_services.c).

#define PIPE_BUF_SIZE   4096        // Size of pipe circular buffer
#define O_NONBLOCK      0x0800      // Non-blocking flag
#define PIPE_MAGIC      0x50495045  // "PIPE" magic number

#ifndef EAGAIN
#define EAGAIN  11
#endif
#ifndef EPIPE
#define EPIPE   32
#endif

// Create an anonymous pipe; fills pipefd[0]=read end, pipefd[1]=write end.
int  pipe_create(vfs_node_t *pipefd[2], int flags);

// Propagate O_NONBLOCK into the pipe state (from sys_fcntl(F_SETFL)).
void vfs_pipe_set_nonblock(vfs_node_t *node, int on);

#endif
