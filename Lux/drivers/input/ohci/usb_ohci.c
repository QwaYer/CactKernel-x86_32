#include "usb_ohci.h"
#include "usb.h"
#include "pci_enum.h"
#include "pci_driver.h"
#include "pci.h"
#include "kernel.h"
#include "memory.h"
#include "libc.h"

static usb_hc_t *ohci_hc_list = NULL;

static inline void ohci_udelay(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 50; i++)
        __asm__ __volatile__("pause");
}

static inline uint32_t ohci_readl(ohci_priv_t *p, uint32_t reg) {
    return p->regs[reg / 4];
}
static inline void ohci_writel(ohci_priv_t *p, uint32_t reg, uint32_t val) {
    p->regs[reg / 4] = val;
}

static ohci_ed_t *ohci_alloc_ed(ohci_priv_t *p) {
    for (int i = 0; i < OHCI_ED_POOL_SIZE; i++) {
        if (!(p->ed_alloc_mask & (1u << i))) {
            p->ed_alloc_mask |= (1u << i);
            memset(&p->ed_pool[i], 0, sizeof(ohci_ed_t));
            return &p->ed_pool[i];
        }
    }
    return NULL;
}

static void ohci_free_ed(ohci_priv_t *p, ohci_ed_t *ed) {
    int i = (int)(ed - p->ed_pool);
    if (i >= 0 && i < OHCI_ED_POOL_SIZE) p->ed_alloc_mask &= ~(1u << i);
}

static ohci_td_t *ohci_alloc_td(ohci_priv_t *p) {
    for (int i = 0; i < OHCI_TD_POOL_SIZE; i++) {
        int w = i/32, b = i%32;
        if (!(p->td_alloc_mask[w] & (1u << b))) {
            p->td_alloc_mask[w] |= (1u << b);
            memset(&p->td_pool[i], 0, sizeof(ohci_td_t));
            return &p->td_pool[i];
        }
    }
    return NULL;
}

static void ohci_free_td(ohci_priv_t *p, ohci_td_t *td) {
    int i = (int)(td - p->td_pool);
    if (i >= 0 && i < OHCI_TD_POOL_SIZE)
        p->td_alloc_mask[i/32] &= ~(1u << (i%32));
}

static int ohci_wait_done(ohci_priv_t *p, uint32_t timeout_ms) {
    uint32_t loops = timeout_ms * 100;
    while (loops--) {
        if (p->xfer_done) {
            p->xfer_done  = 0;
            return p->xfer_error ? -1 : 0;
        }
        uint32_t sts = p->regs[OHCI_INTSTATUS / 4];
        if (sts & (OHCI_INT_WDH | OHCI_INT_UE)) {
            p->regs[OHCI_INTSTATUS / 4] = sts;
            p->xfer_error = (sts & OHCI_INT_UE) ? 1 : 0;
            p->xfer_done  = 0;
            return p->xfer_error ? -1 : 0;
        }
        ohci_udelay(10);
    }
    p->xfer_done = p->xfer_error = 0;
    return -1;
}

static int ohci_control_transfer(usb_hc_t *hc, usb_device_t *dev,
                                  usb_setup_pkt_t *setup,
                                  void *data, uint16_t len)
{
    ohci_priv_t *priv = (ohci_priv_t *)hc->priv;
    uint8_t ls  = (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    uint8_t addr= dev->address;
    uint8_t mps = dev->dev_desc.bMaxPacketSize0;
    if (!mps) mps = 8;

    ohci_td_t *td_tail = ohci_alloc_td(priv);
    if (!td_tail) return -1;
    td_tail->ctrl    = OHCI_TD_CC_NOTACC;
    td_tail->next_sw = NULL;

    ohci_td_t *first = NULL, *last = NULL;

    ohci_td_t *td = ohci_alloc_td(priv);
    if (!td) { ohci_free_td(priv, td_tail); return -1; }
    td->ctrl    = OHCI_TD_CC_NOTACC | OHCI_TD_DP_SETUP
                | OHCI_TD_TOGGLE_DATA0 | OHCI_TD_DI_NONE | OHCI_TD_ROUNDING;
    td->cbp     = (uint32_t)setup;
    td->be      = (uint32_t)setup + 7;
    td->next_td = (uint32_t)td_tail;
    td->next_sw = td_tail;
    first = last = td;

    if (len > 0) {
        uint32_t dp  = (setup->bmRequestType & 0x80) ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT;
        uint8_t *ptr = (uint8_t *)data;
        uint16_t rem = len;
        while (rem > 0) {
            uint16_t   chunk = (rem > mps) ? mps : rem;
            ohci_td_t *tdd   = ohci_alloc_td(priv);
            if (!tdd) goto cleanup;
            tdd->ctrl    = OHCI_TD_CC_NOTACC | dp
                         | OHCI_TD_TOGGLE_DATA1 | OHCI_TD_DI_NONE | OHCI_TD_ROUNDING;
            tdd->cbp     = (uint32_t)ptr;
            tdd->be      = (uint32_t)ptr + chunk - 1;
            tdd->next_td = (uint32_t)td_tail;
            tdd->next_sw = td_tail;
            last->next_td= (uint32_t)tdd;
            last->next_sw= tdd;
            last = tdd;
            ptr += chunk; rem -= chunk;
        }
    }

    {
        ohci_td_t *tds = ohci_alloc_td(priv);
        if (!tds) goto cleanup;
        uint32_t sdp = (setup->bmRequestType & 0x80) ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN;
        tds->ctrl    = OHCI_TD_CC_NOTACC | sdp
                     | OHCI_TD_TOGGLE_DATA1 | OHCI_TD_DI(0) | OHCI_TD_ROUNDING;
        tds->cbp     = 0; tds->be = 0;
        tds->next_td = (uint32_t)td_tail;
        tds->next_sw = td_tail;
        last->next_td= (uint32_t)tds;
        last->next_sw= tds;
        last = tds;
    }

    priv->ctrl_ed->ctrl    = OHCI_ED_ADDR(addr) | OHCI_ED_EN(0)
                           | OHCI_ED_DIR_TD | OHCI_ED_MAXPKT(mps)
                           | (ls ? OHCI_ED_SPEED_LS : 0);
    priv->ctrl_ed->tail_td = (uint32_t)td_tail;
    priv->ctrl_ed->head_td = (uint32_t)first;

    ohci_writel(priv, OHCI_CTRL_HEAD_ED, (uint32_t)priv->ctrl_ed);

    priv->xfer_done  = 0;
    priv->xfer_error = 0;

    ohci_writel(priv, OHCI_CMDSTATUS,
                ohci_readl(priv, OHCI_CMDSTATUS) | OHCI_CMD_CLF);
    ohci_writel(priv, OHCI_CONTROL,
                ohci_readl(priv, OHCI_CONTROL) | OHCI_CTRL_CLE);

    int rc = ohci_wait_done(priv, 200);

    ohci_writel(priv, OHCI_CONTROL,
                ohci_readl(priv, OHCI_CONTROL) & ~OHCI_CTRL_CLE);
    ohci_writel(priv, OHCI_CTRL_HEAD_ED, 0);
    priv->ctrl_ed->head_td = (uint32_t)td_tail | OHCI_ED_HEAD_HALT;

cleanup:
    for (ohci_td_t *t = first; t != td_tail; ) {
        ohci_td_t *nx = t->next_sw; ohci_free_td(priv, t); t = nx;
    }
    ohci_free_td(priv, td_tail);
    return rc;
}

static int ohci_interrupt_transfer(usb_hc_t *hc, usb_device_t *dev,
                                    uint8_t ep_num, void *buf, uint16_t len)
{
    ohci_priv_t *priv = (ohci_priv_t *)hc->priv;
    uint8_t ls   = (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    uint8_t addr = dev->address;

    usb_endpoint_t *ep = NULL;
    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->ep[i].address == ep_num && dev->ep[i].direction == USB_DIR_IN) {
            ep = &dev->ep[i]; break;
        }
    }
    uint16_t mps = ep ? ep->max_packet : 8;
    if (len > mps) len = mps;

    ohci_td_t *td_tail = ohci_alloc_td(priv);
    if (!td_tail) return -1;
    td_tail->ctrl    = OHCI_TD_CC_NOTACC;
    td_tail->next_sw = NULL;

    ohci_td_t *td = ohci_alloc_td(priv);
    if (!td) { ohci_free_td(priv, td_tail); return -1; }
    td->ctrl    = OHCI_TD_CC_NOTACC | OHCI_TD_DP_IN
                | OHCI_TD_TOGGLE_CARRY | OHCI_TD_DI(0) | OHCI_TD_ROUNDING;
    td->cbp     = (uint32_t)buf;
    td->be      = (uint32_t)buf + len - 1;
    td->next_td = (uint32_t)td_tail;
    td->next_sw = td_tail;

    ohci_ed_t *ed = ohci_alloc_ed(priv);
    if (!ed) { ohci_free_td(priv, td); ohci_free_td(priv, td_tail); return -1; }
    ed->ctrl    = OHCI_ED_ADDR(addr) | OHCI_ED_EN(ep_num)
                | OHCI_ED_DIR_IN | OHCI_ED_MAXPKT(mps)
                | (ls ? OHCI_ED_SPEED_LS : 0);
    ed->tail_td = (uint32_t)td_tail;
    ed->head_td = (uint32_t)td;
    ed->next_ed = 0;

    ohci_writel(priv, OHCI_CTRL_HEAD_ED, (uint32_t)ed);

    priv->xfer_done  = 0;
    priv->xfer_error = 0;

    ohci_writel(priv, OHCI_CMDSTATUS,
                ohci_readl(priv, OHCI_CMDSTATUS) | OHCI_CMD_CLF);
    ohci_writel(priv, OHCI_CONTROL,
                ohci_readl(priv, OHCI_CONTROL) | OHCI_CTRL_CLE);

    int rc = ohci_wait_done(priv, 50);

    ohci_writel(priv, OHCI_CONTROL,
                ohci_readl(priv, OHCI_CONTROL) & ~OHCI_CTRL_CLE);
    ohci_writel(priv, OHCI_CTRL_HEAD_ED, 0);

    int actlen = -1;
    if (rc == 0) {
        actlen = td->cbp ? (int)(td->cbp - (uint32_t)buf) : (int)len;
        if (ep) ep->toggle ^= 1;
    }
    ohci_free_td(priv, td);
    ohci_free_td(priv, td_tail);
    ohci_free_ed(priv, ed);
    return actlen;
}

static int ohci_bulk_transfer(usb_hc_t *hc, usb_device_t *dev,
                               uint8_t ep_num, uint8_t dir,
                               void *buf, uint16_t len)
{
    ohci_priv_t *priv = (ohci_priv_t *)hc->priv;
    uint8_t  ls   = (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    uint8_t  addr = dev->address;
    uint32_t dp   = (dir == USB_DIR_IN) ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT;
    uint16_t mps  = 64;

    usb_endpoint_t *ep = NULL;
    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->ep[i].address == ep_num && dev->ep[i].direction == dir) {
            ep = &dev->ep[i]; mps = ep->max_packet; break;
        }
    }

    ohci_td_t *td_tail = ohci_alloc_td(priv);
    if (!td_tail) return -1;
    td_tail->ctrl    = OHCI_TD_CC_NOTACC;
    td_tail->next_sw = NULL;

    ohci_td_t *first = NULL, *last = td_tail;
    uint8_t *ptr = (uint8_t *)buf;
    uint16_t rem = len;

    while (rem > 0) {
        uint16_t   chunk = (rem > mps) ? mps : rem;
        ohci_td_t *td    = ohci_alloc_td(priv);
        if (!td) goto bulk_cleanup;
        td->ctrl    = OHCI_TD_CC_NOTACC | dp | OHCI_TD_TOGGLE_CARRY
                    | OHCI_TD_ROUNDING
                    | (rem <= mps ? OHCI_TD_DI(0) : OHCI_TD_DI_NONE);
        td->cbp     = (uint32_t)ptr;
        td->be      = (uint32_t)ptr + chunk - 1;
        td->next_td = (uint32_t)td_tail;
        td->next_sw = td_tail;
        if (!first) first = td;
        else { last->next_td = (uint32_t)td; last->next_sw = td; }
        last = td;
        ptr += chunk; rem -= chunk;
    }
    if (!first) { ohci_free_td(priv, td_tail); return 0; }

    ohci_ed_t *ed = ohci_alloc_ed(priv);
    if (!ed) goto bulk_cleanup;

    ed->ctrl    = OHCI_ED_ADDR(addr) | OHCI_ED_EN(ep_num)
                | (dir == USB_DIR_IN ? OHCI_ED_DIR_IN : OHCI_ED_DIR_OUT)
                | OHCI_ED_MAXPKT(mps)
                | (ls ? OHCI_ED_SPEED_LS : 0);
    ed->tail_td = (uint32_t)td_tail;
    ed->head_td = (uint32_t)first;
    ed->next_ed = 0;

    ohci_writel(priv, OHCI_BULK_HEAD_ED, (uint32_t)ed);

    priv->xfer_done  = 0;
    priv->xfer_error = 0;

    ohci_writel(priv, OHCI_CMDSTATUS,
                ohci_readl(priv, OHCI_CMDSTATUS) | OHCI_CMD_BLF);
    ohci_writel(priv, OHCI_CONTROL,
                ohci_readl(priv, OHCI_CONTROL) | OHCI_CTRL_BLE);

    int rc = ohci_wait_done(priv, 500);

    ohci_writel(priv, OHCI_CONTROL,
                ohci_readl(priv, OHCI_CONTROL) & ~OHCI_CTRL_BLE);
    ohci_writel(priv, OHCI_BULK_HEAD_ED, 0);
    ohci_free_ed(priv, ed);

bulk_cleanup:
    for (ohci_td_t *t = first; t != td_tail; ) {
        ohci_td_t *nx = t->next_sw; ohci_free_td(priv, t); t = nx;
    }
    ohci_free_td(priv, td_tail);
    return rc == 0 ? len : -1;
}

static int ohci_port_reset(usb_hc_t *hc, uint8_t port) {
    ohci_priv_t *priv = (ohci_priv_t *)hc->priv;
    uint32_t reg = OHCI_RH_PORT_STATUS + port * 4;

    if (!(ohci_readl(priv, reg) & OHCI_PORT_CCS)) return -1;

    ohci_writel(priv, reg, OHCI_PORT_PRS);
    for (int i = 0; i < 200; i++) {
        ohci_udelay(1000);
        if (ohci_readl(priv, reg) & OHCI_PORT_PRSC) break;
    }
    ohci_writel(priv, reg, OHCI_PORT_PRSC);
    ohci_writel(priv, reg, OHCI_PORT_PES);
    ohci_udelay(2000);

    return (ohci_readl(priv, reg) & OHCI_PORT_PES) ? 0 : -1;
}

static int ohci_port_get_status(usb_hc_t *hc, uint8_t port) {
    ohci_priv_t *priv = (ohci_priv_t *)hc->priv;
    return (int)ohci_readl(priv, OHCI_RH_PORT_STATUS + port * 4);
}

int ohci_register_interrupt_ed(usb_hc_t *hc, usb_device_t *dev,
                                 uint8_t ep_num, void *buf, uint16_t len,
                                 usb_irq_notify_fn_t notify, void *notify_priv)
{
    ohci_priv_t *priv = (ohci_priv_t *)hc->priv;

    if (priv->intr_ed_count >= OHCI_MAX_INTR_ED) return -1;
    ohci_intr_ed_slot_t *slot = &priv->intr_slots[priv->intr_ed_count++];

    usb_endpoint_t *ep = NULL;
    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->ep[i].address == ep_num && dev->ep[i].direction == USB_DIR_IN) {
            ep = &dev->ep[i]; break;
        }
    }
    uint16_t mps = ep ? ep->max_packet : 8;
    if (len > mps) len = mps;

    uint8_t ls = (dev->speed == USB_SPEED_LOW) ? 1 : 0;

    ohci_td_t *td_tail = ohci_alloc_td(priv);
    if (!td_tail) { priv->intr_ed_count--; return -1; }
    td_tail->ctrl    = OHCI_TD_CC_NOTACC;
    td_tail->next_sw = NULL;

    ohci_td_t *td = ohci_alloc_td(priv);
    if (!td) { ohci_free_td(priv, td_tail); priv->intr_ed_count--; return -1; }
    td->ctrl    = OHCI_TD_CC_NOTACC | OHCI_TD_DP_IN
                | OHCI_TD_TOGGLE_CARRY | OHCI_TD_DI(0) | OHCI_TD_ROUNDING;
    td->cbp     = (uint32_t)buf;
    td->be      = (uint32_t)buf + len - 1;
    td->next_td = (uint32_t)td_tail;
    td->next_sw = td_tail;

    ohci_ed_t *ed = ohci_alloc_ed(priv);
    if (!ed) {
        ohci_free_td(priv, td); ohci_free_td(priv, td_tail);
        priv->intr_ed_count--; return -1;
    }
    ed->ctrl    = OHCI_ED_ADDR(dev->address) | OHCI_ED_EN(ep_num)
                | OHCI_ED_DIR_IN | OHCI_ED_MAXPKT(mps)
                | (ls ? OHCI_ED_SPEED_LS : 0);
    ed->tail_td = (uint32_t)td_tail;
    ed->head_td = (uint32_t)td;

    uint8_t interval = ep ? ep->interval : 8;
    if (!interval) interval = 1;
    for (int f = 0; f < 32; f += interval) {
        ed->next_ed = priv->hcca->interrupt_table[f];
        priv->hcca->interrupt_table[f] = (uint32_t)ed;
    }

    slot->ed          = ed;
    slot->td          = td;
    slot->td_tail     = td_tail;
    slot->buf         = buf;
    slot->len         = len;
    slot->dev         = dev;
    slot->ep_num      = ep_num;
    slot->active      = 1;
    slot->notify      = notify;
    slot->notify_priv = notify_priv;

    ohci_writel(priv, OHCI_CONTROL,
                ohci_readl(priv, OHCI_CONTROL) | OHCI_CTRL_PLE);

    return 0;
}

static void ohci_process_done_head(usb_hc_t *hc) {
    ohci_priv_t *priv = (ohci_priv_t *)hc->priv;

    uint32_t done_phys = priv->hcca->done_head & ~1u;
    priv->hcca->done_head = 0;
    if (!done_phys) return;

    ohci_td_t *done_td = (ohci_td_t *)done_phys;
    while (done_td) {
        ohci_td_t *current = done_td;
        done_td = (ohci_td_t *)(current->next_td & ~0xFu);

        uint32_t cc = (current->ctrl & OHCI_TD_CC_MASK) >> 28;

        int is_intr = 0;
        for (int i = 0; i < priv->intr_ed_count; i++) {
            ohci_intr_ed_slot_t *slot = &priv->intr_slots[i];
            if (!slot->active || current != slot->td) continue;
            is_intr = 1;

            if (cc == 0x00) {
                uint16_t actlen = current->cbp
                    ? (uint16_t)(current->cbp - (uint32_t)slot->buf)
                    : slot->len;
                if (slot->notify)
                    slot->notify(slot->dev, slot->buf, actlen, slot->notify_priv);

                usb_endpoint_t *ep = NULL;
                for (int j = 0; j < slot->dev->ep_count; j++) {
                    if (slot->dev->ep[j].address == slot->ep_num &&
                        slot->dev->ep[j].direction == USB_DIR_IN) {
                        ep = &slot->dev->ep[j]; break;
                    }
                }
                if (ep) ep->toggle ^= 1;
            }

            current->ctrl    = OHCI_TD_CC_NOTACC | OHCI_TD_DP_IN
                             | OHCI_TD_TOGGLE_CARRY | OHCI_TD_DI(0) | OHCI_TD_ROUNDING;
            current->cbp     = (uint32_t)slot->buf;
            current->be      = (uint32_t)slot->buf + slot->len - 1;
            current->next_td = (uint32_t)slot->td_tail;
            slot->ed->head_td = (uint32_t)current;
            break;
        }

        if (!is_intr) {
            if (cc != 0x00 && cc != 0x0F)
                priv->xfer_error = 1;
            priv->xfer_done = 1;
        }
    }
}

static void ohci_handle_irq(usb_hc_t *hc) {
    ohci_priv_t *priv = (ohci_priv_t *)hc->priv;

    uint32_t ints = ohci_readl(priv, OHCI_INTSTATUS);
    ints &= ohci_readl(priv, OHCI_INTENABLE);
    if (!ints) return;

    ohci_writel(priv, OHCI_INTDISABLE, OHCI_INT_MIE);

    if (ints & OHCI_INT_WDH) {
        ohci_process_done_head(hc);
        ohci_writel(priv, OHCI_INTSTATUS, OHCI_INT_WDH);
    }

    if (ints & OHCI_INT_RHSC) {
        for (uint8_t p = 0; p < priv->num_ports; p++) {
            uint32_t ps = ohci_readl(priv, OHCI_RH_PORT_STATUS + p * 4);
            if (ps & OHCI_PORT_CSC) {
                ohci_writel(priv, OHCI_RH_PORT_STATUS + p * 4, OHCI_PORT_CSC);
                if (ps & OHCI_PORT_CCS) {
                    uint8_t speed = (ps & OHCI_PORT_LSDA) ? USB_SPEED_LOW
                                                           : USB_SPEED_FULL;
                    usb_device_enumerate(hc, p, speed);
                } else {
                    usb_device_disconnect(hc, p);
                }
            }
        }
        ohci_writel(priv, OHCI_INTSTATUS, OHCI_INT_RHSC);
    }

    if (ints & OHCI_INT_UE) {
        kprint("[OHCI] Unrecoverable Error\n");
        ohci_writel(priv, OHCI_CMDSTATUS, OHCI_CMD_HCR);
        ohci_udelay(50000);
        ohci_writel(priv, OHCI_CONTROL,
                    OHCI_HCFS_OPERATIONAL | OHCI_CTRL_CLE | OHCI_CTRL_PLE);
        ohci_writel(priv, OHCI_INTSTATUS, OHCI_INT_UE);
        priv->xfer_error = 1;
        priv->xfer_done  = 1;
    }

    ohci_writel(priv, OHCI_INTENABLE, OHCI_INT_MIE);
}

void ohci_irq_handler(void) {
    for (usb_hc_t *hc = ohci_hc_list; hc; hc = hc->irq_next)
        ohci_handle_irq(hc);
}

static int ohci_init_one(uint32_t phys_base) {
    extern uint32_t page_directory[1024];
    for (uint32_t off = 0; off < 0x4000; off += 0x1000)
        vmm_map(page_directory, phys_base + off, phys_base + off,
                PAGE_PRESENT | PAGE_RW);

    volatile uint32_t *regs = (volatile uint32_t *)phys_base;

    ohci_priv_t *priv = (ohci_priv_t *)kmalloc(sizeof(ohci_priv_t));
    if (!priv) { kprint("[OHCI] kmalloc priv failed\n"); return -1; }
    memset(priv, 0, sizeof(ohci_priv_t));
    priv->regs = regs;

    uint8_t *hcca_raw = (uint8_t *)kmalloc(sizeof(ohci_hcca_t) + 256);
    if (!hcca_raw) { kfree_heap(priv); kprint("[OHCI] kmalloc hcca failed\n"); return -1; }
    priv->hcca = (ohci_hcca_t *)(((uint32_t)hcca_raw + 255) & ~255);
    memset(priv->hcca, 0, sizeof(ohci_hcca_t));

    uint8_t *ed_raw = (uint8_t *)kmalloc(sizeof(ohci_ed_t) * OHCI_ED_POOL_SIZE + 16);
    uint8_t *td_raw = (uint8_t *)kmalloc(sizeof(ohci_td_t) * OHCI_TD_POOL_SIZE + 16);
    if (!ed_raw || !td_raw) { kfree_heap(priv); kprint("[OHCI] kmalloc pools failed\n"); return -1; }
    priv->ed_pool = (ohci_ed_t *)(((uint32_t)ed_raw + 15) & ~15);
    priv->td_pool = (ohci_td_t *)(((uint32_t)td_raw + 15) & ~15);

    uint32_t ctrl = ohci_readl(priv, OHCI_CONTROL);

    if (ctrl & OHCI_CTRL_IR) {
        ohci_writel(priv, OHCI_CMDSTATUS, OHCI_CMD_OCR);
        for (int i = 0; i < 100 && (ohci_readl(priv, OHCI_CONTROL) & OHCI_CTRL_IR); i++)
            ohci_udelay(10000);
    }

    ohci_writel(priv, OHCI_CMDSTATUS, OHCI_CMD_HCR);
    for (int i = 0; i < 50; i++) {
        ohci_udelay(1000);
        if (!(ohci_readl(priv, OHCI_CMDSTATUS) & OHCI_CMD_HCR)) break;
    }

    ohci_writel(priv, OHCI_HCCA, (uint32_t)priv->hcca);

    priv->ctrl_ed = ohci_alloc_ed(priv);
    priv->ctrl_ed->ctrl    = OHCI_ED_SKIP;
    priv->ctrl_ed->next_ed = 0;
    ohci_writel(priv, OHCI_CTRL_HEAD_ED, (uint32_t)priv->ctrl_ed);

    ohci_writel(priv, OHCI_FM_INTERVAL,    0xA7782EDF);
    ohci_writel(priv, OHCI_PERIODIC_START, 0x2A2F);

    uint32_t rha = ohci_readl(priv, OHCI_RH_DESCRIPTOR_A);
    priv->num_ports = (uint8_t)(rha & 0xFF);
    if (priv->num_ports > USB_MAX_PORTS) priv->num_ports = USB_MAX_PORTS;

    ohci_writel(priv, OHCI_RH_STATUS, 0x00010000);
    ohci_udelay(100000);

    ohci_writel(priv, OHCI_CONTROL,
                OHCI_HCFS_OPERATIONAL | OHCI_CTRL_CLE | OHCI_CTRL_PLE);

    ohci_writel(priv, OHCI_INTENABLE,
                OHCI_INT_MIE | OHCI_INT_WDH | OHCI_INT_RHSC | OHCI_INT_UE);

    usb_hc_t *hc = (usb_hc_t *)kmalloc(sizeof(usb_hc_t));
    if (!hc) { kfree_heap(priv); return -1; }
    memset(hc, 0, sizeof(usb_hc_t));

    hc->name               = "OHCI";
    hc->control_transfer   = ohci_control_transfer;
    hc->interrupt_transfer = ohci_interrupt_transfer;
    hc->bulk_transfer      = ohci_bulk_transfer;
    hc->port_reset         = ohci_port_reset;
    hc->port_get_status    = ohci_port_get_status;
    hc->num_ports          = priv->num_ports;
    hc->priv               = priv;

    hc->irq_next  = ohci_hc_list;
    ohci_hc_list  = hc;

    usb_hc_register(hc);

    for (uint8_t p = 0; p < priv->num_ports; p++) {
        uint32_t ps = ohci_readl(priv, OHCI_RH_PORT_STATUS + p * 4);
        if (ps & OHCI_PORT_CCS) {
            uint8_t speed = (ps & OHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
            usb_device_enumerate(hc, p, speed);
        }
    }

    return 0;
}

static int ohci_pci_probe(pci_device_t *pdev) {
    if (pdev->prog_if != 0x10) return -1;

    uint32_t mmio = 0;
    for (int i = 0; i < 6; i++) {
        if (!pdev->bars[i].is_io && pdev->bars[i].base) {
            mmio = pdev->bars[i].base; break;
        }
    }
    if (!mmio)
        mmio = pci_read32(pdev->bus, pdev->dev, pdev->fn, 0x10) & ~0xFu;
    if (!mmio) { kprint("[OHCI] No MMIO BAR\n"); return -1; }

    uint32_t cmd = pci_read32(pdev->bus, pdev->dev, pdev->fn, 0x04);
    pci_write32(pdev->bus, pdev->dev, pdev->fn, 0x04, cmd | 0x06);

    return ohci_init_one(mmio);
}

static pci_driver_t ohci_pci_driver = {
    .name       = "ohci_hcd",
    .vendor_id  = PCI_ANY_ID,
    .device_id  = PCI_ANY_ID,
    .class_code = 0x0C,
    .subclass   = 0x03,
    .probe      = ohci_pci_probe,
};

void ohci_pci_init(void) {
    pci_register_driver(&ohci_pci_driver);

    pci_device_t *d = pci_find_by_class(0x0C, 0x03);
    while (d) {
        if (d->prog_if == 0x10) ohci_pci_probe(d);
        d = d->next;
        while (d && !(d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x10))
            d = d->next;
    }
}