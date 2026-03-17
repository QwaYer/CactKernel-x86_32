#include "buf.h"
#include "nvme.h"
#include "kernel.h"
#include "memory.h"

#define NBUF 30

struct {
    struct buf buf[NBUF];
    struct buf head;
} bcache;

void binit(void) {
    struct buf* b;
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

static struct buf* bget(uint32_t dev, uint32_t blockno) {
    struct buf* b;

    for (b = bcache.head.next; b != &bcache.head; b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            while (b->flags & B_BUSY) schedule();
            b->flags |= B_BUSY;
            return b;
        }
    }

    for (b = bcache.head.prev; b != &bcache.head; b = b->prev) {
        if (!(b->flags & B_BUSY) && !(b->flags & B_DIRTY)) {
            b->dev      = dev;
            b->blockno  = blockno;
            b->flags    = B_BUSY;
            b->waiter   = 0;
            b->callback = 0;
            b->qnext    = 0;
            return b;
        }
    }

    kprint("[buf] bget: no free buffers!\n");
    return 0;
}

struct buf* bread(uint32_t dev, uint32_t blockno) {
    struct buf* b = bget(dev, blockno);
    if (!b) return 0;

    if (b->flags & B_VALID)
        return b;

    bio_enqueue_sync(b);

    while (b->flags & B_QUEUED)
        schedule();

    if (!(b->flags & B_VALID)) {
        nvme_read_sector(b->blockno, b->data);
        b->flags |= B_VALID;
    }

    return b;
}

void bwrite(struct buf* b) {
    if (!(b->flags & B_BUSY)) {
        kprint("[buf] bwrite: buffer not busy\n");
        return;
    }
    b->flags |= B_DIRTY;
    bio_enqueue_sync(b);

    while (b->flags & B_QUEUED)
        schedule();

    if (b->flags & B_DIRTY) {
        nvme_write_sector(b->blockno, b->data);
        b->flags &= ~B_DIRTY;
    }
}

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

void bwrite_async(struct buf* b, io_callback_t cb) {
    if (!(b->flags & B_BUSY)) return;
    b->flags   |= B_DIRTY;
    b->callback = cb;
    b->waiter   = 0;
    bio_enqueue_sync(b);
}

void brelse(struct buf* b) {
    if (!(b->flags & B_BUSY)) {
        kprint("[buf] brelse: buffer not busy\n");
        return;
    }
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next       = b;
    b->flags &= ~B_BUSY;
}
