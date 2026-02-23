#ifndef BUF_H
#define BUF_H

#include <stdint.h>

#define B_BUSY  0x1  /* buffer is locked by some process */
#define B_VALID 0x2  /* buffer has been read from disk */
#define B_DIRTY 0x4  /* buffer needs to be written to disk */

struct buf {
    int flags;
    uint32_t dev;
    uint32_t blockno;
    struct buf *prev; // LRU cache list
    struct buf *next;
    struct buf *qnext; // disk queue
    uint8_t data[512];
};

void binit(void);
struct buf* bread(uint32_t dev, uint32_t blockno);
void bwrite(struct buf *b);
void brelse(struct buf *b);

#endif
