#include "nvme.h"
#include "pci.h"
#include "buf.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "libc.h"

static struct nvme_dev ndev;
static int nvme_ready = 0;

static struct buf *disk_queue      = 0;
static struct buf *disk_queue_tail = 0;

static volatile uint16_t admin_cid = 0;
static volatile uint16_t io_cid    = 0;

static uint8_t admin_sq_mem[sizeof(struct nvme_sq_entry) * NVME_ADMIN_QUEUE_SIZE] __attribute__((aligned(4096)));
static uint8_t admin_cq_mem[sizeof(struct nvme_cq_entry) * NVME_ADMIN_QUEUE_SIZE] __attribute__((aligned(4096)));
static uint8_t io_sq_mem   [sizeof(struct nvme_sq_entry) * NVME_IO_QUEUE_SIZE]    __attribute__((aligned(4096)));
static uint8_t io_cq_mem   [sizeof(struct nvme_cq_entry) * NVME_IO_QUEUE_SIZE]    __attribute__((aligned(4096)));
static uint8_t identify_buf[4096] __attribute__((aligned(4096)));

static void nvme_wait_ready(int expected) {
    for (int i = 0; i < 1000000; i++)
        if (((ndev.bar->csts >> 0) & 1) == (uint32_t)expected)
            return;
    kprint("[NVMe] controller timeout\n");
}

static void admin_submit(struct nvme_sq_entry *cmd) {
    struct nvme_queue *q = &ndev.admin_q;
    volatile struct nvme_sq_entry *dst = &q->sq[q->sq_tail];
    memory_copy((void *)dst, cmd, sizeof(*cmd));
    q->sq_tail = (q->sq_tail + 1) % q->depth;
    *q->sq_db = q->sq_tail;
}

static int admin_poll(void) {
    struct nvme_queue *q = &ndev.admin_q;
    for (int i = 0; i < 2000000; i++) {
        volatile struct nvme_cq_entry *cqe = &q->cq[q->cq_head];
        if ((cqe->status & 1) == q->cq_phase) {
            uint16_t raw = cqe->status;
            uint16_t status = raw >> 1;
            q->cq_head = (q->cq_head + 1) % q->depth;
            if (q->cq_head == 0) q->cq_phase ^= 1;
            *q->cq_db = q->cq_head;
            if (status & 0x7FF) {
                char tmp[16];
                kprint("[NVMe] admin err raw=");
                kprint_hex(raw);
                kprint(" SC=");
                itoa(status & 0xFF, tmp); kprint(tmp);
                kprint(" SCT=");
                itoa((status >> 8) & 0x7, tmp); kprint(tmp);
                kprint("\n");
            }
            return (status & 0x7FF) ? -1 : 0;
        }
    }
    kprint("[NVMe] admin_poll TIMEOUT\n");
    return -1;
}

static int admin_cmd(struct nvme_sq_entry *cmd) {
    admin_submit(cmd);
    return admin_poll();
}

static void io_submit_rw(struct buf *b) {
    int is_write = (b->flags & B_DIRTY);
    struct nvme_queue *q = &ndev.io_q;

    struct nvme_sq_entry cmd;
    memory_set(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = (is_write ? NVME_IO_WRITE : NVME_IO_READ)
             | ((uint32_t)(io_cid++ & 0xFFFF) << 16);
    cmd.nsid = ndev.ns_id;
    cmd.prp1 = (uint32_t)b->data;
    cmd.cdw10 = b->blockno;
    cmd.cdw11 = 0;
    cmd.cdw12 = 0;

    volatile struct nvme_sq_entry *dst = &q->sq[q->sq_tail];
    memory_copy((void *)dst, &cmd, sizeof(cmd));
    q->sq_tail = (q->sq_tail + 1) % q->depth;
    *q->sq_db = q->sq_tail;
}

void bio_enqueue_sync(struct buf *b) {
    b->qnext = 0;
    b->flags |= B_QUEUED;
    if (!disk_queue) {
        disk_queue = disk_queue_tail = b;
        io_submit_rw(b);
    } else {
        disk_queue_tail->qnext = b;
        disk_queue_tail = b;
    }
}

void nvme_irq_handler(void) {
    struct nvme_queue *q = &ndev.io_q;

    while (1) {
        volatile struct nvme_cq_entry *cqe = &q->cq[q->cq_head];
        if ((cqe->status & 1) != q->cq_phase) break;

        uint16_t status = cqe->status >> 1;
        int error = (status & 0x7FF) ? 1 : 0;

        q->cq_head = (q->cq_head + 1) % q->depth;
        if (q->cq_head == 0) q->cq_phase ^= 1;
        *q->cq_db = q->cq_head;

        bio_irq_complete(error);
    }
}

void bio_irq_complete(int error) {
    if (!disk_queue) return;

    struct buf *done = disk_queue;
    disk_queue = done->qnext;
    if (!disk_queue) disk_queue_tail = 0;

    done->flags &= ~B_QUEUED;
    if (error) {
        done->flags |= B_ERROR;
    } else {
        done->flags |= B_VALID;
        done->flags &= ~B_DIRTY;
    }

    if (done->callback)
        done->callback(done, error);

    if (disk_queue)
        io_submit_rw(disk_queue);
}

static int nvme_polled_rw(uint32_t lba, uint8_t *buf, int write) {
    if (!nvme_ready) {
        kprint("[NVMe] read_sector but nvme not ready\n");
        return -1;
    }
    struct nvme_queue *q = &ndev.io_q;

    struct nvme_sq_entry cmd;
    memory_set(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = (write ? NVME_IO_WRITE : NVME_IO_READ)
              | ((uint32_t)(io_cid++ & 0xFFFF) << 16);
    cmd.nsid  = ndev.ns_id;
    cmd.prp1  = (uint32_t)buf;
    cmd.cdw10 = lba;
    cmd.cdw11 = 0;
    cmd.cdw12 = 0;

    volatile struct nvme_sq_entry *dst = &q->sq[q->sq_tail];
    memory_copy((void *)dst, &cmd, sizeof(cmd));
    q->sq_tail = (q->sq_tail + 1) % q->depth;
    *q->sq_db  = q->sq_tail;

    for (int i = 0; i < 2000000; i++) {
        volatile struct nvme_cq_entry *cqe = &q->cq[q->cq_head];
        if ((cqe->status & 1) == q->cq_phase) {
            uint16_t status = cqe->status >> 1;
            q->cq_head = (q->cq_head + 1) % q->depth;
            if (q->cq_head == 0) q->cq_phase ^= 1;
            *q->cq_db = q->cq_head;
            return (status & 0x7FF) ? -1 : 0;
        }
    }
    return -1;
}

void nvme_read_sector(uint32_t lba, uint8_t *buf) {
    if (nvme_polled_rw(lba, buf, 0) < 0)
        memory_set(buf, 0, NVME_SECTOR_SIZE);
}

void nvme_write_sector(uint32_t lba, uint8_t *buf) {
    nvme_polled_rw(lba, buf, 1);
}

static void nvme_setup_admin_queues(void) {
    memory_set(admin_sq_mem, 0, sizeof(admin_sq_mem));
    memory_set(admin_cq_mem, 0, sizeof(admin_cq_mem));

    ndev.admin_q.sq       = (volatile struct nvme_sq_entry *)admin_sq_mem;
    ndev.admin_q.cq       = (volatile struct nvme_cq_entry *)admin_cq_mem;
    ndev.admin_q.sq_tail  = 0;
    ndev.admin_q.cq_head  = 0;
    ndev.admin_q.cq_phase = 1;
    ndev.admin_q.depth    = NVME_ADMIN_QUEUE_SIZE;

    uint32_t stride = ndev.db_stride;
    ndev.admin_q.sq_db = (volatile uint32_t *)((uint8_t *)ndev.bar + 0x1000 + 0 * stride);
    ndev.admin_q.cq_db = (volatile uint32_t *)((uint8_t *)ndev.bar + 0x1000 + 1 * stride);

    ndev.bar->aqa = ((NVME_ADMIN_QUEUE_SIZE - 1) << 16) | (NVME_ADMIN_QUEUE_SIZE - 1);
    ndev.bar->asq = (uint32_t)admin_sq_mem;
    ndev.bar->acq = (uint32_t)admin_cq_mem;
}

static int nvme_set_num_queues(void) {
    struct nvme_sq_entry cmd;
    memory_set(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = NVME_OPC_SET_FEATURES | ((uint32_t)(admin_cid++ & 0xFFFF) << 16);
    cmd.cdw10 = 0x07;
    cmd.cdw11 = (0 << 16) | 0;
    if (admin_cmd(&cmd) < 0) {
        kprint("[NVMe] Set Features (Num Queues) failed\n");
        return -1;
    }
    return 0;
}

static int nvme_create_io_queues(void) {
    memory_set(io_sq_mem, 0, sizeof(io_sq_mem));
    memory_set(io_cq_mem, 0, sizeof(io_cq_mem));

    ndev.io_q.sq       = (volatile struct nvme_sq_entry *)io_sq_mem;
    ndev.io_q.cq       = (volatile struct nvme_cq_entry *)io_cq_mem;
    ndev.io_q.sq_tail  = 0;
    ndev.io_q.cq_head  = 0;
    ndev.io_q.cq_phase = 1;
    ndev.io_q.depth    = NVME_IO_QUEUE_SIZE;

    uint32_t stride = ndev.db_stride;
    ndev.io_q.sq_db = (volatile uint32_t *)((uint8_t *)ndev.bar + 0x1000 + 2 * stride);
    ndev.io_q.cq_db = (volatile uint32_t *)((uint8_t *)ndev.bar + 0x1000 + 3 * stride);

    struct nvme_sq_entry cmd;

    memory_set(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = NVME_OPC_CREATE_IOCQ | ((uint32_t)(admin_cid++ & 0xFFFF) << 16);
    cmd.prp1  = (uint32_t)io_cq_mem;
    cmd.cdw10 = ((NVME_IO_QUEUE_SIZE - 1) << 16) | 1;
    cmd.cdw11 = 0x03;
    if (admin_cmd(&cmd) < 0) {
        kprint("[NVMe] create IO CQ failed\n");
        return -1;
    }

    memory_set(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = NVME_OPC_CREATE_IOSQ | ((uint32_t)(admin_cid++ & 0xFFFF) << 16);
    cmd.prp1  = (uint32_t)io_sq_mem;
    cmd.cdw10 = ((NVME_IO_QUEUE_SIZE - 1) << 16) | 1;
    cmd.cdw11 = (1 << 16) | 0x01;
    if (admin_cmd(&cmd) < 0) {
        kprint("[NVMe] create IO SQ failed\n");
        return -1;
    }

    return 0;
}

static int nvme_identify(void) {
    memory_set(identify_buf, 0, sizeof(identify_buf));

    struct nvme_sq_entry cmd;
    memory_set(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = NVME_OPC_IDENTIFY | ((uint32_t)(admin_cid++ & 0xFFFF) << 16);
    cmd.prp1  = (uint32_t)identify_buf;
    cmd.cdw10 = 1;
    if (admin_cmd(&cmd) < 0) {
        kprint("[NVMe] identify controller failed\n");
        return -1;
    }

    memory_set(identify_buf, 0, sizeof(identify_buf));
    memory_set(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = NVME_OPC_IDENTIFY | ((uint32_t)(admin_cid++ & 0xFFFF) << 16);
    cmd.nsid  = 1;
    cmd.prp1  = (uint32_t)identify_buf;
    cmd.cdw10 = 0;
    if (admin_cmd(&cmd) < 0) {
        kprint("[NVMe] identify namespace failed\n");
        return -1;
    }

    uint32_t *id32 = (uint32_t *)identify_buf;
    ndev.ns_id   = 1;
    ndev.max_lba = id32[0];

    return 0;
}

void nvme_init(void) {
    binit();

    memory_set(&ndev, 0, sizeof(ndev));
    nvme_ready = 0;

    int found = pci_find_nvme(&ndev.pci_bus, &ndev.pci_dev, &ndev.pci_fn);
    if (!found) {
        kprint("[NVMe] no NVMe controller found\n");
        return;
    }

    uint32_t bar0 = pci_read32(ndev.pci_bus, ndev.pci_dev, ndev.pci_fn, 0x10);
    ndev.bar_phys = bar0 & 0xFFFFF000;

    uint32_t bar_size = 0x4000;
    for (uint32_t off = 0; off < bar_size; off += 0x1000)
        vmm_map(get_current_pd(), ndev.bar_phys + off, ndev.bar_phys + off,
                PAGE_PRESENT | PAGE_RW);

    ndev.bar = (volatile struct nvme_bar *)(uintptr_t)ndev.bar_phys;

    uint32_t pcicmd = pci_read32(ndev.pci_bus, ndev.pci_dev, ndev.pci_fn, 0x04);
    pci_write32(ndev.pci_bus, ndev.pci_dev, ndev.pci_fn, 0x04, pcicmd | 0x06);

    uint64_t cap = ndev.bar->cap;
    ndev.db_stride = 4 << ((cap >> 32) & 0xF);

    ndev.bar->cc = 0;
    nvme_wait_ready(0);

    nvme_setup_admin_queues();

    // CC: IOCQES=4 [23:20], IOSQES=6 [19:16], MPS=0 [10:7], CSS=0 [6:4], EN=1 [0]
    ndev.bar->cc = (4 << 20) | (6 << 16) | (0 << 7) | 1;
    nvme_wait_ready(1);

    if (ndev.bar->csts & 0x2) {
        kprint("[NVMe] controller fatal error (CFS)\n");
        return;
    }

    if (nvme_identify() < 0) return;
    if (nvme_set_num_queues() < 0) return;
    if (nvme_create_io_queues() < 0) return;

    nvme_ready = 1;

}

//public api
int nvme_read(vfs_node_t *node, uint32_t offset, uint32_t size, char *buffer) {
    (void)node;
    struct buf *b = bread(0, offset / 512);
    if (!b) return 0;
    for (unsigned int i = 0; i < size; i++) buffer[i] = b->data[i];
    brelse(b);
    return size;
}

int nvme_write(vfs_node_t *node, uint32_t offset, uint32_t size, char *buffer) {
    (void)node;
    struct buf *b = bread(0, offset / 512);
    if (!b) return 0;
    for (unsigned int i = 0; i < size; i++) b->data[i] = buffer[i];
    bwrite(b);
    brelse(b);
    return size;
}

static vfs_ops_t nvme_ops = {
    .read  = nvme_read,
    .write = nvme_write,
};

vfs_node_t *init_nvme_device(void) {
    vfs_node_t *node = (vfs_node_t *)kalloc();
    if (!node) return 0;
    memory_set(node->name, 0, 128);
    node->name[0] = 'n'; node->name[1] = 'v'; node->name[2] = 'm';
    node->name[3] = 'e'; node->name[4] = '0';
    node->type = VFS_BLOCKDEVICE;
    node->ops  = &nvme_ops;
    return node;
}