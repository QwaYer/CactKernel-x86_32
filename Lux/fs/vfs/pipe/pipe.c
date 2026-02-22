#include "pipe.h"
#include "memory.h"  
#include "task.h"    
#include "libc.h"     


#define PIPE_FULL(p)   ((p)->len == PIPE_BUF_SIZE)
#define PIPE_EMPTY(p)  ((p)->len == 0)


static int  _vfs_pipe_read (struct vfs_node* node, unsigned int off,
                             unsigned int size, char* buf);
static int  _vfs_pipe_write(struct vfs_node* node, unsigned int off,
                             unsigned int size, char* buf);
static void _vfs_pipe_open (struct vfs_node* node);
static void _vfs_pipe_close(struct vfs_node* node);


static pipe_t* _pipe_alloc(int flags)
{
    pipe_t* p = (pipe_t*)kmalloc(sizeof(pipe_t));
    if (!p) return 0;

    memset(p, 0, sizeof(pipe_t));
    p->magic      = PIPE_MAGIC;
    p->flags      = flags;
    p->write_open = 1;
    p->read_open  = 1;
    mutex_init(&p->lock);
    return p;
}

static struct vfs_node* _make_pipe_node(pipe_t* p, const char* name)
{
    struct vfs_node* n = (struct vfs_node*)kmalloc(sizeof(struct vfs_node));
    if (!n) return 0;

    memset(n, 0, sizeof(struct vfs_node));
    int i = 0;
    while (name[i] && i < (int)(sizeof(n->name) - 1)) {
        n->name[i] = name[i];
        i++;
    }
    n->name[i] = '\0';
    n->type  = VFS_PIPE;
    n->size  = PIPE_BUF_SIZE;
    n->inode = 0;

    n->read  = _vfs_pipe_read;
    n->write = _vfs_pipe_write;
    n->open  = _vfs_pipe_open;
    n->close = _vfs_pipe_close;

    n->ptr = (struct vfs_node*)p;   

    return n;
}

int pipe_create(struct vfs_node* pipefd[2], int flags)
{
    pipe_t* p = _pipe_alloc(flags);
    if (!p) return -1;

    /* read-end */
    pipefd[0] = _make_pipe_node(p, "pipe:r");
    if (!pipefd[0]) { kfree_heap(p); return -1; }

    /* write-end */
    pipefd[1] = _make_pipe_node(p, "pipe:w");
    if (!pipefd[1]) { kfree_heap(pipefd[0]); kfree_heap(p); return -1; }

    mutex_lock(&p->lock);
    p->write_open = 1;
    p->read_open  = 1; 
    mutex_unlock(&p->lock);

    return 0;
}

struct vfs_node* fifo_create(const char* name, int flags)
{
    pipe_t* p = _pipe_alloc(flags);
    if (!p) return 0;

    struct vfs_node* n = _make_pipe_node(p, name);
    if (!n) { kfree_heap(p); return 0; }

    p->name = n->name;   

    p->write_open = 0;
    p->read_open  = 0;

    return n;
}

int pipe_read(pipe_t* p, unsigned int offset, unsigned int size, char* buffer)
{
    (void)offset; 

    if (!p || p->magic != PIPE_MAGIC || !buffer || size == 0)
        return -1;

    int nonblock = (p->flags & O_NONBLOCK);
    unsigned int copied = 0;

    while (copied < size)
    {
        mutex_lock(&p->lock);

        if (PIPE_EMPTY(p)) {
            if (p->write_open == 0) {
                mutex_unlock(&p->lock);
                return (int)copied;
            }
            if (nonblock) {
                mutex_unlock(&p->lock);
                return (copied > 0) ? (int)copied : -1; 
            }
            mutex_unlock(&p->lock);
            schedule();
            continue;
        }

        buffer[copied] = (char)p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        p->len--;
        copied++;

        mutex_unlock(&p->lock);
    }

    return (int)copied;
}

int pipe_write(pipe_t* p, unsigned int offset, unsigned int size, char* buffer)
{
    (void)offset;

    if (!p || p->magic != PIPE_MAGIC || !buffer || size == 0)
        return -1;

    int nonblock = (p->flags & O_NONBLOCK);
    unsigned int written = 0;

    while (written < size)
    {
        mutex_lock(&p->lock);

        if (PIPE_FULL(p)) {
            if (nonblock) {
                mutex_unlock(&p->lock);
                return (written > 0) ? (int)written : -1; 
            }
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

void pipe_close_read(pipe_t* p)
{
    if (!p || p->magic != PIPE_MAGIC) return;

    mutex_lock(&p->lock);
    if (p->read_open > 0) p->read_open--;
    int dead = (p->read_open == 0 && p->write_open == 0);
    mutex_unlock(&p->lock);

    if (dead) pipe_destroy(p);
}

void pipe_close_write(pipe_t* p)
{
    if (!p || p->magic != PIPE_MAGIC) return;

    mutex_lock(&p->lock);
    if (p->write_open > 0) p->write_open--;
    int dead = (p->read_open == 0 && p->write_open == 0);
    mutex_unlock(&p->lock);

    if (dead) pipe_destroy(p);
}

void pipe_destroy(pipe_t* p)
{
    if (!p || p->magic != PIPE_MAGIC) return;
    p->magic = 0;   
    kfree_heap(p);
}

static inline pipe_t* _node_to_pipe(struct vfs_node* node)
{
    return (pipe_t*)node->ptr;
}

static int _vfs_pipe_read(struct vfs_node* node, unsigned int off,
                           unsigned int size, char* buf)
{
    return pipe_read(_node_to_pipe(node), off, size, buf);
}

static int _vfs_pipe_write(struct vfs_node* node, unsigned int off,
                            unsigned int size, char* buf)
{
    return pipe_write(_node_to_pipe(node), off, size, buf);
}

static void _vfs_pipe_open(struct vfs_node* node)
{
    pipe_t* p = _node_to_pipe(node);
    if (!p || p->magic != PIPE_MAGIC) return;

    mutex_lock(&p->lock);
    if (p->name) {
        p->read_open++;
        p->write_open++;
    }
    mutex_unlock(&p->lock);
}

static void _vfs_pipe_close(struct vfs_node* node)
{
    pipe_t* p = _node_to_pipe(node);
    if (!p || p->magic != PIPE_MAGIC) return;

    if (p->name) {
        pipe_close_read(p);
        pipe_close_write(p);
    }
}