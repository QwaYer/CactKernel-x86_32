#ifndef BUF_H
#define BUF_H

#include <stdint.h>

#define B_BUSY    0x01 
#define B_VALID   0x02  
#define B_DIRTY   0x04  
#define B_QUEUED  0x08   
#define B_ERROR   0x10   

struct prdt_entry {
    uint32_t phys_addr;   
    uint16_t byte_count;  
    uint16_t flags;       
} __attribute__((packed));

#define PRDT_EOT  0x8000

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

    struct prdt_entry prdt[1] __attribute__((aligned(4)));

    uint8_t  data[512]        __attribute__((aligned(512)));
};


//Public api
void        binit  (void);
struct buf *bread  (uint32_t dev, uint32_t blockno);
void        bwrite (struct buf *b);
void        brelse (struct buf *b);

void        bread_async (uint32_t dev, uint32_t blockno, io_callback_t cb);
void        bwrite_async(struct buf *b,                  io_callback_t cb);

void        bio_irq_complete(int error);

#endif