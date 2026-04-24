#include "pipe.h"
#include "memory.h"
#include "task.h"
#include "klib.h"

#define PIPE_FULL(p)   ((p)->len == PIPE_BUF_SIZE)
#define PIPE_EMPTY(p)  ((p)->len == 0)


static int _vfs_pipe_read (vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static int _vfs_pipe_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
static void _vfs_pipe_open (vfs_node_t *node);
static void _vfs_pipe_close(vfs_node_t *node);

static vfs_ops_t pipe_ops = {
    .read  = _vfs_pipe_read,
    .write = _vfs_pipe_write,
    .open  = _vfs_pipe_open,
    .close = _vfs_pipe_close,
};

// inode: 0 = read-end, 1 = write-end
static inline pipe_t *_node_to_pipe    (vfs_node_t *node) { return (pipe_t *)node->priv; }
static inline int     _node_is_write   (vfs_node_t *node) { return (int)node->inode; }

static pipe_t *_pipe_alloc(int flags) {
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return 0;
    memset(p, 0, sizeof(pipe_t));
    p->magic      = PIPE_MAGIC;
    p->flags      = flags;
    p->write_open = 0;
    p->read_open  = 0;
    p->ref_count  = 0;
    mutex_init(&p->lock);
    return p;
}

static vfs_node_t *_make_node(pipe_t *p, const char *name, int is_write) {
    vfs_node_t *n = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!n) return 0;
    memset(n, 0, sizeof(vfs_node_t));

    int i = 0;
    while (name[i] && i < 127) { n->name[i] = name[i]; i++; }
    n->name[i] = '\0';

    n->type     = VFS_PIPE;
    n->size     = PIPE_BUF_SIZE;
    n->inode    = (uint32_t)is_write;  // 0=read-end, 1=write-end
    n->refcount = 1;                   // initial reference
    n->ops      = &pipe_ops;
    n->priv     = p;                   // pipe_t* stored in priv

    /* Each live vfs_node_t counts as one reference to the pipe_t. */
    mutex_lock(&p->lock);
    p->ref_count++;
    mutex_unlock(&p->lock);

    return n;
}


//Public api
int pipe_create(vfs_node_t *pipefd[2], int flags) {
    pipe_t *p = _pipe_alloc(flags);
    if (!p) return -1;

    pipefd[0] = _make_node(p, "pipe:r", 0);
    if (!pipefd[0]) { kfree_heap(p); return -1; }

    pipefd[1] = _make_node(p, "pipe:w", 1);
    if (!pipefd[1]) { kfree_heap(pipefd[0]); kfree_heap(p); return -1; }

    mutex_lock(&p->lock);
    p->write_open = 1;
    p->read_open  = 1;
    mutex_unlock(&p->lock);
    return 0;
}

vfs_node_t *fifo_create(const char *name, int flags) {
    pipe_t *p = _pipe_alloc(flags);
    if (!p) return 0;

    vfs_node_t *n = _make_node(p, name, 0);
    if (!n) { kfree_heap(p); return 0; }

    /*
     * Store the FIFO name inside pipe_t itself so p->name never becomes a
     * dangling pointer if the vfs_node_t wrapper gets freed before pipe_t.
     */
    int i = 0;
    while (name[i] && i < 127) { p->name_buf[i] = name[i]; i++; }
    p->name_buf[i] = '\0';
    p->name        = p->name_buf;

    p->write_open = 0;
    p->read_open  = 0;
    return n;
}

int pipe_read(pipe_t *p, uint32_t offset, uint32_t size, char *buffer) {
    (void)offset;
    if (!p || p->magic != PIPE_MAGIC || !buffer || size == 0) return -1;

    int      nonblock = (p->flags & O_NONBLOCK);
    uint32_t copied   = 0;

    while (copied < size) {
        mutex_lock(&p->lock);

        if (PIPE_EMPTY(p)) {
            if (p->write_open == 0) { mutex_unlock(&p->lock); return (int)copied; }
            if (nonblock)           { mutex_unlock(&p->lock);
                                      return copied ? (int)copied : -EAGAIN; }
            mutex_unlock(&p->lock);
            schedule();
            continue;
        }

        /* batch copy: take as many bytes as available in one lock */
        uint32_t want  = size - copied;
        uint32_t avail = p->len;
        uint32_t chunk = (want < avail) ? want : avail;

        /* copy up to the wrap-around boundary, then the rest */
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

int pipe_write(pipe_t *p, uint32_t offset, uint32_t size, char *buffer) {
    (void)offset;
    if (!p || p->magic != PIPE_MAGIC || !buffer || size == 0) return -1;

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

        if (p->read_open == 0) {
            mutex_unlock(&p->lock);
            if (current_task) task_signal(current_task->pid, SIGPIPE);
            return written ? (int)written : -EPIPE;
        }

        if (PIPE_FULL(p)) {
            if (nonblock) { mutex_unlock(&p->lock);
                            return written ? (int)written : -EAGAIN; }
            mutex_unlock(&p->lock);
            schedule();
            continue;
        }

        /* batch copy: fill as much free space as possible in one lock */
        uint32_t want  = size - written;
        uint32_t space = PIPE_BUF_SIZE - p->len;
        uint32_t chunk = (want < space) ? want : space;

        /* copy up to the wrap-around boundary, then the rest */
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

/*
 * Logical-only close helpers used by external callers (e.g. process teardown)
 * that already hold their own reference and handle lifetime separately.
 * These do NOT free pipe_t; lifetime is managed via ref_count in _vfs_pipe_close.
 */
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

void pipe_destroy(pipe_t *p) {
    if (!p) return;
    mutex_lock(&p->lock);
    p->magic = 0;
    mutex_unlock(&p->lock);
    kfree_heap(p);
}


static int _vfs_pipe_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    return pipe_read(_node_to_pipe(node), off, size, buf);
}

static int _vfs_pipe_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    return pipe_write(_node_to_pipe(node), off, size, buf);
}

static void _vfs_pipe_open(vfs_node_t *node) {
    pipe_t *p = _node_to_pipe(node);
    if (!p || p->magic != PIPE_MAGIC) return;
    node->refcount++;
    mutex_lock(&p->lock);
    p->ref_count++;
    if (p->name) {
        p->read_open++;
        p->write_open++;
    } else {
        if (_node_is_write(node)) p->write_open++;
        else                      p->read_open++;
    }
    mutex_unlock(&p->lock);
}

static void _vfs_pipe_close(vfs_node_t *node) {
    pipe_t *p = _node_to_pipe(node);

    /*
     * Guard against a node whose pipe was already forcefully destroyed
     * (e.g. via pipe_destroy).  We still need to release the node itself.
     */
    if (!p || p->magic != PIPE_MAGIC) {
        if (node->refcount <= 1) kfree_heap(node);
        else                     node->refcount--;
        return;
    }

    /*
     * Update logical open counters and drop the ref_count in one critical
     * section.  This prevents the FIFO UAF (BUG-1) and the double-free race
     * (BUG-2) from the old _pipe_try_destroy pattern.
     */
    mutex_lock(&p->lock);
    if (p->name) {
        /* Named FIFO: one fd covers both ends */
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
    if (should_free) p->magic = 0;
    mutex_unlock(&p->lock);

    /* Free pipe_t only after the lock is released and ref_count confirmed 0. */
    if (should_free) kfree_heap(p);

    /* Release the vfs_node_t wrapper. */
    if (node->refcount <= 1)
        kfree_heap(node);
    else
        node->refcount--;
}