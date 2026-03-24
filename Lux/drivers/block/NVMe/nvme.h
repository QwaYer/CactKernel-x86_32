#ifndef NVME_H
#define NVME_H

#include <stdint.h>
#include "vfs.h"
#include "buf.h"

#define NVME_ADMIN_QUEUE_SIZE  16
#define NVME_IO_QUEUE_SIZE     64
#define NVME_SECTOR_SIZE       512

#define NVME_OPC_IDENTIFY     0x06
#define NVME_OPC_CREATE_IOSQ  0x01
#define NVME_OPC_CREATE_IOCQ  0x05
#define NVME_OPC_SET_FEATURES 0x09

#define NVME_IO_READ   0x02
#define NVME_IO_WRITE  0x01
#define NVME_IO_FLUSH  0x00

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

struct nvme_cq_entry {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed));

struct nvme_bar {
    uint64_t cap;
    uint32_t vs;
    uint32_t intms;
    uint32_t intmc;
    uint32_t cc;
    uint32_t rsvd;
    uint32_t csts;
    uint32_t nssr;
    uint32_t aqa;
    uint64_t asq;
    uint64_t acq;
} __attribute__((packed));

struct nvme_queue {
    volatile struct nvme_sq_entry *sq;
    volatile struct nvme_cq_entry *cq;
    volatile uint32_t             *sq_db;
    volatile uint32_t             *cq_db;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t cq_phase;
    uint16_t depth;
};

struct nvme_dev {
    volatile struct nvme_bar *bar;
    uint32_t                  bar_phys;
    uint32_t                  db_stride;
    uint32_t                  ns_id;
    uint32_t                  max_lba;
    struct nvme_queue         admin_q;
    struct nvme_queue         io_q;
    uint8_t                   pci_bus;
    uint8_t                   pci_dev;
    uint8_t                   pci_fn;
};

//public api
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