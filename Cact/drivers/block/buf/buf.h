#ifndef BUF_H
#define BUF_H

#include <stdint.h>

// Buffer flags
#define B_BUSY    0x01   // buffer is locked by a reader/writer
#define B_VALID   0x02   // buffer contains valid data from disk
#define B_DIRTY   0x04   // buffer has been modified and must be written back
#define B_QUEUED  0x08   // buffer is in the disk I/O queue
#define B_ERROR   0x10   // I/O error occurred on this buffer

struct buf;
typedef void (*io_callback_t)(struct buf *b, int error);

// Buffer cache entry (512-byte sector)
struct buf {
    int      flags;
    uint32_t dev;
    uint32_t blockno;

    struct buf *prev;    // LRU list
    struct buf *next;    // LRU list
    struct buf *qnext;   // disk queue chain

    void    *waiter;     // task waiting for I/O completion

    io_callback_t callback;

    uint8_t  data[512] __attribute__((aligned(512)));
};

// Public API
void        binit  (void);
struct buf *bread  (uint32_t dev, uint32_t blockno);
void        bwrite (struct buf *b);
void        brelse (struct buf *b);

void        bread_async (uint32_t dev, uint32_t blockno, io_callback_t cb);
void        bwrite_async(struct buf *b,                  io_callback_t cb);

void        bio_irq_complete(int error);

#endif