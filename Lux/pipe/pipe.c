#include "pipe.h"
#include "memory.h"
#include "task.h"
#include "libc.h"

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

    n->type  = VFS_PIPE;
    n->size  = PIPE_BUF_SIZE;
    n->inode = (uint32_t)is_write;  // 0=read-end, 1=write-end 
    n->ops   = &pipe_ops;
    n->priv  = p;                   // pipe_t* хранится в priv  
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

    p->name       = n->name;
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

        buffer[copied] = (char)p->buf[p->read_pos];
        p->read_pos    = (p->read_pos + 1) % PIPE_BUF_SIZE;
        p->len--;
        copied++;
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

        p->buf[p->write_pos] = (uint8_t)buffer[written];
        p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        p->len++;
        written++;
        mutex_unlock(&p->lock);
    }
    return (int)written;
}

static void _pipe_try_destroy(pipe_t *p) {
    mutex_lock(&p->lock);
    int dead = (p->read_open == 0 && p->write_open == 0);
    if (dead) p->magic = 0;
    mutex_unlock(&p->lock);
    if (dead) kfree_heap(p);
}

void pipe_close_read(pipe_t *p) {
    if (!p || p->magic != PIPE_MAGIC) return;
    mutex_lock(&p->lock);
    if (p->read_open > 0) p->read_open--;
    mutex_unlock(&p->lock);
    _pipe_try_destroy(p);
}

void pipe_close_write(pipe_t *p) {
    if (!p || p->magic != PIPE_MAGIC) return;
    mutex_lock(&p->lock);
    if (p->write_open > 0) p->write_open--;
    mutex_unlock(&p->lock);
    _pipe_try_destroy(p);
}

void pipe_destroy(pipe_t *p) {
    if (!p) return;
    p->magic = 0;
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
    mutex_lock(&p->lock);
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
    if (!p || p->magic != PIPE_MAGIC) return;
    if (p->name) {
        pipe_close_read(p);
        pipe_close_write(p);
    } else {
        if (_node_is_write(node)) pipe_close_write(p);
        else                      pipe_close_read(p);
    }
}