#ifndef BUF_H
#define BUF_H

#include <stdint.h>

#define B_BUSY    0x01   /* буфер захвачен                     */
#define B_VALID   0x02   /* данные прочитаны с диска           */
#define B_DIRTY   0x04   /* нужна запись на диск               */
#define B_QUEUED  0x08   /* запрос поставлен в очередь DMA     */
#define B_ERROR   0x10   /* последняя операция завершилась с ошибкой */

struct prdt_entry {
    uint32_t phys_addr;   /* физический адрес буфера           */
    uint16_t byte_count;  /* размер в байтах (0 = 64 KiB)      */
    uint16_t flags;       /* бит 15 = EOT (End Of Table)        */
} __attribute__((packed));

#define PRDT_EOT  0x8000

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

/* ── Публичный API ────────────────────────────────────────── */
void        binit  (void);
struct buf *bread  (uint32_t dev, uint32_t blockno);
void        bwrite (struct buf *b);
void        brelse (struct buf *b);

void        bread_async (uint32_t dev, uint32_t blockno, io_callback_t cb);
void        bwrite_async(struct buf *b,                  io_callback_t cb);

void        bio_irq_complete(int error);

#endif 