#include "buf.h"
#include "ata.h"
#include "kernel.h"
#include "memory.h"

#define NBUF 30

struct {
    struct buf buf[NBUF];
    struct buf head; // LRU cache list
} bcache;

void binit(void) {
    struct buf *b;

    bcache.head.prev = &bcache.head;
    bcache.head.next = &bcache.head;
    for(b = bcache.buf; b < bcache.buf + NBUF; b++){
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }
}

static struct buf* bget(uint32_t dev, uint32_t blockno) {
    struct buf *b;

    // Is the block already cached?
    for(b = bcache.head.next; b != &bcache.head; b = b->next){
        if(b->dev == dev && b->blockno == blockno){
            if(!(b->flags & B_BUSY)){
                b->flags |= B_BUSY;
                return b;
            }
            // In a real OS, we would sleep here waiting for the buffer to become free.
            // For now, we just return NULL or panic if it's busy.
            kprint("bget: block is busy\n");
            return 0;
        }
    }

    // Not cached; find an unused buffer.
    // Start from the tail (least recently used).
    for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
        if((b->flags & B_BUSY) == 0 && (b->flags & B_DIRTY) == 0){
            b->dev = dev;
            b->blockno = blockno;
            b->flags = B_BUSY;
            return b;
        }
    }
    kprint("bget: no buffers\n");
    return 0;
}

struct buf* bread(uint32_t dev, uint32_t blockno) {
    struct buf *b;

    b = bget(dev, blockno);
    if(!b) return 0;

    if(!(b->flags & B_VALID)) {
        // TODO: Use dev to determine port and slave
        ata_read_sector(0x1F0, 0, b->blockno, b->data);
        b->flags |= B_VALID;
    }
    return b;
}

void bwrite(struct buf *b) {
    if((b->flags & B_BUSY) == 0) {
        kprint("bwrite: buffer not busy\n");
        return;
    }
    b->flags |= B_DIRTY;
    // TODO: Use dev to determine port and slave
    ata_write_sector(0x1F0, 0, b->blockno, b->data);
    b->flags &= ~B_DIRTY;
}

void brelse(struct buf *b) {
    if((b->flags & B_BUSY) == 0) {
        kprint("brelse: buffer not busy\n");
        return;
    }

    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;

    b->flags &= ~B_BUSY;
}
