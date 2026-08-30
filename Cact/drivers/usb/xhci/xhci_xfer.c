/* xHCI — USB transfer paths: control, interrupt, and bulk. */

#include "xhci.h"
#include "xhci_internal.h"
#include "usb.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "sync.h"

int xhci_control_transfer(usb_hc_t *hc, usb_device_t *dev,
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

int xhci_interrupt_transfer(usb_hc_t *hc, usb_device_t *dev,
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

int xhci_bulk_transfer(usb_hc_t *hc, usb_device_t *dev,
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
