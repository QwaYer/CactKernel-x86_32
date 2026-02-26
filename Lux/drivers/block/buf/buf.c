#include "buf.h"
#include "ata.h"
#include "kernel.h"
#include "memory.h"
#include "task.h"

#define NBUF 32

static struct {
    struct buf buf[NBUF];
    struct buf head;
} bcache;

static struct buf *disk_queue_head = 0;
static struct buf *disk_queue_tail = 0;
static int         disk_busy       = 0;

static inline void _cli(void) { __asm__ __volatile__("cli" ::: "memory"); }
static inline void _sti(void) { __asm__ __volatile__("sti" ::: "memory"); }

void binit(void) {
    bcache.head.prev = &bcache.head;
    bcache.head.next = &bcache.head;
    for (struct buf *b = bcache.buf; b < bcache.buf + NBUF; b++) {
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        b->waiter   = 0;
        b->callback = 0;
        b->qnext    = 0;
        b->flags    = 0;
        bcache.head.next->prev = b;
        bcache.head.next       = b;
    }
}

static void _buf_wakeup(struct task_struct *t) {
    if (!t) return;
    sched_queue_remove(&sleep_queue, t);
    t->state = TASK_READY;
    sched_queue_push(&ready_queue, t);
}

static void _buf_sleep_on(struct buf *b) {
    if (!current_task) {
        _sti();
        while (!(b->flags & (B_VALID | B_ERROR)))
            __asm__ __volatile__("pause");
        _cli();
        return;
    }

    current_task->state = TASK_SLEEPING;
    sched_queue_push(&sleep_queue, current_task);

    _sti();
    schedule();
    _cli();
}

static void bio_start(struct buf *b) {
    disk_busy = 1;
    b->prdt[0].phys_addr  = (uint32_t)b->data;
    b->prdt[0].byte_count = 512;
    b->prdt[0].flags      = PRDT_EOT;
    int write = (b->flags & B_DIRTY) ? 1 : 0;
    ata_dma_start(0x1F0, 0, b->blockno, write, b->prdt);
}

static void bio_enqueue(struct buf *b) {
    b->qnext = 0;
    b->flags |= B_QUEUED;
    if (disk_queue_tail) {
        disk_queue_tail->qnext = b;
        disk_queue_tail        = b;
    } else {
        disk_queue_head = b;
        disk_queue_tail = b;
    }
    if (!disk_busy)
        bio_start(disk_queue_head);
}

void bio_irq_complete(int error) {
    struct buf *b = disk_queue_head;
    if (!b) {
        ata_dma_stop(0x1F0);
        disk_busy = 0;
        return;
    }

    disk_queue_head = b->qnext;
    if (!disk_queue_head) disk_queue_tail = 0;
    disk_busy = 0;
    b->qnext  = 0;
    b->flags &= ~B_QUEUED;

    if (error) {
        b->flags |= B_ERROR;
    } else if (b->flags & B_DIRTY) {
        b->flags &= ~B_DIRTY;
        b->flags |=  B_VALID;
    } else {
        b->flags |= B_VALID;
    }

    if (disk_queue_head)
        bio_start(disk_queue_head);

    if (b->callback) {
        b->callback(b, error);
    } else {
        _buf_wakeup((struct task_struct *)b->waiter);
        b->waiter = 0;
    }
}

static struct buf *bget(uint32_t dev, uint32_t blockno) {
retry:
    for (struct buf *b = bcache.head.next; b != &bcache.head; b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            if (b->flags & B_BUSY) {
                if (!b->waiter && current_task)
                    b->waiter = current_task;
                _buf_sleep_on(b);
                goto retry;
            }
            b->flags |= B_BUSY;
            return b;
        }
    }

    for (struct buf *b = bcache.head.prev; b != &bcache.head; b = b->prev) {
        if (!(b->flags & B_BUSY) && !(b->flags & B_DIRTY)) {
            b->dev     = dev;
            b->blockno = blockno;
            b->flags   = B_BUSY;
            b->waiter  = 0;
            b->callback= 0;
            b->qnext   = 0;
            return b;
        }
    }

    kprint("[buf] bget: no free buffers!\n");
    return 0;
}

struct buf *bread(uint32_t dev, uint32_t blockno) {
    _cli();
    struct buf *b = bget(dev, blockno);
    if (!b) { _sti(); return 0; }

    if (!(b->flags & B_VALID)) {
        b->waiter   = current_task;
        b->callback = 0;
        bio_enqueue(b);
        _buf_sleep_on(b);
        if (!(b->flags & B_VALID) && !(b->flags & B_ERROR)) {
            _sti();
            ata_read_sector(0x1F0, 0, b->blockno, b->data);
            _cli();
            b->flags |= B_VALID;
        }
    }
    _sti();
    return b;
}

void bwrite(struct buf *b) {
    _cli();
    if (!(b->flags & B_BUSY)) {
        kprint("[buf] bwrite: buffer not busy\n");
        _sti();
        return;
    }
    b->flags   |= B_DIRTY;
    b->waiter   = current_task;
    b->callback = 0;
    bio_enqueue(b);
    _buf_sleep_on(b);
    if (b->flags & B_DIRTY) {
        _sti();
        ata_write_sector(0x1F0, 0, b->blockno, b->data);
        _cli();
        b->flags &= ~B_DIRTY;
        b->flags |=  B_VALID;
    }
    _sti();
}

void brelse(struct buf *b) {
    _cli();
    if (!(b->flags & B_BUSY)) {
        kprint("[buf] brelse: buffer not busy\n");
        _sti();
        return;
    }

    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next       = b;

    struct task_struct *w = (struct task_struct *)b->waiter;
    b->flags &= ~B_BUSY;
    b->waiter  = 0;

    if (w) _buf_wakeup(w);
    _sti();
}

void bread_async(uint32_t dev, uint32_t blockno, io_callback_t cb) {
    _cli();
    struct buf *b = bget(dev, blockno);
    if (!b) { _sti(); if (cb) cb(0, -1); return; }

    if (b->flags & B_VALID) {
        _sti();
        if (cb) cb(b, 0);
        return;
    }

    b->callback = cb;
    b->waiter   = 0;
    bio_enqueue(b);
    _sti();
}

void bwrite_async(struct buf *b, io_callback_t cb) {
    _cli();
    if (!(b->flags & B_BUSY)) {
        kprint("[buf] bwrite_async: buffer not busy\n");
        _sti();
        if (cb) cb(b, -1);
        return;
    }
    b->flags   |= B_DIRTY;
    b->callback = cb;
    b->waiter   = 0;
    bio_enqueue(b);
    _sti();
}