#include "virtio_net.h"
#include "net.h"
#include "kernel.h"
#include "memory.h"   

static uint16_t io_base = 0;   /* BAR0 I/O port base */

/* Per-queue bookkeeping */
typedef struct {
    virtq_desc_t*  desc;
    virtq_avail_t* avail;
    virtq_used_t*  used;
    uint16_t       size;
    uint16_t       last_used_idx;
    /* free descriptor list head */
    uint16_t       free_head;
    uint16_t       num_free;
    /* for RX: where the skbs live (one per descriptor) */
    skb_t*         rx_skbs[VIRTQ_MAX_SIZE];
} virtqueue_t;

static virtqueue_t vq_rx;
static virtqueue_t vq_tx;


static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000 | (bus<<16) | (dev<<11) | (fn<<8) | (off & 0xFC);
    port_long_out(0xCF8, addr);
    return (uint16_t)((port_long_in(0xCFC) >> ((off & 2)*8)) & 0xFFFF);
}
/* Scan all 256 buses × 32 devices for vendor:device */
static int pci_find(uint16_t vendor, uint16_t device,
                    uint8_t* out_bus, uint8_t* out_dev) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read32(bus, dev, 0, 0);
            if ((id & 0xFFFF) == vendor && (id >> 16) == device) {
                *out_bus = bus; *out_dev = dev; return 1;
            }
        }
    }
    return 0;
}

/* Align addr up to next multiple of align (must be power of 2) */
static uint32_t align_up(uint32_t addr, uint32_t align) {
    return (addr + align - 1) & ~(align - 1);
}

static void virtq_alloc(virtqueue_t* vq, uint16_t size) {
    vq->size = size;
    vq->last_used_idx = 0;
    vq->free_head     = 0;
    vq->num_free      = size;

    /* Calculate sizes */
    uint32_t desc_sz  = sizeof(virtq_desc_t) * size;
    uint32_t avail_sz = sizeof(uint16_t) * (3 + size);  /* flags+idx+ring+used_event */
    uint32_t used_sz  = sizeof(uint16_t)*3 + sizeof(virtq_used_elem_t)*size;

    uint32_t avail_off = desc_sz;
    uint32_t used_off  = align_up(avail_off + avail_sz, 4096);
    uint32_t total     = used_off + used_sz;

    /* Allocate zeroed memory (page-aligned for virtio) */
    uint8_t* mem = (uint8_t*)kmalloc_aligned(total, 4096);
    /* If you don't have kmalloc_aligned, use kmalloc and align manually */

    vq->desc  = (virtq_desc_t*)  (mem);
    vq->avail = (virtq_avail_t*) (mem + avail_off);
    vq->used  = (virtq_used_t*)  (mem + used_off);

    /* Link free descriptor list */
    for (int i = 0; i < size - 1; i++) {
        vq->desc[i].flags = VIRTQ_DESC_F_NEXT;
        vq->desc[i].next  = i + 1;
    }
    vq->desc[size-1].flags = 0;
    vq->desc[size-1].next  = 0xFFFF;
}

/* Tell the device the physical address of our virtqueue */
static void virtq_activate(virtqueue_t* vq, uint16_t queue_idx) {
    port_word_out(io_base + VIRTIO_PCI_QUEUE_SELECT, queue_idx);
    uint32_t pfn = (uint32_t)(uintptr_t)vq->desc / 4096;
    port_long_out(io_base + VIRTIO_PCI_QUEUE_PFN, pfn);
}

static uint16_t desc_alloc(virtqueue_t* vq) {
    if (vq->num_free == 0) return 0xFFFF;  /* out of descriptors */
    uint16_t idx  = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    return idx;
}

static void desc_free(virtqueue_t* vq, uint16_t idx) {
    vq->desc[idx].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[idx].next  = vq->free_head;
    vq->free_head       = idx;
    vq->num_free++;
}

static void rx_fill(void) {
    /* We use 2 descriptors per buffer: one for the virtio header,
       one for the actual packet payload (write-only for device). */
    while (vq_rx.num_free >= 2) {
        skb_t* skb = skb_alloc();
        if (!skb) break;

        /* Descriptor 0: virtio_net_hdr (device writes here) */
        uint16_t d0 = desc_alloc(&vq_rx);
        uint16_t d1 = desc_alloc(&vq_rx);

        /* Reuse the front of skb->data for the virtio header */
        virtio_net_hdr_t* hdr = (virtio_net_hdr_t*)skb->data;
        skb->data_offset = sizeof(virtio_net_hdr_t);  /* payload starts after */

        vq_rx.desc[d0].addr  = (uint32_t)(uintptr_t)hdr;
        vq_rx.desc[d0].len   = sizeof(virtio_net_hdr_t);
        vq_rx.desc[d0].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
        vq_rx.desc[d0].next  = d1;

        vq_rx.desc[d1].addr  = (uint32_t)(uintptr_t)(skb->data + skb->data_offset);
        vq_rx.desc[d1].len   = SKB_MAX_SIZE - sizeof(virtio_net_hdr_t);
        vq_rx.desc[d1].flags = VIRTQ_DESC_F_WRITE;  /* device writes packet */
        vq_rx.desc[d1].next  = 0;

        vq_rx.rx_skbs[d0] = skb;  /* remember skb by first descriptor idx */

        /* Put the descriptor chain into the available ring */
        uint16_t avail_idx = vq_rx.avail->idx % vq_rx.size;
        vq_rx.avail->ring[avail_idx] = d0;
        __asm__ __volatile__("" ::: "memory");  /* compiler barrier */
        vq_rx.avail->idx++;
    }
    /* Notify device that there are new RX buffers */
    port_word_out(io_base + VIRTIO_PCI_QUEUE_NOTIFY, VIRTIO_NET_QUEUE_RX);
}

static int virtio_net_send(skb_t* skb) {
    if (!skb || vq_tx.num_free < 2) return -1;

    /* Build a zeroed virtio header just before the Ethernet frame.
       skb_push() already handles headroom. */
    virtio_net_hdr_t* hdr = (virtio_net_hdr_t*)skb_push(skb, sizeof(virtio_net_hdr_t));
    hdr->flags      = 0;
    hdr->gso_type   = VIRTIO_NET_HDR_GSO_NONE;
    hdr->hdr_len    = 0;
    hdr->gso_size   = 0;
    hdr->csum_start = 0;
    hdr->csum_offset= 0;

    uint16_t d0 = desc_alloc(&vq_tx);
    /* Single descriptor: virtio_hdr + ethernet frame together */
    vq_tx.desc[d0].addr  = (uint32_t)(uintptr_t)skb_data(skb);
    vq_tx.desc[d0].len   = skb_len(skb);
    vq_tx.desc[d0].flags = 0;  /* read-only for device */
    vq_tx.desc[d0].next  = 0;

    uint16_t avail_idx = vq_tx.avail->idx % vq_tx.size;
    vq_tx.avail->ring[avail_idx] = d0;
    __asm__ __volatile__("" ::: "memory");
    vq_tx.avail->idx++;

    port_word_out(io_base + VIRTIO_PCI_QUEUE_NOTIFY, VIRTIO_NET_QUEUE_TX);
    return 0;
}

static void virtio_net_drain_rx(void) {
    while (vq_rx.last_used_idx != vq_rx.used->idx) {
        uint16_t used_idx = vq_rx.last_used_idx % vq_rx.size;
        virtq_used_elem_t* elem = &vq_rx.used->ring[used_idx];

        uint16_t d0  = (uint16_t)elem->id;
        uint32_t len = elem->len;   /* total bytes written by device (hdr + pkt) */
        vq_rx.last_used_idx++;

        skb_t* skb = vq_rx.rx_skbs[d0];
        if (skb) {
            /* Payload length = total - virtio header */
            skb->total_len = (uint16_t)(len - sizeof(virtio_net_hdr_t));
            vq_rx.rx_skbs[d0] = NULL;

            /* Release descriptors back to free list */
            uint16_t d1 = vq_rx.desc[d0].next;
            desc_free(&vq_rx, d1);
            desc_free(&vq_rx, d0);

            /* Hand up to the network stack */
            net_receive(skb);
        }
    }

    /* Drain TX used ring too (free sent descriptors) */
    while (vq_tx.last_used_idx != vq_tx.used->idx) {
        uint16_t used_idx = vq_tx.last_used_idx % vq_tx.size;
        uint16_t d0 = (uint16_t)vq_tx.used->ring[used_idx].id;
        vq_tx.last_used_idx++;
        desc_free(&vq_tx, d0);
        /* NOTE: caller owns skb memory; we don't free it here.
           For simple TX the caller stack-allocated or will reuse. */
    }

    /* Refill RX ring */
    rx_fill();
}

static void virtio_net_poll(void) {
    virtio_net_drain_rx();
}

void virtio_net_irq_handler(void) {
    /* Ack the ISR register to clear the interrupt */
    port_byte_in(io_base + VIRTIO_PCI_ISR);
    sema_up(&net_sema);
}

static void virtio_net_get_mac(mac_addr_t* out) {
    for (int i = 0; i < 6; i++)
        out->b[i] = port_byte_in(io_base + VIRTIO_PCI_NET_MAC + i);
}

static net_driver_t virtio_driver = {
    .name    = "virtio-net (legacy)",
    .send    = virtio_net_send,
    .poll    = virtio_net_poll,
    .get_mac = virtio_net_get_mac,
};

void virtio_net_init(void) {
    uint8_t bus, dev;

    kprint("[VIRTIO-NET] searching PCI (vendor=1AF4 modern/legacy NIC)\n");
    /* Try modern first, then legacy */
    if (!pci_find(VIRTIO_PCI_VENDOR, VIRTIO_PCI_DEV_NET_MOD, &bus, &dev))
        if (!pci_find(VIRTIO_PCI_VENDOR, VIRTIO_PCI_DEV_NET, &bus, &dev)) {
            kprint("[VIRTIO-NET] Device not found on PCI bus.\n");
            klog(LOG_WARN, "virtio-net: no NIC found");
            return;
        }
    kprint("[VIRTIO-NET] found at bus="); kprint_hex(bus);
    kprint(" dev="); kprint_hex(dev); kprint("\n");

    /* Read BAR0 (I/O space) */
    kprint("[VIRTIO-NET] reading BAR0\n");
    uint32_t bar0 = pci_read32(bus, dev, 0, 0x10);
    if (!(bar0 & 1)) {
        kprint("[VIRTIO-NET] BAR0 is not I/O space — modern virtio needs different init.\n");
        klog(LOG_FAIL, "virtio-net: BAR0 not I/O space");
        return;
    }
    io_base = (uint16_t)(bar0 & ~3);
    kprint("[VIRTIO-NET] BAR0 io_base="); kprint_hex(io_base); kprint("\n");

    /* Enable bus mastering + I/O space in PCI command register */
    kprint("[VIRTIO-NET] enabling bus mastering and I/O space\n");
    uint16_t cmd = pci_read16(bus, dev, 0, 0x04);
    pci_write32(bus, dev, 0, 0x04, cmd | 0x05);

    /* ── Virtio init sequence (legacy, section 3.1 of spec) ── */
    kprint("[VIRTIO-NET] step 1/6: reset device (STATUS=0)\n");
    port_byte_out(io_base + VIRTIO_PCI_STATUS, 0);
    kprint("[VIRTIO-NET] step 2/6: set ACKNOWLEDGE\n");
    port_byte_out(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    kprint("[VIRTIO-NET] step 3/6: set DRIVER\n");
    port_byte_out(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    kprint("[VIRTIO-NET] step 4/6: negotiate features (MAC + STATUS)\n");
    uint32_t host_feat = port_long_in(io_base + VIRTIO_PCI_HOST_FEATURES);
    uint32_t our_feat  = host_feat & (VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS);
    port_long_out(io_base + VIRTIO_PCI_GUEST_FEATURES, our_feat);
    kprint("[VIRTIO-NET] step 5/6: allocate and activate virtqueues (RX+TX)\n");
    virtq_alloc(&vq_rx, VIRTQ_MAX_SIZE);
    virtq_alloc(&vq_tx, VIRTQ_MAX_SIZE);
    virtq_activate(&vq_rx, VIRTIO_NET_QUEUE_RX);
    virtq_activate(&vq_tx, VIRTIO_NET_QUEUE_TX);
    kprint("[VIRTIO-NET] step 6/6: set DRIVER_OK\n");
    port_byte_out(io_base + VIRTIO_PCI_STATUS,
                  VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    /* Read MAC */
    virtio_net_get_mac(&virtio_driver.mac);
    kprint("[VIRTIO-NET] MAC: ");
    for (int i = 0; i < 6; i++) {
        kprint_hex(virtio_driver.mac.b[i]);
        if (i < 5) kprint(":");
    }
    kprint("\n");

    /* Register with the stack */
    kprint("[VIRTIO-NET] registering driver with network stack\n");
    net_register_driver(&virtio_driver);

    kprint("[VIRTIO-NET] filling RX queue with buffers\n");
    rx_fill();
    klog(LOG_OK, "virtio-net ready");
}
