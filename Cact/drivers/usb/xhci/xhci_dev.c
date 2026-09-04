/* xHCI — device/context management: slot enabling, device addressing,
 * endpoint configuration, port operations, and interrupt endpoint slots. */

#include "xhci.h"
#include "xhci_internal.h"
#include "usb.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "sync.h"

int xhci_enable_slot(xhci_priv_t *priv, uint8_t *slot_id) {
    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.control = (XHCI_TRB_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT);
    if (xhci_send_cmd(priv, &trb) < 0) {
        pr_warn("xHCI enable_slot failed");
        return -1;
    }
    *slot_id = (uint8_t)((priv->cmd_result >> 24) & 0xFF);
    if (*slot_id == 0 || *slot_id > priv->max_slots || *slot_id > XHCI_MAX_SLOTS) {
        printk("xHCI: bad slot id %d", (int)*slot_id);
        return -1;
    }
    return 0;
}

uint8_t *xhci_get_dev_ctx(xhci_priv_t *priv, uint8_t slot) {
    if (slot > priv->max_slots) return NULL;
    return priv->dev_ctx_pool + (uint32_t)slot * 2048;
}

uint8_t *xhci_get_input_ctx(xhci_priv_t *priv) {
    return priv->input_ctx_pool;
}

void xhci_setup_ep_ring(xhci_priv_t *priv, uint8_t slot, uint8_t dci) {
    xhci_ring_t *ring = &priv->ep_rings[slot][dci - 1];
    xhci_trb_t *mem = (xhci_trb_t *)kmalloc_aligned(XHCI_EP_RING_SIZE * sizeof(xhci_trb_t), 64);
    if (!mem) {
        pr_warn("xHCI endpoint ring allocation failed");
        return;
    }
    if (ring->ring)
        kfree(ring->ring);
    xhci_ring_init(ring, mem, XHCI_EP_RING_SIZE);
}

int xhci_address_device(xhci_priv_t *priv, uint8_t slot, uint8_t port,
                        uint8_t speed, int bsr) {
    if (slot == 0 || slot > priv->max_slots || slot > XHCI_MAX_SLOTS)
        return -1;

    spin_lock(&priv->ctx_lock);
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
    spin_unlock(&priv->ctx_lock);
    return ret;
}

int xhci_configure_endpoint(xhci_priv_t *priv, uint8_t slot,
                            uint8_t dci, uint8_t ep_type,
                            uint16_t mps, uint8_t interval) {
    if (!priv) return -1;
    if (slot == 0 || slot > priv->max_slots || slot > XHCI_MAX_SLOTS)
        return -1;
    /* DCI 0 is EP0 (control, configured by Address Device); 31 is the last
     * valid endpoint context index (EP7-IN).  Guarding here keeps the
     * add_flags bit shift and the input-context offset in bounds. */
    if (dci == 0 || dci > 31)
        return -1;

    spin_lock(&priv->ctx_lock);
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
    spin_unlock(&priv->ctx_lock);
    return ret;
}

int xhci_port_reset(usb_hc_t *hc, uint8_t port) {
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

int xhci_port_get_status(usb_hc_t *hc, uint8_t port) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
    return (int)xhci_portsc_read(priv, port);
}

void xhci_device_removed(usb_hc_t *hc, usb_device_t *dev) {
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
        pr_warn("xHCI interrupt endpoint slots full");
        return -1;
    }

    /* Endpoint number 1..15 keeps dci = 2*ep_num+1 within 3..31. */
    if (ep_num == 0 || ep_num > 15) {
        pr_warn("xHCI: invalid interrupt endpoint number %d", (int)ep_num);
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

uint8_t xhci_port_speed_to_usb(uint32_t portsc) {
    uint8_t ps = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
    switch (ps) {
        case XHCI_PORT_SPEED_LS: return USB_SPEED_LOW;
        case XHCI_PORT_SPEED_FS: return USB_SPEED_FULL;
        case XHCI_PORT_SPEED_HS: return USB_SPEED_HIGH;
        default:                 return USB_SPEED_HIGH;
    }
}
