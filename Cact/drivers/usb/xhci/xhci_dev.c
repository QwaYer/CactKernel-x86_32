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
        pr_err("xHCI: bad slot id %d", (int)*slot_id);
        return -1;
    }
    return 0;
}

int xhci_disable_slot(xhci_priv_t *priv, uint8_t slot) {
    if (slot == 0 || slot > priv->max_slots || slot > XHCI_MAX_SLOTS)
        return -1;
    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.control = (XHCI_TRB_DISABLE_SLOT << XHCI_TRB_TYPE_SHIFT)
                | ((uint32_t)slot << 24);
    return xhci_send_cmd(priv, &trb);
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
    /* Re-configuring an endpoint must not throw its ring away: the previous
     * behaviour freed and re-allocated on every Configure Endpoint, which
     * abandoned any TRB in flight and left every copy of the ring (notably the
     * one an interrupt slot caches) dangling. */
    if (ring->ring)
        return;
    xhci_trb_t *mem = (xhci_trb_t *)kmalloc_aligned(XHCI_EP_RING_SIZE * sizeof(xhci_trb_t), 64);
    if (!mem) {
        pr_warn("xHCI endpoint ring allocation failed");
        return;
    }
    xhci_ring_init(ring, mem, XHCI_EP_RING_SIZE);
}

/* Release every ring of a slot.  Only safe once the slot has been disabled
 * (the controller no longer DMAs them). */
void xhci_free_ep_rings(xhci_priv_t *priv, uint8_t slot) {
    if (slot > priv->max_slots)
        return;
    for (int dci = 1; dci <= 31; dci++) {
        xhci_ring_t *ring = &priv->ep_rings[slot][dci - 1];
        if (ring->ring)
            kfree(ring->ring);
        memset(ring, 0, sizeof(*ring));
    }
}

int xhci_address_device(xhci_priv_t *priv, uint8_t slot, uint8_t port,
                        uint8_t speed, int bsr,
                        uint8_t parent_slot, uint8_t parent_port) {
    if (slot == 0 || slot > priv->max_slots || slot > XHCI_MAX_SLOTS)
        return -1;

    spin_lock(&priv->ctx_lock);
    uint8_t *input = xhci_get_input_ctx(priv);
    memset(input, 0, 2048);

    xhci_input_ctrl_ctx_t *icc = (xhci_input_ctrl_ctx_t *)input;
    icc->add_flags = (1u << 0) | (1u << 1);

    uint32_t ctx_off = priv->context_size;
    xhci_slot_ctx_t *slot_ctx = (xhci_slot_ctx_t *)(input + ctx_off);

    uint8_t  xhci_speed;
    uint16_t mps;
    switch (speed) {
        case USB_SPEED_LOW:  xhci_speed = 2; mps = 8;   break;
        case USB_SPEED_FULL: xhci_speed = 1; mps = 8;   break;
        case USB_SPEED_HIGH: xhci_speed = 3; mps = 64;  break;
        case USB_SPEED_SUPER: xhci_speed = 4; mps = 512; break;
        case USB_SPEED_SUPER_PLUS: xhci_speed = 5; mps = 512; break;
        default:             xhci_speed = 4; mps = 512; break;
    }

    slot_ctx->ctx[0] = (1u << 27) | ((uint32_t)xhci_speed << 20);
    /* A device behind a hub names the downstream port it hangs off in the
     * Route String (nibble 0 for a hub that sits directly on a root port). */
    if (parent_slot)
        slot_ctx->ctx[0] |= ((uint32_t)(parent_port & 0xF));
    /* The Root Hub Port Number is the port of the topmost root hub the device
     * is under; `port` carries it for a root device and for a hub child alike. */
    slot_ctx->ctx[1] = ((uint32_t)(port + 1) << 16);
    /* Word 2 holds the TT hub slot / port, needed only by a LS/FS device behind
     * a high-speed hub.  Not wired up yet, so that combination is unsupported. */
    slot_ctx->ctx[2] = 0;

    xhci_ep_ctx_t *ep0 = (xhci_ep_ctx_t *)(input + ctx_off * 2);

    xhci_setup_ep_ring(priv, slot, 1);
    xhci_ring_t *ep0_ring = &priv->ep_rings[slot][0];
    if (!ep0_ring->ring) {
        spin_unlock(&priv->ctx_lock);
        return -1;
    }

    ep0->ctx[1] = (XHCI_EP_CTX_TYPE_CTRL_BI << 3) | (3u << 1) | ((uint32_t)mps << 16);
    ep0->ctx[2] = xhci_va_to_pa(&ep0_ring->ring[ep0_ring->dequeue])
                | (ep0_ring->cycle ? 1u : 0u);
    ep0->ctx[3] = 0;   /* dequeue pointer high dword (32-bit phys => 0) */
    ep0->ctx[4] = 8;   /* EP_AVG_TRB_LENGTH(8), as Linux sets it */

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

    /* Context Entries must be the *highest* valid DCI and may never shrink: a
     * Configure Endpoint that reports fewer entries than an already-enabled
     * endpoint is a Context State Error, and the controller does not complete
     * it — which surfaced as a ~0.5 s timeout + abort every time a device
     * (a flash drive, whose bulk IN/OUT order is device-specific) configured an
     * endpoint with a smaller DCI after a larger one. */
    uint32_t old_entries = (old_slot->ctx[0] >> 27) & 0x1Fu;
    uint32_t entries = (old_entries > (uint32_t)dci) ? old_entries : (uint32_t)dci;
    slot_ctx->ctx[0] = (old_slot->ctx[0] & ~(0x1Fu << 27)) | (entries << 27);
    for (int i = 1; i < 8; i++) slot_ctx->ctx[i] = old_slot->ctx[i];

    xhci_ep_ctx_t *ep = (xhci_ep_ctx_t *)(input + ctx_off * (dci + 1));

    xhci_setup_ep_ring(priv, slot, dci);
    xhci_ring_t *ring = &priv->ep_rings[slot][dci - 1];
    if (!ring->ring) {
        spin_unlock(&priv->ctx_lock);
        return -1;
    }

    ep->ctx[0] = ((uint32_t)interval << 16);
    ep->ctx[1] = ((uint32_t)ep_type << 3) | (3u << 1) | ((uint32_t)mps << 16);
    /* Hand back the *current* dequeue position, not the ring base: a
     * re-configured endpoint must keep whatever it has already consumed. */
    ep->ctx[2] = xhci_va_to_pa(&ring->ring[ring->dequeue])
               | (ring->cycle ? 1u : 0u);
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

/* Mark a slot as a hub in the controller's slot context: the xHC wants the Hub
 * bit set and the downstream port count to route to devices behind it (Linux's
 * xhci_update_hub_device).  Issued as a Configure Endpoint that updates only
 * the slot context, copying the current output context first so nothing else
 * changes. */
int xhci_update_hub(xhci_priv_t *priv, uint8_t slot, uint8_t num_ports) {
    if (!priv)
        return -1;
    if (slot == 0 || slot > priv->max_slots || slot > XHCI_MAX_SLOTS)
        return -1;

    spin_lock(&priv->ctx_lock);
    uint8_t *input = xhci_get_input_ctx(priv);
    memset(input, 0, 2048);

    xhci_input_ctrl_ctx_t *icc = (xhci_input_ctrl_ctx_t *)input;
    icc->add_flags = (1u << 0);              /* slot context only */

    uint32_t ctx_off = priv->context_size;
    xhci_slot_ctx_t *slot_ctx = (xhci_slot_ctx_t *)(input + ctx_off);
    xhci_slot_ctx_t *cur = (xhci_slot_ctx_t *)xhci_get_dev_ctx(priv, slot);
    for (int i = 0; i < 8; i++) slot_ctx->ctx[i] = cur->ctx[i];

    slot_ctx->ctx[0] |= (1u << 26);                           /* Hub */
    slot_ctx->ctx[1] |= ((uint32_t)num_ports << 24);          /* Number of Ports */

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

/* Poll PORTSC until the reset completion bit is raised (PRC for a hot reset,
 * WRC for a warm one) or the port reports itself enabled with the reset bit it
 * started from already cleared (some controllers complete that way instead of
 * raising the change bit).  Returns the last PORTSC value read. */
static uint32_t xhci_port_wait_reset(xhci_priv_t *priv, uint8_t port,
                                     uint32_t done_bit, uint32_t reset_bit) {
    uint32_t sc = xhci_portsc_read(priv, port);
    for (int i = 0; i < 800; i++) {
        if (sc & done_bit) break;
        /* Only a link that is really up counts as done: PED alone can still be
         * the stale value from before the reset was asserted. */
        if (!(sc & reset_bit) && (sc & XHCI_PORTSC_PED)
            && xhci_portsc_pls(sc) == XHCI_PORT_PLS_U0)
            break;
        xhci_udelay(1000);
        sc = xhci_portsc_read(priv, port);
    }
    return sc;
}

/* Bring a root port up and leave it enabled; returns 0 when it reports PED.
 *
 * A USB3 (SuperSpeed) port is enabled by the xHC itself as soon as the link
 * trains, but that is not a USB reset: the device is only put into the Default
 * state that Address Device requires by an actual port reset.  Such a port is
 * therefore reset even though it already reports PED.  A USB2 port is not
 * enabled until it is reset, so there only a not-enabled port needs one — which
 * also keeps a healthy USB2 port from being disturbed.  A link that came up
 * Inactive or in Compliance Mode — or that carries Cold Attach Status
 * (PORT_CAS, a device attached while the system was asleep) — needs a warm
 * reset (WR), which retrains the link.  As Linux does, a hot reset that leaves
 * the link Inactive/Compliance is escalated to a warm one.  Note also that a
 * write to the PLS field only latches with the Link State Write Strobe set. */
int xhci_port_reset(usb_hc_t *hc, uint8_t port) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
    if (port >= priv->max_ports)
        return -1;

    uint32_t sc = xhci_portsc_read(priv, port);
    if (!(sc & XHCI_PORTSC_CCS)) return -1;

    /* A port can still be unpowered (some firmware leaves PP clear); a reset
     * written to an unpowered port is ignored. */
    if (!(sc & XHCI_PORTSC_PP)) {
        xhci_portsc_set(priv, port, XHCI_PORTSC_PP);
        xhci_udelay(20000);
        sc = xhci_portsc_read(priv, port);
    }

    /* A freshly attached device walks the link through RxDetect and Polling on
     * its own (a USB3 link is allowed tPollingLFPS = 360 ms to train); a reset
     * written while that is still running is ignored or aborts the training.
     * Give the link time to settle before deciding what the port needs. */
    if (!(sc & XHCI_PORTSC_PED)) {
        for (int i = 0; i < 400; i++) {
            uint32_t pls = xhci_portsc_pls(sc);
            if (pls != XHCI_PORT_PLS_POLLING && pls != XHCI_PORT_PLS_RXDETECT)
                break;
            xhci_udelay(1000);
            sc = xhci_portsc_read(priv, port);
            if (sc & XHCI_PORTSC_PED) break;
        }
    }

    int warm = (sc & XHCI_PORTSC_CAS)
            || xhci_portsc_pls(sc) == XHCI_PORT_PLS_INACTIVE
            || xhci_portsc_pls(sc) == XHCI_PORT_PLS_COMPLIANCE;

    uint32_t port_speed = (sc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
    int superspeed = (port_speed >= XHCI_PORT_SPEED_SS);

    if (!warm && (!(sc & XHCI_PORTSC_PED) || superspeed)) {
        xhci_portsc_clear_change(priv, port, XHCI_PORTSC_RW1C_BITS);
        xhci_portsc_set(priv, port, XHCI_PORTSC_PP | XHCI_PORTSC_PR);
        sc = xhci_port_wait_reset(priv, port, XHCI_PORTSC_PRC, XHCI_PORTSC_PR);
        /* A hot reset that leaves the link Inactive or in Compliance Mode has
         * failed; a warm reset retrains it. */
        if (!(sc & XHCI_PORTSC_PED)
            && (xhci_portsc_pls(sc) == XHCI_PORT_PLS_INACTIVE
                || xhci_portsc_pls(sc) == XHCI_PORT_PLS_COMPLIANCE))
            warm = 1;
    }

    if (warm) {
        xhci_portsc_clear_change(priv, port, XHCI_PORTSC_RW1C_BITS);
        xhci_portsc_set(priv, port, XHCI_PORTSC_PP | XHCI_PORTSC_WR);
        sc = xhci_port_wait_reset(priv, port, XHCI_PORTSC_WRC, XHCI_PORTSC_WR);
    }

    /* Clear everything the reset latched: a stuck change bit keeps the port
     * from accepting a later reset. */
    xhci_portsc_clear_change(priv, port, XHCI_PORTSC_RW1C_BITS);
    xhci_udelay(10000);
    sc = xhci_portsc_read(priv, port);

    /* A SuperSpeed port must be enabled *and* have its link in U0 before the
     * device can be addressed; a link still training or recovering makes
     * Address Device fail.  A USB2 port only needs PED. */
    if (superspeed) {
        for (int i = 0; i < 400; i++) {
            if ((sc & XHCI_PORTSC_PED)
                && xhci_portsc_pls(sc) == XHCI_PORT_PLS_U0)
                return 0;
            xhci_udelay(1000);
            sc = xhci_portsc_read(priv, port);
        }
        return -1;
    }

    if (sc & XHCI_PORTSC_PED)
        return 0;

    /* USB2 link training can still be in progress; it is allowed up to 360 ms
     * (tPollingLFPS). */
    for (int i = 0; i < 10 && xhci_portsc_pls(sc) == XHCI_PORT_PLS_POLLING; i++) {
        xhci_udelay(36000);
        sc = xhci_portsc_read(priv, port);
    }
    if (sc & XHCI_PORTSC_PED)
        return 0;

    /* USB2 last resort: the port never left a state a reset can complete from —
     * force it back to RxDetect (a PLS write, so LWS is required) and reset
     * once more. */
    if (!warm && xhci_portsc_pls(sc) != XHCI_PORT_PLS_POLLING) {
        xhci_portsc_set_link_state(priv, port, XHCI_PORT_PLS_RXDETECT);
        xhci_udelay(20000);
        xhci_portsc_clear_change(priv, port, XHCI_PORTSC_RW1C_BITS);
        xhci_portsc_set(priv, port, XHCI_PORTSC_PP | XHCI_PORTSC_PR);
        sc = xhci_port_wait_reset(priv, port, XHCI_PORTSC_PRC, XHCI_PORTSC_PR);
        xhci_portsc_clear_change(priv, port, XHCI_PORTSC_RW1C_BITS);
        xhci_udelay(10000);
        sc = xhci_portsc_read(priv, port);
    }

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
            s->ring   = NULL;
        }
    }

    if (dev->port < XHCI_MAX_PORTS && priv->port_dev[dev->port] == dev)
        priv->port_dev[dev->port] = NULL;

    /* Do not issue Disable Slot here: this can be reached with the event lock
     * held (a hub's interrupt report is processed inside xhci_drain_events),
     * and a command would deadlock on that lock.  Hand the slot to the hotplug
     * task, which runs in plain task context. */
    uint8_t slot = dev->address;
    if (slot && slot <= priv->max_slots && slot <= XHCI_MAX_SLOTS)
        __sync_fetch_and_or(&priv->slot_reap, 1u << slot);
}

/* Encode an interrupt endpoint's bInterval into the Endpoint Context Interval
 * field.
 *
 * The two mean different things, so the descriptor value cannot be dropped in
 * raw.  The field is a power of two in 125 us microframes (2^n * 125 us), while
 * bInterval is a count of microframes for a high-speed endpoint but a count of
 * 1 ms frames for a low/full-speed one.  A full-speed keyboard reports
 * bInterval 10, which is 8 ms and therefore 6 in the field; passing 10 through
 * programmed the endpoint to be polled every 2^10 * 125 us = 128 ms.  At that
 * rate a keystroke is pressed and released between two polls, so the keyboard
 * has no report to send when it is finally asked — the key is simply lost, and
 * only keys held long enough to survive a poll ever register. */
static uint8_t xhci_encode_intr_interval(uint8_t speed, uint8_t binterval) {
    if (speed == USB_SPEED_HIGH) {
        /* bInterval = 2^(bInterval-1) microframes -> field = bInterval-1. */
        if (binterval < 1)  binterval = 1;
        if (binterval > 16) binterval = 16;
        return (uint8_t)(binterval - 1);
    }

    /* Low/full speed: bInterval is in 1 ms frames.  The field can only express
     * periods that are a power of two, so round down to the next one: polling
     * faster than the device promised is harmless, polling slower loses keys.
     * Values 3..10 cover the legal 1..128 ms range; bInterval 0 means "no
     * interval given" and gets the 1 ms floor. */
    uint32_t uframes = (uint32_t)binterval * 8;   /* 1 frame = 8 microframes */
    uint8_t  exp     = 3;
    while (exp < 10 && (1u << (exp + 1)) <= uframes)
        exp++;
    return exp;
}

int xhci_register_interrupt_ep(usb_hc_t *hc, usb_device_t *dev,
                               uint8_t ep_num, void *buf, uint16_t len,
                               usb_irq_notify_fn_t notify, void *notify_priv) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;

    /* Take the first free slot, not the next index.  A slot is released when
     * its device is unplugged (xhci_device_removed), but the count only ever
     * grew, so eight plug/unplug cycles exhausted the table and every later
     * HID probe failed with "slots full" — the keyboard was gone for good. */
    xhci_intr_ep_slot_t *s = NULL;
    int s_idx = 0;
    for (int i = 0; i < XHCI_MAX_INTR_EP; i++) {
        if (!priv->intr_slots[i].active) {
            s     = &priv->intr_slots[i];
            s_idx = i;
            break;
        }
    }
    if (!s) {
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
    uint8_t binterval = ep ? ep->interval : 8;
    uint8_t interval  = xhci_encode_intr_interval(dev->speed, binterval);

    pr_info("  %-11s : ep%u slot %u dci %u mps %u interval %u (bInterval %u) len %u\n",
            "xhci-ep", (unsigned)ep_num, (unsigned)slot, (unsigned)dci,
            (unsigned)mps, (unsigned)interval, (unsigned)binterval, (unsigned)len);

    if (xhci_configure_endpoint(priv, slot, dci, XHCI_EP_CTX_TYPE_INTR_IN, mps, interval) < 0)
        return -1;

    /* The event path only scans slots below intr_ep_count, so keep it one past
     * the highest slot in use. */
    if (s_idx + 1 > priv->intr_ep_count)
        priv->intr_ep_count = s_idx + 1;

    s->slot_id     = slot;
    s->dci         = dci;
    s->dev         = dev;
    s->ep_num      = ep_num;
    s->buf         = buf;
    s->len         = len;
    s->notify      = notify;
    s->notify_priv = notify_priv;
    s->err_logged  = 0;
    s->active      = 1;
    s->ring        = &priv->ep_rings[slot][dci - 1];

    xhci_trb_t trb;
    memset(&trb, 0, sizeof(trb));
    trb.param_lo = xhci_va_to_pa(buf);
    trb.status   = len;
    trb.control  = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
    if (xhci_ring_enqueue(s->ring, &trb) < 0) {
        pr_warn("xHCI: interrupt ring full for slot %u dci %u\n",
                (unsigned)slot, (unsigned)dci);
        s->active = 0;
        s->ring   = NULL;
        return -1;
    }
    xhci_db_write32(priv, slot, dci);

    return 0;
}

int xhci_configure_device_endpoints(usb_hc_t *hc, usb_device_t *dev) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
    uint8_t slot = dev->address;
    int configured = 0;

    for (int i = 0; i < dev->ep_count; i++) {
        usb_endpoint_t *ep = &dev->ep[i];
        uint8_t type;

        /* Only bulk endpoints are brought up here; interrupt endpoints are
         * configured by their class driver (e.g. HID) when it starts a
         * transfer, so re-configuring them would disturb its ring. */
        if (ep->transfer_type != USB_TRANSFER_BULK)
            continue;

        type = (ep->direction == USB_DIR_IN) ? XHCI_EP_CTX_TYPE_BULK_IN
                                             : XHCI_EP_CTX_TYPE_BULK_OUT;

        uint8_t dci = (uint8_t)((ep->address * 2) +
                                (ep->direction == USB_DIR_IN ? 1 : 0));

        int rc = xhci_configure_endpoint(priv, slot, dci, type,
                                         ep->max_packet, ep->interval);
        pr_info("  %-11s : bulk ep%u %s dci %u mps %u %s\n", "xhci-ep",
                (unsigned)ep->address,
                ep->direction == USB_DIR_IN ? "IN" : "OUT",
                (unsigned)dci, (unsigned)ep->max_packet,
                rc == 0 ? "ok" : "failed");
        if (rc == 0)
            configured++;
    }
    return configured;
}

uint8_t xhci_port_speed_to_usb(uint32_t portsc) {
    uint8_t ps = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
    switch (ps) {
        case XHCI_PORT_SPEED_LS: return USB_SPEED_LOW;
        case XHCI_PORT_SPEED_FS: return USB_SPEED_FULL;
        case XHCI_PORT_SPEED_HS: return USB_SPEED_HIGH;
        case XHCI_PORT_SPEED_SS: return USB_SPEED_SUPER;
        /* Speed 5 is SuperSpeedPlus (Gen2, 10 Gbps).  Without this it fell into
         * the default and the port was advertised to the controller as High
         * Speed, so a Gen2 root port (often the front-panel ones) could not
         * address the device at all. */
        case XHCI_PORT_SPEED_SSP: return USB_SPEED_SUPER_PLUS;
        default:                 return USB_SPEED_HIGH;
    }
}
