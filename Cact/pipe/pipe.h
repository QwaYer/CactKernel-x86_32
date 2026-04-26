#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include "vfs.h"
#include "sync.h"

#define PIPE_BUF_SIZE   4096
#define O_NONBLOCK      0x0800
#define PIPE_MAGIC      0x50495045

#ifndef EAGAIN
#define EAGAIN  11
#endif
#ifndef EPIPE
#define EPIPE   32
#endif

typedef struct pipe {
    uint32_t    magic;
    uint8_t     buf[PIPE_BUF_SIZE];
    uint32_t    read_pos;
    uint32_t    write_pos;
    uint32_t    len;
    int         flags;
    int         write_open;
    int         read_open;
    int         ref_count;    /* # of vfs_node_t wrappers pointing here */
    mutex_t     lock;
    char       *name;         /* points into name_buf, or NULL for anonymous pipe */
    char        name_buf[128];
} pipe_t;


//Public api
int         pipe_create(vfs_node_t *pipefd[2], int flags);
vfs_node_t *fifo_create(const char *name, int flags);

int  pipe_read (pipe_t *p, uint32_t offset, uint32_t size, char *buffer);
int  pipe_write(pipe_t *p, uint32_t offset, uint32_t size, char *buffer);

void pipe_close_read (pipe_t *p);
void pipe_close_write(pipe_t *p);
void pipe_destroy    (pipe_t *p);

#endif