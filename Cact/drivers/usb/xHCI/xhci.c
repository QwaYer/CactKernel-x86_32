#include "xhci.h"
#include "usb.h"
#include "pci_enum.h"
#include "pci_driver.h"
#include "pci.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "sync.h"

static usb_hc_t *xhci_hc_list = NULL;
static irq_spinlock_t xhci_evt_lock;

extern uint32_t page_directory[1024];

static inline uint32_t xhci_va_to_pa(void *va) {
    return vmm_get_phys(page_directory, (uint32_t)va);
}

static inline void xhci_udelay(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 50; i++)
        __asm__ __volatile__("pause");
}

static inline uint32_t xhci_cap_read32(xhci_priv_t *p, uint32_t off) {
    return p->cap[off / 4];
}

static inline uint32_t xhci_op_read32(xhci_priv_t *p, uint32_t off) {
    return p->op[off / 4];
}

static inline void xhci_op_write32(xhci_priv_t *p, uint32_t off, uint32_t val) {
    p->op[off / 4] = val;
}

static inline uint32_t xhci_rt_read32(xhci_priv_t *p, uint32_t off) {
    return p->rt[off / 4];
}

static inline void xhci_rt_write32(xhci_priv_t *p, uint32_t off, uint32_t val) {
    p->rt[off / 4] = val;
}

static inline void xhci_db_write32(xhci_priv_t *p, uint32_t idx, uint32_t val) {
    extern void xhci_unmask_irq11(void);
    xhci_unmask_irq11();
    p->db[idx] = val;
}

static inline uint32_t xhci_portsc_off(uint8_t port) {
    return XHCI_OP_PORTSC_BASE + (port * 0x10);
}

static inline uint32_t xhci_portsc_read(xhci_priv_t *p, uint8_t port) {
    return xhci_op_read32(p, xhci_portsc_off(port));
}

#define XHCI_PORTSC_RW1C_BITS (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_PRC | (1u<<19) | (1u<<20) | (1u<<22) | (1u<<23))

static inline void xhci_portsc_write(xhci_priv_t *p, uint8_t port, uint32_t val) {
    xhci_op_write32(p, xhci_portsc_off(port), val);
}

static inline void xhci_portsc_set(xhci_priv_t *p, uint8_t port, uint32_t bits) {
    uint32_t sc = xhci_portsc_read(p, port);
    sc = (sc & ~XHCI_PORTSC_RW1C_BITS) | bits;
    xhci_portsc_write(p, port, sc);
}

static inline void xhci_portsc_clear_change(xhci_priv_t *p, uint8_t port, uint32_t bits) {
    uint32_t sc = xhci_portsc_read(p, port);
    sc = (sc & ~XHCI_PORTSC_RW1C_BITS) | bits;
    xhci_portsc_write(p, port, sc);
}

static void xhci_ring_init(xhci_ring_t *ring, xhci_trb_t *mem, uint32_t size) {
    ring->ring    = mem;
    ring->enqueue = 0;
    ring->cycle   = 1;
    ring->size    = size;
    memset(mem, 0, size * sizeof(xhci_trb_t));
    xhci_trb_t *link = &mem[size - 1];
    link->param_lo = xhci_va_to_pa(mem);
    link->param_hi = 0;
    link->status   = 0;
    link->control  = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_CYCLE | (1u << 1);
}

static void xhci_ring_enqueue(xhci_ring_t *ring, xhci_trb_t *trb) {
    uint32_t idx = ring->enqueue;
    xhci_trb_t *dst = &ring->ring[idx];

    dst->param_lo = trb->param_lo;
    dst->param_hi = trb->param_hi;
    dst->status   = trb->status;

    uint32_t ctrl = trb->control & ~XHCI_TRB_CYCLE;
    if (ring->cycle)
        ctrl |= XHCI_TRB_CYCLE;
    dst->control = ctrl;

    ring->enqueue++;
    if (ring->enqueue >= ring->size - 1) {
        xhci_trb_t *link = &ring->ring[ring->size - 1];
        uint32_t lc = link->control & ~XHCI_TRB_CYCLE;
        if (ring->cycle)
            lc |= XHCI_TRB_CYCLE;
        link->control = lc;
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
}

static void xhci_process_event(xhci_priv_t *priv, xhci_trb_t *evt);

static void xhci_drain_events(xhci_priv_t *priv) {
    int processed = 0;

    while (1) {
        xhci_trb_t *evt = &priv->evt_ring[priv->evt_dequeue];
        uint32_t c = (evt->control & XHCI_TRB_CYCLE) ? 1 : 0;
        if (c != priv->evt_cycle) break;

        xhci_process_event(priv, evt);
        processed++;

        priv->evt_dequeue++;
        if (priv->evt_dequeue >= XHCI_EVT_RING_SIZE) {
            priv->evt_dequeue = 0;
            priv->evt_cycle ^= 1;
        }
    }

    if (processed) {
        uint32_t erdp = xhci_va_to_pa(&priv->evt_ring[priv->evt_dequeue]);
        xhci_rt_write32(priv, 0x20 + XHCI_ERDP, erdp | (1u << 3));
        xhci_rt_write32(priv, 0x20 + XHCI_ERDP + 4, 0);
    }
}

static int xhci_wait_cmd(xhci_priv_t *priv, uint32_t timeout_ms) {
    uint32_t loops = timeout_ms * 100;
    while (loops--) {
        if (priv->cmd_done || priv->transfer_done) {
            priv->cmd_done = 0;
            priv->transfer_done = 0;
            return priv->cmd_error ? -1 : 0;
        }
        irq_spinlock_acquire(&xhci_evt_lock);
        xhci_drain_events(priv);
        irq_spinlock_release(&xhci_evt_lock);
        if (priv->cmd_done || priv->transfer_done) {
            priv->cmd_done = 0;
            priv->transfer_done = 0;
            return priv->cmd_error ? -1 : 0;
        }
        xhci_udelay(10);
    }
    klog(LOG_WARN, "xHCI command timeout");
    priv->cmd_done = 0;
    priv->transfer_done = 0;
    return -1;
}

static int xhci_send_cmd(xhci_priv_t *priv, xhci_trb_t *trb) {
    priv->cmd_done   = 0;
    priv->cmd_error  = 0;
    priv->cmd_result = 0;
    xhci_ring_enqueue(&priv->cmd_ring, trb);
    xhci_db_write32(priv, 0, 0);
    return xhci_wait_cmd(priv, 500);
}

static int xhci_enable_slot(xhci_priv_t *priv, uint8_t *slot_id) {
    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.control = (XHCI_TRB_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT);
    if (xhci_send_cmd(priv, &trb) < 0) {
        klog(LOG_WARN, "xHCI enable_slot failed");
        return -1;
    }
    *slot_id = (uint8_t)((priv->cmd_result >> 24) & 0xFF);
    if (*slot_id == 0 || *slot_id > priv->max_slots || *slot_id > XHCI_MAX_SLOTS) {
        klog(LOG_WARN, "xHCI invalid slot ID ");
        { char _nb[8]; itoa(*slot_id, _nb); kprint(_nb); }
        return -1;
    }
    return 0;
}

static uint8_t *xhci_get_dev_ctx(xhci_priv_t *priv, uint8_t slot) {
    if (slot > priv->max_slots) return NULL;
    return priv->dev_ctx_pool + (uint32_t)slot * 2048;
}

static uint8_t *xhci_get_input_ctx(xhci_priv_t *priv) {
    return priv->input_ctx_pool;
}

static void xhci_setup_ep_ring(xhci_priv_t *priv, uint8_t slot, uint8_t dci) {
    xhci_ring_t *ring = &priv->ep_rings[slot][dci - 1];
    xhci_trb_t *mem = (xhci_trb_t *)kmalloc_aligned(XHCI_EP_RING_SIZE * sizeof(xhci_trb_t), 64);
    if (!mem) {
        klog(LOG_WARN, "xHCI endpoint ring allocation failed");
        return;
    }
    if (ring->ring)
        kfree_aligned(ring->ring);
    xhci_ring_init(ring, mem, XHCI_EP_RING_SIZE);
}

static int xhci_address_device(xhci_priv_t *priv, uint8_t slot, uint8_t port,
                                uint8_t speed, int bsr) {
    if (slot == 0 || slot > priv->max_slots || slot > XHCI_MAX_SLOTS)
        return -1;

    spinlock_acquire(&priv->ctx_lock);
    uint8_t *input = xhci_get_input_ctx(priv);
    memset(input, 0, 2048);

    xhci_input_ctrl_ctx_t *icc = (xhci_input_ctrl_ctx_t *)input;
    icc->add_flags = (1u << 0) | (1u << 1);

    uint32_t ctx_off = priv->context_size;
    xhci_slot_ctx_t *slot_ctx = (xhci_slot_ctx_t *)(input + ctx_off);

    uint8_t xhci_speed;
    switch (speed) {
        case USB_SPEED_LOW:  xhci_speed = 2; break;
        case USB_SPEED_FULL: xhci_speed = 1; break;
        case USB_SPEED_HIGH: xhci_speed = 3; break;
        default:             xhci_speed = 4; break;
    }

    slot_ctx->ctx[0] = (1u << 27) | ((uint32_t)xhci_speed << 20);
    slot_ctx->ctx[1] = ((uint32_t)(port + 1) << 16);

    xhci_ep_ctx_t *ep0 = (xhci_ep_ctx_t *)(input + ctx_off * 2);
    uint16_t mps;
    switch (speed) {
        case USB_SPEED_LOW:  mps = 8;   break;
        case USB_SPEED_FULL: mps = 8;   break;
        case USB_SPEED_HIGH: mps = 64;  break;
        default:             mps = 512; break;
    }

    xhci_setup_ep_ring(priv, slot, 1);
    xhci_ring_t *ep0_ring = &priv->ep_rings[slot][0];

    ep0->ctx[1] = (XHCI_EP_CTX_TYPE_CTRL_BI << 3) | (3u << 1) | ((uint32_t)mps << 16);
    ep0->ctx[2] = xhci_va_to_pa(ep0_ring->ring) | ep0_ring->cycle;
    ep0->ctx[3] = 0;

    uint8_t *dev_ctx = xhci_get_dev_ctx(priv, slot);
    memset(dev_ctx, 0, 2048);
    priv->dcbaa[slot] = xhci_va_to_pa(dev_ctx);

    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.param_lo = xhci_va_to_pa(input);
    trb.param_hi = 0;
    trb.control  = (XHCI_TRB_ADDRESS_DEV << XHCI_TRB_TYPE_SHIFT)
                 | ((uint32_t)slot << 24)
                 | (bsr ? XHCI_TRB_BSR : 0);

    int ret = xhci_send_cmd(priv, &trb);
    spinlock_release(&priv->ctx_lock);
    return ret;
}

static int xhci_configure_endpoint(xhci_priv_t *priv, uint8_t slot,
                                    uint8_t dci, uint8_t ep_type,
                                    uint16_t mps, uint8_t interval) {
    spinlock_acquire(&priv->ctx_lock);
    uint8_t *input = xhci_get_input_ctx(priv);
    memset(input, 0, 2048);

    xhci_input_ctrl_ctx_t *icc = (xhci_input_ctrl_ctx_t *)input;
    icc->add_flags = (1u << 0) | (1u << dci);

    uint32_t ctx_off = priv->context_size;
    xhci_slot_ctx_t *slot_ctx = (xhci_slot_ctx_t *)(input + ctx_off);
    uint8_t *dev_raw = xhci_get_dev_ctx(priv, slot);
    xhci_slot_ctx_t *old_slot = (xhci_slot_ctx_t *)dev_raw;

    slot_ctx->ctx[0] = (old_slot->ctx[0] & ~(0x1Fu << 27))
                      | ((uint32_t)dci << 27);
    for (int i = 1; i < 8; i++) slot_ctx->ctx[i] = old_slot->ctx[i];

    xhci_ep_ctx_t *ep = (xhci_ep_ctx_t *)(input + ctx_off * (dci + 1));

    xhci_setup_ep_ring(priv, slot, dci);
    xhci_ring_t *ring = &priv->ep_rings[slot][dci - 1];

    ep->ctx[0] = ((uint32_t)interval << 16);
    ep->ctx[1] = ((uint32_t)ep_type << 3) | (3u << 1) | ((uint32_t)mps << 16);
    ep->ctx[2] = xhci_va_to_pa(ring->ring) | ring->cycle;
    ep->ctx[3] = 0;
    ep->ctx[4] = (uint32_t)mps;

    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.param_lo = xhci_va_to_pa(input);
    trb.param_hi = 0;
    trb.control  = (XHCI_TRB_CONFIG_EP << XHCI_TRB_TYPE_SHIFT)
                 | ((uint32_t)slot << 24);

    int ret = xhci_send_cmd(priv, &trb);
    spinlock_release(&priv->ctx_lock);
    return ret;
}

static int xhci_control_transfer(usb_hc_t *hc, usb_device_t *dev,
                                  usb_setup_pkt_t *setup,
                                  void *data, uint16_t len) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
    uint8_t slot = dev->address;
    if (!slot || slot > XHCI_MAX_SLOTS) return -1;

    xhci_ring_t *ring = &priv->ep_rings[slot][0];
    if (!ring->ring) return -1;

    xhci_trb_t trb;

    memset(&trb, 0, sizeof(trb));
    trb.param_lo = ((uint32_t)setup->wValue << 16) | ((uint32_t)setup->bRequest << 8)
                 | (uint32_t)setup->bmRequestType;
    trb.param_hi = ((uint32_t)setup->wLength << 16) | (uint32_t)setup->wIndex;
    trb.status   = 8;
    trb.control  = (XHCI_TRB_SETUP << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IDT
                 | (len ? (2u << 16) : 0);

    if (setup->bmRequestType & 0x80)
        trb.control |= (3u << 16);

    xhci_ring_enqueue(ring, &trb);

    if (len > 0 && data) {
        memset(&trb, 0, sizeof(trb));
        trb.param_lo = xhci_va_to_pa(data);
        trb.param_hi = 0;
        trb.status   = len;
        trb.control  = (XHCI_TRB_DATA << XHCI_TRB_TYPE_SHIFT);
        if (setup->bmRequestType & 0x80)
            trb.control |= XHCI_TRB_DIR_IN;
        xhci_ring_enqueue(ring, &trb);
    }

    memset(&trb, 0, sizeof(trb));
    trb.control = (XHCI_TRB_STATUS << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
    if (len > 0 && !(setup->bmRequestType & 0x80))
        trb.control |= XHCI_TRB_DIR_IN;
    else if (!len)
        trb.control |= XHCI_TRB_DIR_IN;
    xhci_ring_enqueue(ring, &trb);

    priv->cmd_done  = 0;
    priv->cmd_error = 0;
    priv->transfer_done = 0;
    xhci_db_write32(priv, slot, 1);

    return xhci_wait_cmd(priv, 500);
}

static int xhci_interrupt_transfer(usb_hc_t *hc, usb_device_t *dev,
                                    uint8_t ep_num, void *buf, uint16_t len) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
    uint8_t slot = dev->address;
    uint8_t dci = (ep_num * 2) + 1;

    xhci_ring_t *ring = &priv->ep_rings[slot][dci - 1];
    if (!ring->ring) return -1;

    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.param_lo = xhci_va_to_pa(buf);
    trb.param_hi = 0;
    trb.status   = len;
    trb.control  = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
    xhci_ring_enqueue(ring, &trb);

    priv->cmd_done  = 0;
    priv->cmd_error = 0;
    priv->transfer_done = 0;
    xhci_db_write32(priv, slot, dci);

    return xhci_wait_cmd(priv, 500);
}

static int xhci_bulk_transfer(usb_hc_t *hc, usb_device_t *dev,
                               uint8_t ep_num, uint8_t dir,
                               void *buf, uint16_t len) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
    uint8_t slot = dev->address;
    uint8_t dci = (ep_num * 2) + (dir == USB_DIR_IN ? 1 : 0);

    xhci_ring_t *ring = &priv->ep_rings[slot][dci - 1];
    if (!ring->ring) return -1;

    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.param_lo = xhci_va_to_pa(buf);
    trb.param_hi = 0;
    trb.status   = len;
    trb.control  = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
    xhci_ring_enqueue(ring, &trb);

    priv->cmd_done  = 0;
    priv->cmd_error = 0;
    priv->transfer_done = 0;
    xhci_db_write32(priv, slot, dci);

    return xhci_wait_cmd(priv, 1000) == 0 ? len : -1;
}

static int xhci_port_reset(usb_hc_t *hc, uint8_t port) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;

    uint32_t sc = xhci_portsc_read(priv, port);
    if (!(sc & XHCI_PORTSC_CCS)) return -1;

    xhci_portsc_set(priv, port, XHCI_PORTSC_PR);

    for (int i = 0; i < 500; i++) {
        xhci_udelay(1000);
        sc = xhci_portsc_read(priv, port);
        if (sc & XHCI_PORTSC_PRC) break;
    }

    if (sc & XHCI_PORTSC_PRC)
        xhci_portsc_clear_change(priv, port, XHCI_PORTSC_PRC);
    if (sc & XHCI_PORTSC_CSC)
        xhci_portsc_clear_change(priv, port, XHCI_PORTSC_CSC);
    xhci_udelay(5000);

    sc = xhci_portsc_read(priv, port);
    return (sc & XHCI_PORTSC_PED) ? 0 : -1;
}

static int xhci_port_get_status(usb_hc_t *hc, uint8_t port) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
    return (int)xhci_portsc_read(priv, port);
}

static void xhci_process_event(xhci_priv_t *priv, xhci_trb_t *evt) {
    uint32_t type = (evt->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
    uint8_t  cc   = (uint8_t)((evt->status >> 24) & 0xFF);

    switch (type) {
    case XHCI_TRB_CMD_COMPLETE:
        priv->cmd_result = evt->control;
        priv->cmd_error  = (cc != XHCI_CC_SUCCESS) ? 1 : 0;
        priv->cmd_done   = 1;
        break;

    case XHCI_TRB_TRANSFER_EVT: {
        uint8_t slot = (uint8_t)((evt->control >> 24) & 0xFF);
        uint8_t dci  = (uint8_t)((evt->control >> 16) & 0x1F);

        if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
            for (int i = 0; i < priv->intr_ep_count; i++) {
                xhci_intr_ep_slot_t *s = &priv->intr_slots[i];
                if (s->active && s->slot_id == slot && s->dci == dci) {
                    if (s->notify)
                        s->notify(s->dev, s->buf, s->len, s->notify_priv);

                    xhci_trb_t re_trb;
                    memset(&re_trb, 0, sizeof(re_trb));
                    re_trb.param_lo = xhci_va_to_pa(s->buf);
                    re_trb.status   = s->len;
                    re_trb.control  = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
                    xhci_ring_enqueue(&s->ring, &re_trb);
                    xhci_db_write32(priv, slot, dci);
                    break;
                }
            }
            priv->cmd_error = 0;
        } else {
            priv->cmd_error = 1;
        }
        priv->transfer_done = 1;
        break;
    }

    case XHCI_TRB_PORT_STATUS: {
        uint8_t port = (uint8_t)((evt->param_lo >> 24) & 0xFF) - 1;
        uint32_t sc = xhci_portsc_read(priv, port);
        uint32_t changes = sc & XHCI_PORTSC_RW1C_BITS;
        if (changes)
            xhci_portsc_clear_change(priv, port, changes);
        break;
    }

    default:
        break;
    }
}

static volatile int xhci_irq_masked = 0;

void xhci_unmask_irq11(void) {
    if (xhci_irq_masked) {
        uint8_t mask = port_byte_in(0xA1);
        port_byte_out(0xA1, mask & ~(1u << 3));
        xhci_irq_masked = 0;
    }
}

static void xhci_handle_irq(usb_hc_t *hc) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;

    uint32_t sts = xhci_op_read32(priv, XHCI_OP_USBSTS);

    if (!(sts & XHCI_STS_EINT)) {
        return;
    }

    xhci_op_write32(priv, XHCI_OP_USBSTS, XHCI_STS_EINT);
    irq_spinlock_acquire(&xhci_evt_lock);
    xhci_drain_events(priv);
    irq_spinlock_release(&xhci_evt_lock);

    xhci_rt_write32(priv, 0x20 + XHCI_IMAN, 0x3);

    if (sts & XHCI_STS_HSE)
        kprint("[XHCI] Host System Error!\n");
}

void xhci_irq_handler(void) {
    for (usb_hc_t *hc = xhci_hc_list; hc; hc = hc->irq_next)
        xhci_handle_irq(hc);
}

static void xhci_device_removed(usb_hc_t *hc, usb_device_t *dev) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
    for (int i = 0; i < priv->intr_ep_count; i++) {
        xhci_intr_ep_slot_t *s = &priv->intr_slots[i];
        if (s->active && s->dev == dev) {
            s->active = 0;
            s->dev    = NULL;
            s->buf    = NULL;
        }
    }
}

int xhci_register_interrupt_ep(usb_hc_t *hc, usb_device_t *dev,
                                uint8_t ep_num, void *buf, uint16_t len,
                                usb_irq_notify_fn_t notify, void *notify_priv) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;

    if (priv->intr_ep_count >= XHCI_MAX_INTR_EP) {
        klog(LOG_WARN, "xHCI interrupt endpoint slots full");
        return -1;
    }

    uint8_t slot = dev->address;
    uint8_t dci  = (ep_num * 2) + 1;

    usb_endpoint_t *ep = NULL;
    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->ep[i].address == ep_num && dev->ep[i].direction == USB_DIR_IN) {
            ep = &dev->ep[i]; break;
        }
    }
    uint16_t mps = ep ? ep->max_packet : 8;
    if (len > mps) len = mps;
    uint8_t interval = ep ? ep->interval : 8;

    if (xhci_configure_endpoint(priv, slot, dci, XHCI_EP_CTX_TYPE_INTR_IN, mps, interval) < 0)
        return -1;

    xhci_intr_ep_slot_t *s = &priv->intr_slots[priv->intr_ep_count++];
    s->slot_id     = slot;
    s->dci         = dci;
    s->dev         = dev;
    s->ep_num      = ep_num;
    s->buf         = buf;
    s->len         = len;
    s->notify      = notify;
    s->notify_priv = notify_priv;
    s->active      = 1;
    s->ring        = priv->ep_rings[slot][dci - 1];

    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.param_lo = xhci_va_to_pa(buf);
    trb.status   = len;
    trb.control  = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
    xhci_ring_enqueue(&s->ring, &trb);
    xhci_db_write32(priv, slot, dci);

    return 0;
}

static uint8_t xhci_port_speed_to_usb(uint32_t portsc) {
    uint8_t ps = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
    switch (ps) {
        case XHCI_PORT_SPEED_LS: return USB_SPEED_LOW;
        case XHCI_PORT_SPEED_FS: return USB_SPEED_FULL;
        case XHCI_PORT_SPEED_HS: return USB_SPEED_HIGH;
        default:                 return USB_SPEED_HIGH;
    }
}

static int xhci_init_one(uint32_t phys_base) {
    extern uint32_t page_directory[1024];
    uint32_t map_size = 0x10000;
    /* Map xHCI MMIO as uncacheable (PCD|PWT). */
    for (uint32_t off = 0; off < map_size; off += 0x1000)
        vmm_map(page_directory, phys_base + off, phys_base + off,
                PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);

    xhci_priv_t *priv = (xhci_priv_t *)kmalloc(sizeof(xhci_priv_t));
    if (!priv) { klog(LOG_WARN, "xHCI state allocation failed"); return -1; }
    memset(priv, 0, sizeof(xhci_priv_t));
    spinlock_init(&priv->ctx_lock);

    priv->cap_phys = phys_base;
    priv->cap = (volatile uint32_t *)phys_base;

    uint8_t  cap_len    = (uint8_t)(xhci_cap_read32(priv, XHCI_CAP_CAPLENGTH) & 0xFF);
    uint32_t hcsparams1 = xhci_cap_read32(priv, XHCI_CAP_HCSPARAMS1);
    uint32_t hccparams1 = xhci_cap_read32(priv, XHCI_CAP_HCCPARAMS1);

    priv->max_slots = (uint8_t)(hcsparams1 & 0xFF);
    priv->max_intrs = (uint16_t)((hcsparams1 >> 8) & 0x7FF);
    priv->max_ports = (uint8_t)((hcsparams1 >> 24) & 0xFF);
    priv->context_size = (hccparams1 & (1u << 2)) ? 64 : 32;

    if (priv->max_slots > XHCI_MAX_SLOTS) priv->max_slots = XHCI_MAX_SLOTS;
    if (priv->max_ports > XHCI_MAX_PORTS) priv->max_ports = XHCI_MAX_PORTS;

    priv->op_off = cap_len;
    priv->op     = (volatile uint32_t *)(phys_base + cap_len);
    priv->rt_off = xhci_cap_read32(priv, XHCI_CAP_RTSOFF) & ~0x1F;
    priv->rt     = (volatile uint32_t *)(phys_base + priv->rt_off);
    priv->db_off = xhci_cap_read32(priv, XHCI_CAP_DBOFF) & ~0x3;
    priv->db     = (volatile uint32_t *)(phys_base + priv->db_off);

    uint32_t xecp_off = ((hccparams1 >> 16) & 0xFFFF) << 2;
    if (xecp_off) {
        uint32_t cur = xecp_off;
        for (int i = 0; i < 32 && cur; i++) {
            volatile uint32_t *ecap = (volatile uint32_t *)(phys_base + cur);
            uint8_t ecap_id = (uint8_t)(ecap[0] & 0xFF);
            if (ecap_id == 1) {
                ecap[0] |= (1u << 24);
                for (int w = 0; w < 100; w++) {
                    if ((ecap[0] & (1u << 24)) && !(ecap[0] & (1u << 16))) break;
                    xhci_udelay(10000);
                }
                if (ecap[0] & (1u << 16))
                    klog(LOG_WARN, "xHCI BIOS ownership handoff timed out");
                ecap[1] = 0;
                break;
            }
            uint8_t next = (uint8_t)((ecap[0] >> 8) & 0xFF);
            if (!next) break;
            cur += (uint32_t)next << 2;
        }
    }

    xhci_op_write32(priv, XHCI_OP_USBCMD,
                     xhci_op_read32(priv, XHCI_OP_USBCMD) & ~XHCI_CMD_RS);
    for (int i = 0; i < 100; i++) {
        if (xhci_op_read32(priv, XHCI_OP_USBSTS) & XHCI_STS_HCH) break;
        xhci_udelay(1000);
    }

    xhci_op_write32(priv, XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    for (int i = 0; i < 100; i++) {
        if (!(xhci_op_read32(priv, XHCI_OP_USBCMD) & XHCI_CMD_HCRST)) break;
        xhci_udelay(1000);
    }
    for (int i = 0; i < 100; i++) {
        if (!(xhci_op_read32(priv, XHCI_OP_USBSTS) & XHCI_STS_CNR)) break;
        xhci_udelay(1000);
    }
    xhci_op_write32(priv, XHCI_OP_USBSTS, xhci_op_read32(priv, XHCI_OP_USBSTS));
    xhci_op_write32(priv, XHCI_OP_DNCTRL, 0x2);
    xhci_op_write32(priv, XHCI_OP_CONFIG, priv->max_slots);

    priv->dcbaa = (uint64_t *)kmalloc_aligned((priv->max_slots + 1) * sizeof(uint64_t), 64);
    if (!priv->dcbaa) { kfree_heap(priv); return -1; }
    memset(priv->dcbaa, 0, (priv->max_slots + 1) * sizeof(uint64_t));
    xhci_op_write32(priv, XHCI_OP_DCBAAP, xhci_va_to_pa(priv->dcbaa));
    xhci_op_write32(priv, XHCI_OP_DCBAAP + 4, 0);

    priv->dev_ctx_pool = (uint8_t *)kmalloc_aligned((priv->max_slots + 1) * 2048, 64);
    if (!priv->dev_ctx_pool) { kfree_heap(priv); return -1; }
    memset(priv->dev_ctx_pool, 0, (priv->max_slots + 1) * 2048);

    priv->input_ctx_pool = (uint8_t *)kmalloc_aligned(2048, 64);
    if (!priv->input_ctx_pool) { kfree_heap(priv); return -1; }

    xhci_trb_t *cmd_mem = (xhci_trb_t *)kmalloc_aligned(XHCI_CMD_RING_SIZE * sizeof(xhci_trb_t), 64);
    if (!cmd_mem) { kfree_heap(priv); return -1; }
    xhci_ring_init(&priv->cmd_ring, cmd_mem, XHCI_CMD_RING_SIZE);

    xhci_op_write32(priv, XHCI_OP_CRCR, xhci_va_to_pa(cmd_mem) | 1);
    xhci_op_write32(priv, XHCI_OP_CRCR + 4, 0);

    priv->evt_ring = (xhci_trb_t *)kmalloc_aligned(XHCI_EVT_RING_SIZE * sizeof(xhci_trb_t), 64);
    if (!priv->evt_ring) { kfree_heap(priv); return -1; }
    memset(priv->evt_ring, 0, XHCI_EVT_RING_SIZE * sizeof(xhci_trb_t));
    priv->evt_dequeue = 0;
    priv->evt_cycle   = 1;

    priv->erst = (xhci_erst_entry_t *)kmalloc_aligned(sizeof(xhci_erst_entry_t) * XHCI_ERST_SIZE, 64);
    if (!priv->erst) { kfree_heap(priv); return -1; }
    memset(priv->erst, 0, sizeof(xhci_erst_entry_t) * XHCI_ERST_SIZE);
    priv->erst[0].seg_addr_lo = xhci_va_to_pa(priv->evt_ring);
    priv->erst[0].seg_addr_hi = 0;
    priv->erst[0].seg_size    = XHCI_EVT_RING_SIZE;

    xhci_rt_write32(priv, 0x20 + XHCI_ERSTSZ, XHCI_ERST_SIZE);
    xhci_rt_write32(priv, 0x20 + XHCI_ERDP, xhci_va_to_pa(priv->evt_ring) | (1u << 3));
    xhci_rt_write32(priv, 0x20 + XHCI_ERDP + 4, 0);
    xhci_rt_write32(priv, 0x20 + XHCI_ERSTBA, xhci_va_to_pa(priv->erst));
    xhci_rt_write32(priv, 0x20 + XHCI_ERSTBA + 4, 0);

    xhci_rt_write32(priv, 0x20 + XHCI_IMOD, 0x000003F8);
    xhci_rt_write32(priv, 0x20 + XHCI_IMAN, 0x3);

    xhci_op_write32(priv, XHCI_OP_USBCMD,
                     XHCI_CMD_RS | XHCI_CMD_INTE | XHCI_CMD_HSEE);

    for (int i = 0; i < 100; i++) {
        if (!(xhci_op_read32(priv, XHCI_OP_USBSTS) & XHCI_STS_HCH)) break;
        xhci_udelay(1000);
    }

    if (xhci_op_read32(priv, XHCI_OP_USBSTS) & XHCI_STS_HCH) {
        klog(LOG_WARN, "xHCI host controller did not start");
        kfree_heap(priv);
        return -1;
    }

    usb_hc_t *hc = (usb_hc_t *)kmalloc(sizeof(usb_hc_t));
    if (!hc) { kfree_heap(priv); return -1; }
    memset(hc, 0, sizeof(usb_hc_t));

    hc->name               = "XHCI";
    hc->control_transfer   = xhci_control_transfer;
    hc->interrupt_transfer = xhci_interrupt_transfer;
    hc->bulk_transfer      = xhci_bulk_transfer;
    hc->port_reset         = xhci_port_reset;
    hc->port_get_status    = xhci_port_get_status;
    hc->device_removed     = xhci_device_removed;
    hc->num_ports          = priv->max_ports;
    hc->priv               = priv;

    hc->irq_next  = xhci_hc_list;
    xhci_hc_list  = hc;

    usb_hc_register(hc);

    for (uint8_t p = 0; p < priv->max_ports; p++) {
        uint32_t sc = xhci_portsc_read(priv, p);
        if (sc & XHCI_PORTSC_CCS) {
            if (!(sc & XHCI_PORTSC_PED)) {
                xhci_portsc_set(priv, p, XHCI_PORTSC_PR);
                for (int i = 0; i < 500; i++) {
                    xhci_udelay(1000);
                    sc = xhci_portsc_read(priv, p);
                    if (sc & XHCI_PORTSC_PRC) break;
                }
                if (sc & XHCI_PORTSC_PRC)
                    xhci_portsc_clear_change(priv, p, XHCI_PORTSC_PRC);
                if (sc & XHCI_PORTSC_CSC)
                    xhci_portsc_clear_change(priv, p, XHCI_PORTSC_CSC);
                xhci_udelay(10000);
                sc = xhci_portsc_read(priv, p);
            }

            if (sc & XHCI_PORTSC_PED) {
                uint8_t speed = xhci_port_speed_to_usb(sc);
                uint8_t slot_id;
                if (xhci_enable_slot(priv, &slot_id) == 0) {
                    priv->slot_used[slot_id] = 1;
                    priv->slot_port[slot_id] = p;

                    if (xhci_address_device(priv, slot_id, p, speed, 0) == 0) {
                        usb_device_t *dev = (usb_device_t *)kmalloc(sizeof(usb_device_t));
                        if (dev) {
                            memset(dev, 0, sizeof(usb_device_t));
                            dev->hc      = hc;
                            dev->port    = p;
                            dev->speed   = speed;
                            dev->address = slot_id;

                            if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0,
                                                    &dev->dev_desc,
                                                    sizeof(usb_dev_desc_t)) >= 0) {
                                usb_cfg_desc_t cfg_hdr;
                                if (usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0,
                                                        &cfg_hdr, sizeof(usb_cfg_desc_t)) >= 0) {
                                    uint16_t total = cfg_hdr.wTotalLength;
                                    if (total > sizeof(dev->config_buf))
                                        total = sizeof(dev->config_buf);
                                    usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0,
                                                        dev->config_buf, total);
                                    dev->config_len = total;

                                    uint8_t *cp   = dev->config_buf;
                                    uint8_t *cend = cp + dev->config_len;
                                    dev->ep_count = 0;
                                    while (cp + 2 <= cend) {
                                        uint8_t dlen = cp[0], dtype = cp[1];
                                        if (dlen < 2 || cp + dlen > cend) break;
                                        if (dtype == USB_DESC_INTERFACE && dlen >= 9) {
                                            usb_iface_desc_t *ifd = (usb_iface_desc_t *)cp;
                                            if (dev->class_code == 0) {
                                                dev->class_code = ifd->bInterfaceClass;
                                                dev->subclass   = ifd->bInterfaceSubClass;
                                                dev->protocol   = ifd->bInterfaceProtocol;
                                            }
                                        } else if (dtype == USB_DESC_ENDPOINT && dlen >= 7) {
                                            usb_ep_desc_t *epd = (usb_ep_desc_t *)cp;
                                            if (dev->ep_count < USB_MAX_ENDPOINTS) {
                                                usb_endpoint_t *epp = &dev->ep[dev->ep_count++];
                                                epp->address       = epd->bEndpointAddress & 0x0F;
                                                epp->direction     = (epd->bEndpointAddress & 0x80) ? USB_DIR_IN : USB_DIR_OUT;
                                                epp->transfer_type = epd->bmAttributes & 0x03;
                                                epp->max_packet    = epd->wMaxPacketSize & 0x7FF;
                                                epp->interval      = epd->bInterval;
                                                epp->toggle        = 0;
                                            }
                                        }
                                        cp += dlen;
                                    }

                                    usb_set_configuration(dev, cfg_hdr.bConfigurationValue);
                                }

                                usb_register_device(dev);
                            }
                        }
                    }
                }
            }
        }
    }

    return 0;
}

static int xhci_pci_probe(pci_device_t *pdev) {
    if (pdev->prog_if != 0x30) return -1;

    uint32_t mmio = 0;
    for (int i = 0; i < 6; i++) {
        if (!pdev->bars[i].is_io && pdev->bars[i].base) {
            mmio = pdev->bars[i].base; break;
        }
    }
    if (!mmio)
        mmio = pci_read32(pdev->bus, pdev->dev, pdev->fn, 0x10) & ~0xFu;
    if (!mmio) { klog(LOG_WARN, "xHCI MMIO BAR missing"); return -1; }

    uint32_t cmd = pci_read32(pdev->bus, pdev->dev, pdev->fn, 0x04);
    pci_write32(pdev->bus, pdev->dev, pdev->fn, 0x04, cmd | 0x06);

    uint8_t cap_ptr = (uint8_t)(pci_read32(pdev->bus, pdev->dev, pdev->fn, 0x34) & 0xFF);
    while (cap_ptr && cap_ptr != 0xFF) {
        uint32_t cap    = pci_read32(pdev->bus, pdev->dev, pdev->fn, cap_ptr);
        uint8_t  cap_id = (uint8_t)(cap & 0xFF);
        if (cap_id == 0x05) {
            uint32_t cap0 = pci_read32(pdev->bus, pdev->dev, pdev->fn, cap_ptr);
            cap0 &= ~(1u << 16); 
            pci_write32(pdev->bus, pdev->dev, pdev->fn, cap_ptr, cap0);
            break;
        }
        cap_ptr = (uint8_t)((cap >> 8) & 0xFF);
    }

    cmd = pci_read32(pdev->bus, pdev->dev, pdev->fn, 0x04);
    pci_write32(pdev->bus, pdev->dev, pdev->fn, 0x04, cmd & ~(1u << 10));

    return xhci_init_one(mmio);
}

static pci_driver_t xhci_pci_driver = {
    .name       = "xhci_hcd",
    .vendor_id  = PCI_ANY_ID,
    .device_id  = PCI_ANY_ID,
    .class_code = 0x0C,
    .subclass   = 0x03,
    .probe      = xhci_pci_probe,
};

void xhci_pci_init(void) {
    irq_spinlock_init(&xhci_evt_lock);
    pci_register_driver(&xhci_pci_driver);

    int found = 0;
    pci_device_t *d = pci_find_by_class(0x0C, 0x03);
    while (d) {
        if (d->prog_if == 0x30) {
            found++;
            xhci_pci_probe(d);
        }
        d = d->next;
        while (d && !(d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30))
            d = d->next;
    }
    if (found == 0) {
        klog(LOG_WARN, "xHCI: no USB3 controller found on PCI bus");
    } else {
        klog(LOG_OK, "xHCI host controller(s) brought up");
    }
}