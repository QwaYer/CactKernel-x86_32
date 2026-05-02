#include "buf.h"
#include "nvme.h"
#include "kernel.h"
#include "memory.h"
#include "task.h"
#include "sync.h"

// Number of cache slots — small for a single-user system
#define NBUF 30

// Global buffer cache: fixed array of buffers + sentinel head
struct {
    struct buf buf[NBUF];
    struct buf head;
} bcache;

// Serialises all bcache modifications (LRU list + flags)
static irq_spinlock_t bcache_lock;

// Initialise the buffer cache: link all buffers into a circular LRU list
void binit(void) {
    struct buf* b;
    irq_spinlock_init(&bcache_lock);
    bcache.head.prev = &bcache.head;
    bcache.head.next = &bcache.head;
    for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
        b->next     = bcache.head.next;
        b->prev     = &bcache.head;
        b->flags    = 0;
        b->waiter   = 0;
        b->callback = 0;
        b->qnext    = 0;
        bcache.head.next->prev = b;
        bcache.head.next       = b;
    }
}

// Find a buffer by (dev, blockno); if busy, sleep until released.
// If not found, recycle the least-recently-used clean buffer.
static struct buf* bget(uint32_t dev, uint32_t blockno) {
    struct buf* b;

    irq_spinlock_acquire(&bcache_lock);

    // First pass: exact match
    for (b = bcache.head.next; b != &bcache.head; b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            while (b->flags & B_BUSY) {
                irq_spinlock_release(&bcache_lock);
                schedule();
                irq_spinlock_acquire(&bcache_lock);
            }
            b->flags |= B_BUSY;
            irq_spinlock_release(&bcache_lock);
            return b;
        }
    }

    // Second pass: find a clean, unused buffer from the tail (LRU)
    for (b = bcache.head.prev; b != &bcache.head; b = b->prev) {
        if (!(b->flags & B_BUSY) && !(b->flags & B_DIRTY)) {
            b->dev      = dev;
            b->blockno  = blockno;
            b->flags    = B_BUSY;
            b->waiter   = 0;
            b->callback = 0;
            b->qnext    = 0;
            irq_spinlock_release(&bcache_lock);
            return b;
        }
    }

    irq_spinlock_release(&bcache_lock);
    kprint("[buf] bget: no free buffers!\n");
    return 0;
}

// Wait until the buffer's I/O completes (B_QUEUED cleared).
// If no scheduler, busy-wait with HLT.
static void bio_wait(struct buf* b) {
    if (!current_task) {
        while (b->flags & B_QUEUED)
            __asm__ __volatile__("sti; hlt" ::: "memory");
        return;
    }

    while (1) {
        irq_spinlock_acquire(&bcache_lock);
        if (!(b->flags & B_QUEUED)) {
            irq_spinlock_release(&bcache_lock);
            return;
        }
        b->waiter = (void*)current_task;
        irq_spinlock_acquire(&scheduler_lock);
        current_task->state = TASK_SLEEPING;
        irq_spinlock_release(&scheduler_lock);
        irq_spinlock_release(&bcache_lock);
        schedule();
    }
}

// Synchronous bread: get buffer, wait for I/O, return valid data
struct buf* bread(uint32_t dev, uint32_t blockno) {
    struct buf* b = bget(dev, blockno);
    if (!b) return 0;

    if (b->flags & B_VALID)
        return b;

    bio_enqueue_sync(b);
    bio_wait(b);

    if (b->flags & B_ERROR) {
        b->flags &= ~B_ERROR;
    }

    return b;
}

// Synchronous bwrite: mark dirty, enqueue I/O, wait, clear errors
void bwrite(struct buf* b) {
    if (!(b->flags & B_BUSY)) {
        kprint("[buf] bwrite: buffer not busy\n");
        return;
    }
    b->flags |= B_DIRTY;
    bio_enqueue_sync(b);
    bio_wait(b);

    if (b->flags & B_ERROR) {
        b->flags &= ~B_ERROR;
    }
}

// Asynchronous bread: callback invoked when I/O completes
void bread_async(uint32_t dev, uint32_t blockno, io_callback_t cb) {
    struct buf* b = bget(dev, blockno);
    if (!b) { if (cb) cb(0, 1); return; }

    if (b->flags & B_VALID) {
        if (cb) cb(b, 0);
        brelse(b);
        return;
    }

    b->callback = cb;
    b->waiter   = 0;
    bio_enqueue_sync(b);
}

// Asynchronous bwrite: mark dirty, enqueue, callback on completion
void bwrite_async(struct buf* b, io_callback_t cb) {
    if (!(b->flags & B_BUSY)) return;
    b->flags   |= B_DIRTY;
    b->callback = cb;
    b->waiter   = 0;
    bio_enqueue_sync(b);
}

// Release a buffer: move to head of LRU and clear BUSY
void brelse(struct buf* b) {
    if (!(b->flags & B_BUSY)) {
        kprint("[buf] brelse: buffer not busy\n");
        return;
    }
    irq_spinlock_acquire(&bcache_lock);
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next       = b;
    b->flags &= ~B_BUSY;
    irq_spinlock_release(&bcache_lock);
}