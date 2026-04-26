#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include "net.h"


#define VIRTIO_PCI_VENDOR       0x1AF4
#define VIRTIO_PCI_DEV_NET      0x1000   /* legacy virtio-net          */
#define VIRTIO_PCI_DEV_NET_MOD  0x1041   /* modern virtio-net (1.0)    */

/* Virtio PCI capability registers (legacy, BAR0) */
#define VIRTIO_PCI_HOST_FEATURES   0x00
#define VIRTIO_PCI_GUEST_FEATURES  0x04
#define VIRTIO_PCI_QUEUE_PFN       0x08
#define VIRTIO_PCI_QUEUE_SIZE      0x0C
#define VIRTIO_PCI_QUEUE_SELECT    0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10
#define VIRTIO_PCI_STATUS          0x12
#define VIRTIO_PCI_ISR             0x13
/* Device-specific config starts here (for net: MAC + status) */
#define VIRTIO_PCI_NET_MAC         0x14
#define VIRTIO_PCI_NET_STATUS      0x1A

/* Virtio device status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE  0x01
#define VIRTIO_STATUS_DRIVER       0x02
#define VIRTIO_STATUS_DRIVER_OK    0x04
#define VIRTIO_STATUS_FAILED       0x80

/* Virtio-net feature bits */
#define VIRTIO_NET_F_MAC           (1 << 5)
#define VIRTIO_NET_F_STATUS        (1 << 16)

/* Virtio queue indices */
#define VIRTIO_NET_QUEUE_RX  0
#define VIRTIO_NET_QUEUE_TX  1


#define VIRTQ_MAX_SIZE 256   /* must be power of 2 */

/* Descriptor flags */
#define VIRTQ_DESC_F_NEXT     0x1
#define VIRTQ_DESC_F_WRITE    0x2   /* device writes to this buffer  */

typedef struct virtq_desc {
    uint64_t addr;    /* guest physical address */
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_MAX_SIZE];
    uint16_t used_event;
} __attribute__((packed)) virtq_avail_t;

typedef struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VIRTQ_MAX_SIZE];
    uint16_t avail_event;
} __attribute__((packed)) virtq_used_t;


typedef struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed)) virtio_net_hdr_t;

#define VIRTIO_NET_HDR_F_NEEDS_CSUM 1
#define VIRTIO_NET_HDR_GSO_NONE     0

/* ───────────────────────────────────────────────
   Public API
   ─────────────────────────────────────────────── */
void virtio_net_init(void);
void virtio_net_irq_handler(void);

#endif 