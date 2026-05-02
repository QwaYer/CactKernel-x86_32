#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include "vfs.h"
#include "sync.h"

#define PIPE_BUF_SIZE   4096    // Size of pipe circular buffer
#define O_NONBLOCK      0x0800  // Non-blocking flag
#define PIPE_MAGIC      0x50495045  // "PIPE" magic number for UAF detection

#ifndef EAGAIN
#define EAGAIN  11   // Resource temporarily unavailable
#endif
#ifndef EPIPE
#define EPIPE   32   // Broken pipe
#endif

typedef struct pipe {
    uint32_t    magic;          // Magic number for validation (PIPE_MAGIC)
    uint8_t     buf[PIPE_BUF_SIZE];  // Circular buffer
    uint32_t    read_pos;       // Current read position in circular buffer
    uint32_t    write_pos;      // Current write position in circular buffer
    uint32_t    len;            // Number of bytes currently in buffer
    int         flags;          // O_NONBLOCK and other flags
    int         write_open;     // Number of open write ends
    int         read_open;      // Number of open read ends
    int         ref_count;      // Number of vfs_node_t wrappers pointing here
    mutex_t     lock;           // Mutex for thread-safe operations
    char       *name;           // Points into name_buf, or NULL for anonymous pipe
    char        name_buf[128];  // Storage for FIFO name
} pipe_t;

// Public API
int         pipe_create(vfs_node_t *pipefd[2], int flags);  // Create anonymous pipe
vfs_node_t *fifo_create(const char *name, int flags);       // Create named FIFO

int  pipe_read (pipe_t *p, uint32_t offset, uint32_t size, char *buffer);   // Read from pipe
int  pipe_write(pipe_t *p, uint32_t offset, uint32_t size, char *buffer);   // Write to pipe

void pipe_close_read (pipe_t *p);   // Close read end
void pipe_close_write(pipe_t *p);   // Close write end
void pipe_destroy    (pipe_t *p);   // Force-destroy pipe

#endif