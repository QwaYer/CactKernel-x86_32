/* xHCI — USB transfer paths: control, interrupt, and bulk. */

#include "xhci.h"
#include "xhci_internal.h"
#include "usb.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "sync.h"

/* Recover an endpoint whose transfer timed out.
 *
 * A timed-out transfer leaves a TD the controller will never complete (the
 * device stopped answering).  That pending TD blocks the whole endpoint: every
 * later transfer to it times out too, so a single stalled control transfer
 * used to wedge EP0 for good.  Stop the endpoint, then move its dequeue past
 * the abandoned TRBs so it runs again from the next free slot. */
static void xhci_recover_ep(xhci_priv_t *priv, uint8_t slot, uint8_t dci) {
    xhci_ring_t *ring = &priv->ep_rings[slot][dci - 1];
    if (!ring->ring)
        return;

    xhci_trb_t trb;

    /* Stop Endpoint: abort the TD that never completed. */
    memset(&trb, 0, sizeof(trb));
    trb.control = (XHCI_TRB_STOP_EP << XHCI_TRB_TYPE_SHIFT)
                | ((uint32_t)slot << 24)
                | ((uint32_t)dci << 16);
    int rc_stop = xhci_send_cmd(priv, &trb);

    /* Set TR Dequeue Pointer: resume at the next free TRB (the abandoned
     * SETUP/DATA/STATUS are skipped).  The dequeue cycle state is bit 0. */
    memset(&trb, 0, sizeof(trb));
    trb.param_lo = xhci_va_to_pa(&ring->ring[ring->enqueue])
                 | (ring->cycle ? 1u : 0u);
    trb.param_hi = 0;
    trb.control  = (XHCI_TRB_SET_TR_DEQ << XHCI_TRB_TYPE_SHIFT)
                 | ((uint32_t)slot << 24)
                 | ((uint32_t)dci << 16);
    int rc_deq = xhci_send_cmd(priv, &trb);

    /* The Stop raises a "stopped" Transfer Event for the discarded TD.  Drain
     * it and drop whatever completion state this endpoint still had, so the
     * next transfer does not consume it and return early. */
    xhci_poll_events(priv);
    priv->cmd_done  = 0;
    priv->cmd_error = 0;
    if (dci >= 1 && dci <= 31) {
        xhci_ring_t *rr = &priv->ep_rings[slot][dci - 1];
        rr->done       = 0;
        rr->err        = 0;
        rr->xfer_armed = 0;
    }

    pr_warn("xHCI: EP slot %u dci %u recovery stop=%d setdeq=%d",
            (unsigned)slot, (unsigned)dci, rc_stop, rc_deq);
}

int xhci_control_transfer(usb_hc_t *hc, usb_device_t *dev,
                          usb_setup_pkt_t *setup,
                          void *data, uint16_t len) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
    uint8_t slot = dev->address;
    if (!slot || slot > XHCI_MAX_SLOTS) {
        pr_err("  %-11s : control transfer: bad slot %u (setup failure)\n",
               "xhci-xfer", (unsigned)slot);
        return -1;
    }

    xhci_ring_t *ring = &priv->ep_rings[slot][0];
    if (!ring->ring) {
        pr_err("  %-11s : control transfer: EP0 ring not set up (slot %u)\n",
               "xhci-xfer", (unsigned)slot);
        return -1;
    }

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
    xhci_db_write32(priv, slot, 1);

    int rc = xhci_wait_transfer(priv, ring, 500);
    if (rc != 0)
        xhci_recover_ep(priv, slot, 1);   /* EP0 is DCI 1 */
    return rc == 0 ? 0 : -1;
}

int xhci_interrupt_transfer(usb_hc_t *hc, usb_device_t *dev,
                            uint8_t ep_num, void *buf, uint16_t len) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
    uint8_t slot = dev->address;
    uint8_t dci = (ep_num * 2) + 1;

    xhci_ring_t *ring = &priv->ep_rings[slot][dci - 1];
    if (!ring->ring) {
        pr_err("  %-11s : interrupt transfer: ring not set up (slot %u dci %u)\n",
               "xhci-xfer", (unsigned)slot, (unsigned)dci);
        return -1;
    }

    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.param_lo = xhci_va_to_pa(buf);
    trb.param_hi = 0;
    trb.status   = len;
    trb.control  = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
    xhci_ring_enqueue(ring, &trb);

    priv->cmd_done  = 0;
    priv->cmd_error = 0;
    xhci_db_write32(priv, slot, dci);

    return xhci_wait_transfer(priv, ring, 500) == 0 ? 0 : -1;
}

int xhci_bulk_transfer(usb_hc_t *hc, usb_device_t *dev,
                       uint8_t ep_num, uint8_t dir,
                       void *buf, uint16_t len, uint32_t timeout_ms) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
    uint8_t slot = dev->address;
    uint8_t dci = (ep_num * 2) + (dir == USB_DIR_IN ? 1 : 0);

    /* A module built against the older 6-argument op would hand us garbage in
     * the new slot; keep the wait inside a sane range instead of stalling the
     * bus on a nonsensical value. */
    if (timeout_ms == 0 || timeout_ms > 10000)
        timeout_ms = 1000;

    xhci_ring_t *ring = &priv->ep_rings[slot][dci - 1];
    if (!ring->ring) {
        pr_err("  %-11s : bulk transfer: ring not set up (slot %u dci %u)\n",
               "xhci-xfer", (unsigned)slot, (unsigned)dci);
        return -1;
    }

    /* One transfer in flight per endpoint.  A timed-out bulk-IN leaves its TRB
     * armed so the next frame still lands in the caller's buffer; enqueuing a
     * second transfer for the same endpoint would then let the chip rewrite
     * that buffer at any moment, and the caller reads a frame that changes
     * under it (observed as an RXINFO that no longer matched the bytes right
     * after it).  A retry on the same buffer waits on the armed transfer; a
     * retry on a different buffer has to abandon it first. */
    if (ring->xfer_armed &&
        (ring->xfer_buf != buf || ring->xfer_len != len)) {
        ring->xfer_armed = 0;
        ring->done = 0;
        xhci_recover_ep(priv, slot, dci);
    }

    if (!ring->xfer_armed) {
        xhci_trb_t trb;
        memset(&trb, 0, sizeof(trb));
        trb.param_lo = xhci_va_to_pa(buf);
        trb.param_hi = 0;
        trb.status   = len;
        trb.control  = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
        xhci_ring_enqueue(ring, &trb);

        priv->cmd_done  = 0;
        priv->cmd_error = 0;
        ring->done       = 0;
        ring->xfer_buf   = buf;
        ring->xfer_len   = len;
        ring->xfer_armed = 1;
        xhci_db_write32(priv, slot, dci);
    }

    int rc = xhci_wait_transfer(priv, ring, timeout_ms);
    if (rc >= 0) {
        /* Completed (0), or completed with an error (1): the TRB is consumed
         * either way, so the endpoint is free again. */
        ring->xfer_armed = 0;
        return rc == 0 ? len : -1;
    }

    /* Timed out.  For an IN the TRB stays armed — "no data yet", the chip NAKs
     * a quiet RX endpoint.  A bulk-OUT timeout means the firmware stopped
     * draining that endpoint: recover it, or the abandoned TD wedges every
     * later transmit. */
    if (dir == USB_DIR_OUT) {
        ring->xfer_armed = 0;
        xhci_recover_ep(priv, slot, dci);
    }
    return -1;
}
