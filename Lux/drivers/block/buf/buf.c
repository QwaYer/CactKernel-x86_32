#include "buf.h"
#include "ata.h"
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
            /* Ждём если занят (другой поток читает тот же блок) */
            while (b->flags & B_BUSY) schedule();
            b->flags |= B_BUSY;
            return b;
        }
    }

    /* LRU вытеснение */
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

/* ── Синхронное чтение: ставим в DMA очередь и крутимся пока не готово ── */
struct buf* bread(uint32_t dev, uint32_t blockno) {
    struct buf* b = bget(dev, blockno);
    if (!b) return 0;

    if (b->flags & B_VALID)
        return b;  /* кэш-хит */

    /* Ставим в DMA очередь */
    bio_enqueue_sync(b);

    /* Отдаём управление планировщику пока DMA не завершится */
    while (b->flags & B_QUEUED)
        schedule();

    /* Fallback: если DMA не сработал — PIO */
    if (!(b->flags & B_VALID)) {
        ata_read_sector(0x1F0, (uint8_t)(dev & 1), b->blockno, b->data);
        b->flags |= B_VALID;
    }

    return b;
}

/* ── Синхронная запись ────────────────────────────────────────── */
void bwrite(struct buf* b) {
    if (!(b->flags & B_BUSY)) {
        kprint("[buf] bwrite: buffer not busy\n");
        return;
    }
    b->flags |= B_DIRTY;
    bio_enqueue_sync(b);

    while (b->flags & B_QUEUED)
        schedule();

    /* Fallback */
    if (b->flags & B_DIRTY) {
        ata_write_sector(0x1F0, (uint8_t)(b->dev & 1), b->blockno, b->data);
        b->flags &= ~B_DIRTY;
    }
}

/* ── Асинхронное чтение с callback (неблокирующее) ───────────── */
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
    /* callback вызовется из bio_irq_complete при завершении DMA */
}

/* ── Асинхронная запись с callback ───────────────────────────── */
void bwrite_async(struct buf* b, io_callback_t cb) {
    if (!(b->flags & B_BUSY)) return;
    b->flags   |= B_DIRTY;
    b->callback = cb;
    b->waiter   = 0;
    bio_enqueue_sync(b);
}

/* ── Освобождение буфера (MRU — в голову LRU списка) ─────────── */
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