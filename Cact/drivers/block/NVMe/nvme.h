#ifndef NVME_H
#define NVME_H

#include <stdint.h>
#include "vfs.h"
#include "buf.h"

// Queue depths
#define NVME_ADMIN_QUEUE_SIZE  16
#define NVME_IO_QUEUE_SIZE     64
#define NVME_SECTOR_SIZE       512

// Admin opcodes
#define NVME_OPC_IDENTIFY     0x06
#define NVME_OPC_CREATE_IOSQ  0x01
#define NVME_OPC_CREATE_IOCQ  0x05
#define NVME_OPC_SET_FEATURES 0x09

// I/O opcodes
#define NVME_IO_READ   0x02
#define NVME_IO_WRITE  0x01
#define NVME_IO_FLUSH  0x00

// 64-byte Submission Queue Entry
struct nvme_sq_entry {
    uint32_t cdw0;
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

// 16-byte Completion Queue Entry
struct nvme_cq_entry {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;       // bit 0 = phase tag, bits 15:1 = status code
} __attribute__((packed));

// Controller registers (MMIO BAR0)
struct nvme_bar {
    uint64_t cap;      // controller capabilities
    uint32_t vs;       // version
    uint32_t intms;    // interrupt mask set
    uint32_t intmc;    // interrupt mask clear
    uint32_t cc;       // controller configuration
    uint32_t rsvd;
    uint32_t csts;     // controller status
    uint32_t nssr;     // NVM subsystem reset
    uint32_t aqa;      // admin queue attributes
    uint64_t asq;      // admin SQ base
    uint64_t acq;      // admin CQ base
} __attribute__((packed));

// Software queue state
struct nvme_queue {
    volatile struct nvme_sq_entry *sq;
    volatile struct nvme_cq_entry *cq;
    volatile uint32_t             *sq_db;    // doorbell register
    volatile uint32_t             *cq_db;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t cq_phase;   // expected phase tag for current CQ entry
    uint16_t depth;
};

// Per-controller state
struct nvme_dev {
    volatile struct nvme_bar *bar;
    uint32_t                  bar_phys;
    uint32_t                  db_stride;    // doorbell stride in bytes
    uint32_t                  ns_id;
    uint32_t                  max_lba;
    struct nvme_queue         admin_q;
    struct nvme_queue         io_q;
    uint8_t                   pci_bus;
    uint8_t                   pci_dev;
    uint8_t                   pci_fn;
};

// Public API
void nvme_init(void);
void nvme_read_sector (uint32_t lba, uint8_t *buf);
void nvme_write_sector(uint32_t lba, uint8_t *buf);
void nvme_irq_handler (void);

int              nvme_read (struct vfs_node *node, uint32_t off, uint32_t sz, char *buf);
int              nvme_write(struct vfs_node *node, uint32_t off, uint32_t sz, char *buf);
struct vfs_node *init_nvme_device(void);

struct buf;
void bio_enqueue_sync(struct buf *b);
void bio_irq_complete(int error);

int      nvme_is_ready   (void);
uint32_t nvme_get_max_lba(void);

#endif