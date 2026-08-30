/* xHCI — command/event ring core: TRB enqueue, event draining, command
 * submission/wait, and the shared IRQ path. */

#include "xhci.h"
#include "xhci_internal.h"
#include "usb.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "sync.h"

void xhci_ring_init(xhci_ring_t *ring, xhci_trb_t *mem, uint32_t size) {
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

void xhci_ring_enqueue(xhci_ring_t *ring, xhci_trb_t *trb) {
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

int xhci_wait_cmd(xhci_priv_t *priv, uint32_t timeout_ms) {
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
    pr_warn("xHCI command timeout");
    priv->cmd_done = 0;
    priv->transfer_done = 0;
    return -1;
}

int xhci_send_cmd(xhci_priv_t *priv, xhci_trb_t *trb) {
    priv->cmd_done   = 0;
    priv->cmd_error  = 0;
    priv->cmd_result = 0;
    xhci_ring_enqueue(&priv->cmd_ring, trb);
    xhci_db_write32(priv, 0, 0);
    return xhci_wait_cmd(priv, 500);
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

void xhci_handle_irq(usb_hc_t *hc) {
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
        printk("[XHCI] Host System Error!\n");
}
