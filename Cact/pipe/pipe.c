#include "pipe.h"
#include "memory.h"
#include "task.h"
#include "klib.h"

// Helper macros for buffer state checks
#define PIPE_FULL(p)   ((p)->len == PIPE_BUF_SIZE)   // 1 if buffer has no free space
#define PIPE_EMPTY(p)  ((p)->len == 0)               // 1 if buffer has no data

// Forward declarations for VFS callback functions
static int _vfs_pipe_read (vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static int _vfs_pipe_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static void _vfs_pipe_open (vfs_node_t *node);
static void _vfs_pipe_close(vfs_node_t *node);
static int  _vfs_pipe_poll (vfs_node_t *node, uint32_t events);

// VFS operation table for pipe/fifo nodes
static vfs_ops_t pipe_ops = {
    .read  = _vfs_pipe_read,
    .write = _vfs_pipe_write,
    .open  = _vfs_pipe_open,
    .close = _vfs_pipe_close,
    .poll  = _vfs_pipe_poll,
};

// Extract pipe_t from vfs_node_t private data
// inode: 0 = read-end, 1 = write-end (for anonymous pipes only)
static inline pipe_t *_node_to_pipe    (vfs_node_t *node) { return (pipe_t *)node->priv; }
static inline int     _node_is_write   (vfs_node_t *node) { return (int)node->inode; }

// Allocate and initialize a new pipe structure
static pipe_t *_pipe_alloc(int flags) {
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return 0;
    memset(p, 0, sizeof(pipe_t));
    p->magic      = PIPE_MAGIC;           // Magic number for UAF detection
    p->flags      = flags;                // O_NONBLOCK and other flags
    p->write_open = 0;                    // Number of open write ends
    p->read_open  = 0;                    // Number of open read ends
    p->ref_count  = 0;                    // Number of vfs_node_t wrappers referencing this pipe
    mutex_init(&p->lock);
    return p;
}

// Create a vfs_node wrapper around a pipe_t
static vfs_node_t *_make_node(pipe_t *p, const char *name, int is_write) {
    vfs_node_t *n = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!n) return 0;
    memset(n, 0, sizeof(vfs_node_t));

    // Copy name (max 127 chars + null terminator)
    int i = 0;
    while (name[i] && i < 127) { n->name[i] = name[i]; i++; }
    n->name[i] = '\0';

    n->type     = VFS_PIPE;
    n->size     = PIPE_BUF_SIZE;
    n->inode    = (uint32_t)is_write;     // 0=read-end, 1=write-end
    n->refcount = 1;                      // Initial reference
    n->ops      = &pipe_ops;
    n->priv     = p;                      // Store pipe_t* in priv

    // Each live vfs_node_t counts as one reference to the pipe_t
    mutex_lock(&p->lock);
    p->ref_count++;
    mutex_unlock(&p->lock);

    return n;
}

// Public API: Create anonymous pipe, returns two vfs_node pointers (read, write)
int pipe_create(vfs_node_t *pipefd[2], int flags) {
    pipe_t *p = _pipe_alloc(flags);
    if (!p) return -1;

    pipefd[0] = _make_node(p, "pipe:r", 0);   // Read end
    if (!pipefd[0]) { kfree_heap(p); return -1; }

    pipefd[1] = _make_node(p, "pipe:w", 1);   // Write end
    if (!pipefd[1]) { kfree_heap(pipefd[0]); kfree_heap(p); return -1; }

    mutex_lock(&p->lock);
    p->write_open = 1;
    p->read_open  = 1;
    mutex_unlock(&p->lock);
    return 0;
}

// Public API: Create named FIFO, returns single vfs_node
vfs_node_t *fifo_create(const char *name, int flags) {
    pipe_t *p = _pipe_alloc(flags);
    if (!p) return 0;

    vfs_node_t *n = _make_node(p, name, 0);
    if (!n) { kfree_heap(p); return 0; }

    // Store FIFO name inside pipe_t to avoid dangling pointer if vfs_node is freed first
    int i = 0;
    while (name[i] && i < 127) { p->name_buf[i] = name[i]; i++; }
    p->name_buf[i] = '\0';
    p->name        = p->name_buf;

    p->write_open = 0;
    p->read_open  = 0;
    return n;
}

// Core read function: copies data from circular buffer to output buffer
int pipe_read(pipe_t *p, uint32_t offset, uint32_t size, char *buffer) {
    (void)offset;   // offset is ignored for pipes (sequential only)
    if (!p || p->magic != PIPE_MAGIC || !buffer || size == 0) return -1;

    int      nonblock = (p->flags & O_NONBLOCK);
    uint32_t copied   = 0;

    while (copied < size) {
        mutex_lock(&p->lock);

        if (PIPE_EMPTY(p)) {
            // Writer closed: return what we have (EOF semantics)
            if (p->write_open == 0) { mutex_unlock(&p->lock); return (int)copied; }
            // Non-blocking: return EAGAIN if nothing read yet
            if (nonblock)           { mutex_unlock(&p->lock);
                                      return copied ? (int)copied : -EAGAIN; }
            mutex_unlock(&p->lock);
            schedule();              // Block waiting for data
            continue;
        }

        // Copy minimum of requested and available bytes
        uint32_t want  = size - copied;
        uint32_t avail = p->len;
        uint32_t chunk = (want < avail) ? want : avail;

        // Handle circular buffer wrap-around
        uint32_t to_end = PIPE_BUF_SIZE - p->read_pos;
        if (chunk <= to_end) {
            memcpy(buffer + copied, p->buf + p->read_pos, chunk);
        } else {
            memcpy(buffer + copied, p->buf + p->read_pos, to_end);
            memcpy(buffer + copied + to_end, p->buf, chunk - to_end);
        }
        p->read_pos = (p->read_pos + chunk) % PIPE_BUF_SIZE;
        p->len     -= chunk;
        copied     += chunk;

        mutex_unlock(&p->lock);
    }
    return (int)copied;
}

// Core write function: copies data from input buffer to circular buffer
int pipe_write(pipe_t *p, uint32_t offset, uint32_t size, char *buffer) {
    (void)offset;   // offset is ignored for pipes (sequential only)
    if (!p || p->magic != PIPE_MAGIC || !buffer || size == 0) return -1;

    // Quick check: reader closed -> EPIPE before entering main loop
    mutex_lock(&p->lock);
    if (p->read_open == 0) {
        mutex_unlock(&p->lock);
        if (current_task) task_signal(current_task->pid, SIGPIPE);
        return -EPIPE;
    }
    mutex_unlock(&p->lock);

    int      nonblock = (p->flags & O_NONBLOCK);
    uint32_t written  = 0;

    while (written < size) {
        mutex_lock(&p->lock);

        // Re-check reader status after acquiring lock
        if (p->read_open == 0) {
            mutex_unlock(&p->lock);
            if (current_task) task_signal(current_task->pid, SIGPIPE);
            return written ? (int)written : -EPIPE;
        }

        if (PIPE_FULL(p)) {
            if (nonblock) { mutex_unlock(&p->lock);
                            return written ? (int)written : -EAGAIN; }
            mutex_unlock(&p->lock);
            schedule();              // Block waiting for space
            continue;
        }

        // Write minimum of requested and free space
        uint32_t want  = size - written;
        uint32_t space = PIPE_BUF_SIZE - p->len;
        uint32_t chunk = (want < space) ? want : space;

        // Handle circular buffer wrap-around
        uint32_t to_end = PIPE_BUF_SIZE - p->write_pos;
        if (chunk <= to_end) {
            memcpy(p->buf + p->write_pos, buffer + written, chunk);
        } else {
            memcpy(p->buf + p->write_pos, buffer + written, to_end);
            memcpy(p->buf, buffer + written + to_end, chunk - to_end);
        }
        p->write_pos = (p->write_pos + chunk) % PIPE_BUF_SIZE;
        p->len      += chunk;
        written     += chunk;

        mutex_unlock(&p->lock);
    }
    return (int)written;
}

// Logical close helpers: decrement counters without freeing pipe_t
// Used by external callers (e.g., process teardown) that manage lifetime separately
void pipe_close_read(pipe_t *p) {
    if (!p || p->magic != PIPE_MAGIC) return;
    mutex_lock(&p->lock);
    if (p->read_open > 0) p->read_open--;
    mutex_unlock(&p->lock);
}

void pipe_close_write(pipe_t *p) {
    if (!p || p->magic != PIPE_MAGIC) return;
    mutex_lock(&p->lock);
    if (p->write_open > 0) p->write_open--;
    mutex_unlock(&p->lock);
}

// Force-destroy pipe_t (called when ref_count reaches zero)
void pipe_destroy(pipe_t *p) {
    if (!p) return;
    mutex_lock(&p->lock);
    p->magic = 0;    // Invalidate magic for UAF detection
    mutex_unlock(&p->lock);
    kfree_heap(p);
}

// VFS read wrapper
static int _vfs_pipe_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    return pipe_read(_node_to_pipe(node), off, size, buf);
}

// VFS write wrapper
static int _vfs_pipe_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    return pipe_write(_node_to_pipe(node), off, size, buf);
}

// VFS open wrapper: increment reference counters
static void _vfs_pipe_open(vfs_node_t *node) {
    pipe_t *p = _node_to_pipe(node);
    if (!p || p->magic != PIPE_MAGIC) return;
    node->refcount++;
    mutex_lock(&p->lock);
    p->ref_count++;
    if (p->name) {
        // Named FIFO: single node represents both ends
        p->read_open++;
        p->write_open++;
    } else {
        // Anonymous pipe: node distinguishes read vs write via inode
        if (_node_is_write(node)) p->write_open++;
        else                      p->read_open++;
    }
    mutex_unlock(&p->lock);
}

// VFS close wrapper: decrement counters, free pipe_t when ref_count reaches zero
static void _vfs_pipe_close(vfs_node_t *node) {
    pipe_t *p = _node_to_pipe(node);

    // Handle case where pipe_t was already force-destroyed (e.g., via pipe_destroy)
    if (!p || p->magic != PIPE_MAGIC) {
        if (node->refcount <= 1) kfree_heap(node);
        else                     node->refcount--;
        return;
    }

    // Atomically update counters and determine if pipe_t should be freed
    mutex_lock(&p->lock);
    if (p->name) {
        // Named FIFO: one node covers both ends
        if (p->read_open  > 0) p->read_open--;
        if (p->write_open > 0) p->write_open--;
    } else {
        if (_node_is_write(node)) {
            if (p->write_open > 0) p->write_open--;
        } else {
            if (p->read_open  > 0) p->read_open--;
        }
    }
    int should_free = (--p->ref_count == 0);
    if (should_free) p->magic = 0;    // Invalidate before freeing
    mutex_unlock(&p->lock);

    // Free pipe_t outside lock (after confirming ref_count == 0)
    if (should_free) {
        kfree_heap(p);
        node->priv = NULL;
    }

    // Release the vfs_node_t wrapper
    if (node->refcount <= 1)
        kfree_heap(node);
    else
        node->refcount--;
}

static int _vfs_pipe_poll(vfs_node_t *node, uint32_t events) {
    pipe_t *p = _node_to_pipe(node);
    if (!p || p->magic != PIPE_MAGIC) return VFS_POLLERR;

    uint32_t revents = 0;
    int is_wr = _node_is_write(node);

    mutex_lock(&p->lock);

    if (is_wr) {
        // Write end
        if (events & VFS_POLLOUT) {
            if (p->len < PIPE_BUF_SIZE) revents |= VFS_POLLOUT;
        }
        if (!p->read_open) revents |= VFS_POLLERR;
    } else {
        // Read end
        if (events & VFS_POLLIN) {
            if (p->len > 0) revents |= VFS_POLLIN;
        }
        if (!p->write_open && p->len == 0) revents |= VFS_POLLHUP;
    }

    mutex_unlock(&p->lock);
    return (int)revents;
}