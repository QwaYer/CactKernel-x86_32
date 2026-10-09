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
    ring->dequeue = 0;
    ring->cycle   = 1;
    ring->size    = size;
    ring->done        = 0;
    ring->err         = 0;
    ring->last_cc     = 0;
    ring->last_residual = 0;
    ring->xfer_buf   = 0;
    ring->xfer_len   = 0;
    ring->xfer_armed = 0;
    memset(mem, 0, size * sizeof(xhci_trb_t));
    xhci_trb_t *link = &mem[size - 1];
    link->param_lo = xhci_va_to_pa(mem);
    link->param_hi = 0;
    link->status   = 0;
    link->control  = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_CYCLE | (1u << 1);
}

/* Queue one TRB.  The last slot is the Link TRB and is never used for data, so
 * the ring holds size-1 entries; `(enqueue + 1) % (size - 1) == dequeue` is the
 * full condition.  Returning -1 instead of overwriting a live TRB keeps a
 * mis-sized ring from silently corrupting an in-flight TD. */
int xhci_ring_enqueue(xhci_ring_t *ring, xhci_trb_t *trb) {
    uint32_t idx = ring->enqueue;
    uint32_t next = idx + 1;
    if (next >= ring->size - 1)
        next = 0;
    if (next == ring->dequeue)
        return -1;                     /* ring full */

    xhci_trb_t *dst = &ring->ring[idx];

    dst->param_lo = trb->param_lo;
    dst->param_hi = trb->param_hi;
    dst->status   = trb->status;

    uint32_t ctrl = trb->control & ~XHCI_TRB_CYCLE;
    if (ring->cycle)
        ctrl |= XHCI_TRB_CYCLE;
    dst->control = ctrl;

    if (next == 0) {
        /* Wrap through the Link TRB: give it the cycle state the controller
         * will be looking for and flip ours. */
        xhci_trb_t *link = &ring->ring[ring->size - 1];
        uint32_t lc = link->control & ~XHCI_TRB_CYCLE;
        if (ring->cycle)
            lc |= XHCI_TRB_CYCLE;
        link->control = lc;
        ring->cycle ^= 1;
    }
    ring->enqueue = next;
    return 0;
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

/* Process whatever the controller has already posted (task context). */
void xhci_poll_events(xhci_priv_t *priv) {
    irq_spinlock_acquire(&xhci_evt_lock);
    xhci_drain_events(priv);
    irq_spinlock_release(&xhci_evt_lock);
}

/* Wait for one specific completion flag.
 *
 * Commands and transfers must not release each other's wait.  The interrupt
 * endpoints stay armed for as long as the driver is up, so a transfer event
 * can arrive while a command is in flight — and a command wait released early
 * is a command the controller never confirmed.  When that happens to Configure
 * Endpoint the endpoint is not in the controller yet while the doorbell that
 * starts it has already been rung, and the endpoint is then never polled:
 * exactly the shape of "the keyboard re-enumerates but no report ever
 * arrives".  Both submission paths therefore clear the flag they are not
 * waiting on before they start. */
static int xhci_wait_flag(xhci_priv_t *priv, volatile uint8_t *flag,
                          volatile uint8_t *err, uint32_t timeout_ms,
                          const char *what)
{
    uint32_t loops = timeout_ms * 100;
    while (loops--) {
        if (*flag) {
            *flag = 0;
            int e = err ? *err : 0;
            if (err)
                *err = 0;
            return e ? 1 : 0;
        }
        irq_spinlock_acquire(&xhci_evt_lock);
        xhci_drain_events(priv);
        irq_spinlock_release(&xhci_evt_lock);
        if (*flag) {
            *flag = 0;
            int e = err ? *err : 0;
            if (err)
                *err = 0;
            return e ? 1 : 0;
        }
        /* A Host System Error halts the controller and no completion will ever
         * arrive: report it instead of burning the whole timeout. */
        if ((loops % 100u) == 0u) {
            uint32_t sts = xhci_op_read32(priv, XHCI_OP_USBSTS);
            if (sts & XHCI_STS_HSE) {
                pr_err("xHCI: Host System Error during command (USBSTS=0x%x)\n", sts);
                break;
            }
        }
        xhci_udelay(10);
    }
    /* Only named waits report.  A *transfer* timeout is ordinary traffic: a
     * bulk-IN on a quiet endpoint just means "no data" (the device NAKs), and
     * the cases that are real faults — a bulk-OUT or control transfer that
     * never completed — are reported by xhci_recover_ep().  Logging every one
     * of them buried the console in hundreds of lines per scan. */
    if (what)
        pr_warn("xHCI %s timeout\n", what);
    *flag = 0;
    return -1;
}

/* 0 = completed, 1 = completed with an error, -1 = timed out (the TRB is still
 * armed, which for a bulk-IN just means "no data yet"). */
int xhci_wait_transfer(xhci_priv_t *priv, xhci_ring_t *ring, uint32_t timeout_ms) {
    return xhci_wait_flag(priv, &ring->done, &ring->err, timeout_ms, NULL);
}

int xhci_send_cmd(xhci_priv_t *priv, xhci_trb_t *trb) {
    priv->cmd_error  = 0;
    priv->cmd_result = 0;
    priv->cmd_done   = 0;
    if (xhci_ring_enqueue(&priv->cmd_ring, trb) < 0) {
        pr_warn("xHCI: command ring full\n");
        return -1;
    }
    xhci_db_write32(priv, 0, 0);
    int rc = xhci_wait_flag(priv, &priv->cmd_done, &priv->cmd_error, 500,
                            "command");
    if (rc < 0) {
        /* Abort the command the controller never completed, then reap its
         * Command Completion Event.  Without this the ring stays wedged and
         * every later command times out too.  The CRCR value is written from
         * the copy saved at init rather than read back: some controllers do
         * not implement CRCR read-back and return 0, which would clobber the
         * ring pointer. */
        xhci_op_write32(priv, XHCI_OP_CRCR, priv->cmd_ring_phys | 0x1 | (1u << 2));
        xhci_op_write32(priv, XHCI_OP_CRCR + 4, 0);
        xhci_wait_flag(priv, &priv->cmd_done, &priv->cmd_error, 500, NULL);
        return -1;
    }
    return rc == 0 ? 0 : -1;
}

static void xhci_process_event(xhci_priv_t *priv, xhci_trb_t *evt) {
    uint32_t type = (evt->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
    uint8_t  cc   = (uint8_t)((evt->status >> 24) & 0xFF);

    switch (type) {
    case XHCI_TRB_CMD_COMPLETE:
        priv->cmd_result = evt->control;
        priv->cmd_error  = (cc != XHCI_CC_SUCCESS) ? 1 : 0;
        priv->cmd_cc     = cc;
        priv->cmd_done   = 1;
        break;

    case XHCI_TRB_TRANSFER_EVT: {
        uint8_t slot = (uint8_t)((evt->control >> 24) & 0xFF);
        uint8_t dci  = (uint8_t)((evt->control >> 16) & 0x1F);
        int is_intr = 0;

        /* Interrupt endpoints are re-armed continuously and complete on their
         * own schedule.  Their events must NOT release the synchronous
         * control/bulk transfer that is currently waiting on its endpoint: with
         * a single shared flag a HID completion "finished" the control
         * transfer early, leaving it with uninitialised (zero) data. */
        for (int i = 0; i < priv->intr_ep_count; i++) {
            xhci_intr_ep_slot_t *s = &priv->intr_slots[i];
            if (!s->active || s->slot_id != slot || s->dci != dci)
                continue;

            is_intr = 1;
            if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
                if (s->notify)
                    s->notify(s->dev, s->buf, s->len, s->notify_priv);
            } else if (!s->err_logged) {
                s->err_logged = 1;
                pr_warn("xHCI: interrupt EP slot %u dci %u completion code %u\n",
                        (unsigned)slot, (unsigned)dci, (unsigned)cc);
            }

            /* Re-arm whatever the completion said.  The event consumed the
             * TRB, so tying the re-arm to a good completion code left the
             * endpoint idle for good after a single transient error (babble,
             * missed service, ring underrun, a device that answered late) —
             * the keyboard then delivered nothing at all until it was
             * re-plugged.  A halted endpoint simply ignores the doorbell, so
             * the extra TRB costs nothing in that case. */
            xhci_trb_t re_trb;
            memset(&re_trb, 0, sizeof(re_trb));
            re_trb.param_lo = xhci_va_to_pa(s->buf);
            re_trb.status   = s->len;
            re_trb.control  = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;
            if (s->ring) {
                s->ring->dequeue = s->ring->enqueue;
                if (xhci_ring_enqueue(s->ring, &re_trb) == 0)
                    xhci_db_write32(priv, slot, dci);
            }
            break;
        }

        if (!is_intr) {
            /* Release exactly the endpoint this completion belongs to.  The
             * module's register access is control transfers on its own
             * endpoint; with one controller-wide flag those waits consumed a
             * bulk-IN completion, and the RX endpoint then looked armed for
             * ever. */
            if (dci >= 1 && dci <= 31) {
                xhci_ring_t *er = &priv->ep_rings[slot][dci - 1];
                er->err  = (cc == XHCI_CC_SUCCESS ||
                            cc == XHCI_CC_SHORT_PACKET) ? 0 : 1;
                er->last_cc       = cc;
                er->last_residual = evt->status & 0x00FFFFFFu;
                /* Everything up to enqueue was consumed by this TD. */
                er->dequeue = er->enqueue;
                er->done = 1;
            }
        }
        break;
    }

    case XHCI_TRB_PORT_STATUS: {
        /* Record the port and let the hotplug task reset/enumerate.  The change
         * latches are deliberately left set: clearing them here would race the
         * task's own read of PORTSC, and the xHC does not re-post the event
         * until software clears them. */
        uint8_t port = (uint8_t)((evt->param_lo >> 24) & 0xFF) - 1;
        if (port < priv->max_ports)
            xhci_mark_port(priv, port);
        break;
    }

    default:
        break;
    }
}

void xhci_handle_irq(usb_hc_t *hc) {
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;

    uint32_t sts = xhci_op_read32(priv, XHCI_OP_USBSTS);

    if (sts & XHCI_STS_HSE) {
        /* HSE is RW1C.  On Intel 300-series it can be raised spuriously;
         * with the quirk active we ack it so it can never cascade into a
         * chipset-triggered system reboot. */
        xhci_op_write32(priv, XHCI_OP_USBSTS, XHCI_STS_HSE);
        if (priv->quirks & XHCI_QUIRK_SPURIOUS_REBOOT) {
            pr_warn("xHCI: spurious host error ignored (Intel quirk)");
        } else {
            /* A real HSE puts the xHC into a fatal state: clearing the bit is
             * not enough, further register access just hangs the bus.  Reset
             * it here and hand the re-programming/re-enumeration to the
             * hotplug task — that path sleeps and issues commands, which an
             * interrupt handler may not. */
            pr_err("[XHCI] Host System Error! resetting controller\n");
            xhci_op_write32(priv, XHCI_OP_USBCMD, XHCI_CMD_HCRST);
            for (int i = 0; i < 100; i++) {
                if (!(xhci_op_read32(priv, XHCI_OP_USBCMD) & XHCI_CMD_HCRST)) break;
                xhci_udelay(1000);
            }
            xhci_op_write32(priv, XHCI_OP_USBSTS, xhci_op_read32(priv, XHCI_OP_USBSTS));
            priv->reset_pending = 1;
            return;
        }
    }

    /* A port change latches PCD regardless of whether the controller posted a
     * Port Status Change Event; whatever the reason, each port that carries a
     * change bit is handed to the hotplug task. */
    if (sts & XHCI_STS_PCD) {
        xhci_op_write32(priv, XHCI_OP_USBSTS, XHCI_STS_PCD);
        for (uint8_t p = 0; p < priv->max_ports; p++) {
            if (xhci_portsc_read(priv, p) & XHCI_PORTSC_RW1C_BITS)
                xhci_mark_port(priv, p);
        }
    }

    if (!(sts & XHCI_STS_EINT))
        return;

    xhci_op_write32(priv, XHCI_OP_USBSTS, XHCI_STS_EINT);
    irq_spinlock_acquire(&xhci_evt_lock);
    xhci_drain_events(priv);
    irq_spinlock_release(&xhci_evt_lock);

    xhci_rt_write32(priv, 0x20 + XHCI_IMAN, 0x3);
}
