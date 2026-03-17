#ifndef BUF_H
#define BUF_H

#include <stdint.h>

#define B_BUSY    0x01
#define B_VALID   0x02
#define B_DIRTY   0x04
#define B_QUEUED  0x08
#define B_ERROR   0x10

struct buf;
typedef void (*io_callback_t)(struct buf *b, int error);

struct buf {
    int      flags;
    uint32_t dev;
    uint32_t blockno;

    struct buf *prev;
    struct buf *next;
    struct buf *qnext;

    void    *waiter;

    io_callback_t callback;

    uint8_t  data[512] __attribute__((aligned(512)));
};

//public api
void        binit  (void);
struct buf *bread  (uint32_t dev, uint32_t blockno);
void        bwrite (struct buf *b);
void        brelse (struct buf *b);

void        bread_async (uint32_t dev, uint32_t blockno, io_callback_t cb);
void        bwrite_async(struct buf *b,                  io_callback_t cb);

void        bio_irq_complete(int error);

#endif
