#include "usb_uhci.h"
#include "usb.h"
#include "pci_enum.h"
#include "pci_driver.h"
#include "kernel.h"
#include "memory.h"
#include "libc.h"


static usb_hc_t *uhci_hc_list = NULL;

static inline void uhci_udelay(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 50; i++)
        __asm__ __volatile__("pause");
}

static inline uint16_t uhci_inw(uint16_t b, uint8_t r)  { return port_word_in(b + r); }
static inline void     uhci_outw(uint16_t b, uint8_t r, uint16_t v) { port_word_out(b + r, v); }
static inline uint32_t uhci_inl(uint16_t b, uint8_t r)  { return port_long_in(b + r); }
static inline void     uhci_outl(uint16_t b, uint8_t r, uint32_t v){ port_long_out(b + r, v); }

static uhci_td_t *uhci_alloc_td(uhci_priv_t *p) {
    for (int i = 0; i < UHCI_TD_POOL_SIZE; i++) {
        int w = i/32, b = i%32;
        if (!(p->td_alloc_mask[w] & (1u<<b))) {
            p->td_alloc_mask[w] |= (1u<<b);
            uhci_td_t *td = &p->td_pool[i];
            memset(td, 0, sizeof(uhci_td_t));
            return td;
        }
    }
    return NULL;
}

static void uhci_free_td(uhci_priv_t *p, uhci_td_t *td) {
    int i = (int)(td - p->td_pool);
    if (i >= 0 && i < UHCI_TD_POOL_SIZE)
        p->td_alloc_mask[i/32] &= ~(1u << (i%32));
}

static uhci_qh_t *uhci_alloc_qh(uhci_priv_t *p) {
    for (int i = 0; i < UHCI_QH_POOL_SIZE; i++) {
        if (!(p->qh_alloc_mask & (1u<<i))) {
            p->qh_alloc_mask |= (1u<<i);
            memset(&p->qh_pool[i], 0, sizeof(uhci_qh_t));
            return &p->qh_pool[i];
        }
    }
    return NULL;
}

static int uhci_wait_done(uhci_priv_t *p, uint32_t timeout_ms) {
    uint32_t loops = timeout_ms * 100;
    while (loops--) {
        if (p->xfer_done) {
            p->xfer_done  = 0;
            return p->xfer_error ? -1 : 0;
        }
        uint16_t sts = uhci_inw(p->io_base, UHCI_USBSTS);
        if (sts & (UHCI_STS_USBINT | UHCI_STS_ERROR)) {
            uhci_outw(p->io_base, UHCI_USBSTS, sts);  
            p->xfer_error = (sts & UHCI_STS_ERROR) ? 1 : 0;
            p->xfer_done  = 0;
            return p->xfer_error ? -1 : 0;
        }
        uhci_udelay(10);
    }
    p->xfer_done  = 0;
    p->xfer_error = 0;
    return -1;  
}

static int uhci_control_transfer(usb_hc_t *hc, usb_device_t *dev,
                                  usb_setup_pkt_t *setup,
                                  void *data, uint16_t len)
{
    uhci_priv_t *priv = (uhci_priv_t *)hc->priv;
    uint16_t    base  = priv->io_base;
    uint8_t     ls    = (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    uint8_t     addr  = dev->address;
    uint8_t     mps   = dev->dev_desc.bMaxPacketSize0;
    if (!mps) mps = 8;

    uhci_td_t *first = NULL, *last = NULL;

    uhci_td_t *td = uhci_alloc_td(priv);
    if (!td) return -1;
    td->ctrl_sts = UHCI_TD_ACTIVE | UHCI_TD_ERR_LIMIT(3) | (ls ? UHCI_TD_LS : 0);
    td->token    = UHCI_TD_TOKEN(8, 0, 0, addr, UHCI_TD_PID_SETUP);
    td->buf_ptr  = (uint32_t)setup;
    td->link     = UHCI_LINK_TERM;
    td->next_sw  = NULL;
    first = last = td;

    uint8_t  toggle = 1;
    uint8_t  pid    = (setup->bmRequestType & 0x80) ? UHCI_TD_PID_IN
                                                     : UHCI_TD_PID_OUT;
    uint8_t *ptr    = (uint8_t *)data;
    uint16_t rem    = len;

    while (rem > 0) {
        uint16_t chunk = (rem > mps) ? mps : rem;
        td = uhci_alloc_td(priv);
        if (!td) goto cleanup;
        td->ctrl_sts = UHCI_TD_ACTIVE | UHCI_TD_ERR_LIMIT(3) | (ls ? UHCI_TD_LS : 0);
        td->token    = UHCI_TD_TOKEN(chunk, toggle, 0, addr, pid);
        td->buf_ptr  = (uint32_t)ptr;
        td->link     = UHCI_LINK_TERM;
        td->next_sw  = NULL;
        last->link   = (uint32_t)td | UHCI_LINK_VF;
        last->next_sw = td;
        last = td;
        ptr += chunk; rem -= chunk; toggle ^= 1;
    }

    td = uhci_alloc_td(priv);
    if (!td) goto cleanup;
    {
        uint8_t spid = (pid == UHCI_TD_PID_IN) ? UHCI_TD_PID_OUT : UHCI_TD_PID_IN;
        td->ctrl_sts = UHCI_TD_ACTIVE | UHCI_TD_IOC | UHCI_TD_ERR_LIMIT(3)
                     | (ls ? UHCI_TD_LS : 0);
        td->token    = UHCI_TD_TOKEN(0, 1, 0, addr, spid);
        td->buf_ptr  = 0;
        td->link     = UHCI_LINK_TERM;
        td->next_sw  = NULL;
        last->link   = (uint32_t)td | UHCI_LINK_VF;
        last->next_sw = td;
        last = td;
    }

    priv->xfer_done  = 0;
    priv->xfer_error = 0;
    priv->async_qh->elem_link = (uint32_t)first | UHCI_LINK_VF;

    int rc = uhci_wait_done(priv, 200);

    priv->async_qh->elem_link = UHCI_LINK_TERM;

cleanup:
    for (uhci_td_t *t = first; t; ) {
        uhci_td_t *nx = t->next_sw;
        uhci_free_td(priv, t);
        t = nx;
    }
    return rc;
}

static int uhci_interrupt_transfer(usb_hc_t *hc, usb_device_t *dev,
                                    uint8_t ep_num, void *buf, uint16_t len)
{
    uhci_priv_t *priv = (uhci_priv_t *)hc->priv;
    uint8_t ls   = (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    uint8_t addr = dev->address;

    usb_endpoint_t *ep = NULL;
    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->ep[i].address == ep_num && dev->ep[i].direction == USB_DIR_IN) {
            ep = &dev->ep[i]; break;
        }
    }
    uint8_t  toggle  = ep ? ep->toggle : 0;
    uint16_t max_pkt = ep ? ep->max_packet : 8;
    if (len > max_pkt) len = max_pkt;

    uhci_td_t *td = uhci_alloc_td(priv);
    if (!td) return -1;

    td->ctrl_sts = UHCI_TD_ACTIVE | UHCI_TD_IOC | UHCI_TD_ERR_LIMIT(3)
                 | UHCI_TD_SPD | (ls ? UHCI_TD_LS : 0);
    td->token    = UHCI_TD_TOKEN(len, toggle, ep_num, addr, UHCI_TD_PID_IN);
    td->buf_ptr  = (uint32_t)buf;
    td->link     = UHCI_LINK_TERM;
    td->next_sw  = NULL;

    priv->xfer_done  = 0;
    priv->xfer_error = 0;
    priv->async_qh->elem_link = (uint32_t)td | UHCI_LINK_VF;

    int rc = uhci_wait_done(priv, 50);
    priv->async_qh->elem_link = UHCI_LINK_TERM;

    int actlen = -1;
    if (rc == 0) {
        if (ep) ep->toggle ^= 1;
        actlen = (int)((td->ctrl_sts & UHCI_TD_ACTLEN_MASK) + 1);
    }
    uhci_free_td(priv, td);
    return actlen;
}

static int uhci_bulk_transfer(usb_hc_t *hc, usb_device_t *dev,
                               uint8_t ep_num, uint8_t dir,
                               void *buf, uint16_t len)
{
    uhci_priv_t *priv = (uhci_priv_t *)hc->priv;
    uint8_t  ls   = (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    uint8_t  addr = dev->address;
    uint8_t  pid  = (dir == USB_DIR_IN) ? UHCI_TD_PID_IN : UHCI_TD_PID_OUT;
    uint16_t mps  = 64;

    usb_endpoint_t *ep = NULL;
    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->ep[i].address == ep_num && dev->ep[i].direction == dir) {
            ep = &dev->ep[i]; mps = ep->max_packet; break;
        }
    }

    uhci_td_t *first = NULL, *last = NULL;
    uint8_t  toggle = ep ? ep->toggle : 0;
    uint8_t *ptr    = (uint8_t *)buf;
    uint16_t rem    = len;

    while (rem > 0) {
        uint16_t   chunk = (rem > mps) ? mps : rem;
        uhci_td_t *td    = uhci_alloc_td(priv);
        if (!td) break;

        td->ctrl_sts = UHCI_TD_ACTIVE | UHCI_TD_ERR_LIMIT(3)
                     | (ls ? UHCI_TD_LS : 0)
                     | (rem <= mps ? UHCI_TD_IOC : 0);
        td->token    = UHCI_TD_TOKEN(chunk, toggle, ep_num, addr, pid);
        td->buf_ptr  = (uint32_t)ptr;
        td->link     = UHCI_LINK_TERM;
        td->next_sw  = NULL;

        if (last) { last->link = (uint32_t)td | UHCI_LINK_VF; last->next_sw = td; }
        else first = td;
        last = td;

        ptr += chunk; rem -= chunk; toggle ^= 1;
    }
    if (!first) return -1;

    priv->xfer_done  = 0;
    priv->xfer_error = 0;
    priv->async_qh->elem_link = (uint32_t)first | UHCI_LINK_VF;

    int rc = uhci_wait_done(priv, 500);
    priv->async_qh->elem_link = UHCI_LINK_TERM;

    if (rc == 0 && ep)
        ep->toggle = (last->token >> 19) & 1 ^ 1;

    for (uhci_td_t *t = first; t; ) {
        uhci_td_t *nx = t->next_sw; uhci_free_td(priv, t); t = nx;
    }
    return rc == 0 ? len : -1;
}

static int uhci_port_reset(usb_hc_t *hc, uint8_t port) {
    uhci_priv_t *priv = (uhci_priv_t *)hc->priv;
    uint16_t     base = priv->io_base;
    uint8_t      reg  = (port == 0) ? UHCI_PORTSC0 : UHCI_PORTSC1;

    if (!(uhci_inw(base, reg) & UHCI_PORT_CCS)) return -1;

    uhci_outw(base, reg, UHCI_PORT_RESET);
    uhci_udelay(60000);   

    uhci_outw(base, reg, 0);
    uhci_udelay(10000);   

    uint16_t s = uhci_inw(base, reg);
    uhci_outw(base, reg, s | UHCI_PORT_CSC | UHCI_PORT_PEDC);
    uhci_udelay(1000);

    uhci_outw(base, reg, UHCI_PORT_PED);
    uhci_udelay(10000);

    for (int i = 0; i < 20; i++) {
        s = uhci_inw(base, reg);
        if (s & (UHCI_PORT_CSC | UHCI_PORT_PEDC))
            uhci_outw(base, reg, s | UHCI_PORT_CSC | UHCI_PORT_PEDC);
        if (!(s & UHCI_PORT_CCS)) return -1;  
        if (s & UHCI_PORT_PED)    return 0;   
        uhci_udelay(5000);
    }
    return -1;
}

static int uhci_port_get_status(usb_hc_t *hc, uint8_t port) {
    uhci_priv_t *priv = (uhci_priv_t *)hc->priv;
    uint8_t reg = (port == 0) ? UHCI_PORTSC0 : UHCI_PORTSC1;
    return (int)uhci_inw(priv->io_base, reg);
}

int uhci_register_interrupt_td(usb_hc_t *hc, usb_device_t *dev,
                                 uint8_t ep_num, void *buf, uint16_t len,
                                 usb_irq_notify_fn_t notify, void *notify_priv)
{
    uhci_priv_t *priv = (uhci_priv_t *)hc->priv;
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

    if (priv->intr_td_count >= UHCI_MAX_INTR_TD) return -1;
    uhci_intr_td_slot_t *slot = &priv->intr_slots[priv->intr_td_count++];

    slot->td = uhci_alloc_td(priv);
    if (!slot->td) { priv->intr_td_count--; return -1; }

    slot->td->ctrl_sts = UHCI_TD_ACTIVE | UHCI_TD_IOC
                       | UHCI_TD_ERR_LIMIT(3)
                       | UHCI_TD_SPD
                       | (ls ? UHCI_TD_LS : 0);
    slot->td->token    = UHCI_TD_TOKEN(len, ep ? ep->toggle : 0,
                                       ep_num, addr, UHCI_TD_PID_IN);
    slot->td->buf_ptr  = (uint32_t)buf;
    slot->td->link     = UHCI_LINK_TERM;
    slot->td->next_sw  = NULL;

    slot->buf        = buf;
    slot->len        = len;
    slot->dev        = dev;
    slot->ep_num     = ep_num;
    slot->notify     = notify;
    slot->notify_priv= notify_priv;
    slot->active     = 1;

    uint32_t qh_link = (uint32_t)priv->async_qh | UHCI_LINK_QH;
    slot->td->link = qh_link;

    uint8_t interval = ep ? ep->interval : 8;
    if (!interval) interval = 1;

    for (int f = 0; f < UHCI_FRAME_COUNT; f += interval)
        priv->frame_list[f] = (uint32_t)slot->td | UHCI_LINK_VF;

    return 0;
}

static void uhci_handle_irq(usb_hc_t *hc) {
    uhci_priv_t *priv = (uhci_priv_t *)hc->priv;
    uint16_t     base = priv->io_base;

    uint16_t sts = uhci_inw(base, UHCI_USBSTS);
    if (!sts) return;

    uhci_outw(base, UHCI_USBSTS, sts);

    if (sts & (UHCI_STS_USBINT | UHCI_STS_ERROR)) {
        priv->xfer_error = (sts & UHCI_STS_ERROR) ? 1 : 0;
        priv->xfer_done  = 1;
    }

    for (int i = 0; i < priv->intr_td_count; i++) {
        uhci_intr_td_slot_t *slot = &priv->intr_slots[i];
        if (!slot->active) continue;

        uhci_td_t *td = slot->td;
        if (td->ctrl_sts & UHCI_TD_ACTIVE) continue;   

        uint32_t cs = td->ctrl_sts;
        int error   = cs & (UHCI_TD_STALLED | UHCI_TD_BABBLE |
                            UHCI_TD_DBUFERR | UHCI_TD_CRC_TO);

        if (!error) {
            uint16_t actlen = (uint16_t)((cs & UHCI_TD_ACTLEN_MASK) + 1);
            if (slot->notify)
                slot->notify(slot->dev, slot->buf, actlen, slot->notify_priv);

            usb_endpoint_t *ep = NULL;
            for (int j = 0; j < slot->dev->ep_count; j++) {
                if (slot->dev->ep[j].address == slot->ep_num &&
                    slot->dev->ep[j].direction == USB_DIR_IN) {
                    ep = &slot->dev->ep[j]; break;
                }
            }
            uint8_t new_toggle = ep ? (ep->toggle ^= 1) : 0;

            td->token = (td->token & ~(1u << 19)) | ((uint32_t)new_toggle << 19);
            td->ctrl_sts = UHCI_TD_ACTIVE | UHCI_TD_IOC
                         | UHCI_TD_ERR_LIMIT(3) | UHCI_TD_SPD
                         | (cs & UHCI_TD_LS);
        } else {
            td->ctrl_sts = UHCI_TD_ACTIVE | UHCI_TD_IOC
                         | UHCI_TD_ERR_LIMIT(3) | UHCI_TD_SPD
                         | (cs & UHCI_TD_LS);
        }
    }

    for (uint8_t p = 0; p < 2; p++) {
        uint8_t  reg = (p == 0) ? UHCI_PORTSC0 : UHCI_PORTSC1;
        uint16_t s   = uhci_inw(base, reg);

        if (!(s & UHCI_PORT_CSC)) continue;

        uhci_outw(base, reg, s | UHCI_PORT_CSC);

        if (s & UHCI_PORT_CCS) {
            uint8_t speed = (s & UHCI_PORT_LSDA) ? USB_SPEED_LOW
                                                  : USB_SPEED_FULL;
            usb_device_enumerate(hc, p, speed);
        } else {
            usb_device_disconnect(hc, p);
        }
    }

    if (sts & UHCI_STS_HSE)  kprint("[UHCI] Host System Error\n");
    if (sts & UHCI_STS_HCPE) kprint("[UHCI] HC Process Error\n");
}

void uhci_irq_handler(void) {
    for (usb_hc_t *hc = uhci_hc_list; hc; hc = hc->irq_next)
        uhci_handle_irq(hc);
}

static int uhci_init_one(uint16_t io_base) {
    uhci_priv_t *priv = (uhci_priv_t *)kmalloc(sizeof(uhci_priv_t));
    if (!priv) { kprint("[UHCI] kmalloc priv failed\n"); return -1; }
    memset(priv, 0, sizeof(uhci_priv_t));
    priv->io_base = io_base;

    uint8_t *fl_raw = (uint8_t *)kmalloc(UHCI_FRAME_COUNT * 4 + 4096);
    if (!fl_raw) { kfree_heap(priv); return -1; }
    priv->frame_list = (uint32_t *)(((uint32_t)fl_raw + 0xFFF) & ~0xFFF);

    uint8_t *td_raw = (uint8_t *)kmalloc(sizeof(uhci_td_t) * UHCI_TD_POOL_SIZE + 16);
    uint8_t *qh_raw = (uint8_t *)kmalloc(sizeof(uhci_qh_t) * UHCI_QH_POOL_SIZE + 16);
    if (!td_raw || !qh_raw) { kfree_heap(priv); return -1; }
    priv->td_pool = (uhci_td_t *)(((uint32_t)td_raw + 15) & ~15);
    priv->qh_pool = (uhci_qh_t *)(((uint32_t)qh_raw + 15) & ~15);

    uhci_outw(io_base, UHCI_USBCMD, UHCI_CMD_HCRESET);
    uhci_udelay(50000);
    uhci_outw(io_base, UHCI_USBCMD, 0);

    priv->async_qh = uhci_alloc_qh(priv);
    priv->async_qh->head_link = UHCI_LINK_TERM;
    priv->async_qh->elem_link = UHCI_LINK_TERM;

    uint32_t qh_ptr = (uint32_t)priv->async_qh | UHCI_LINK_QH;
    for (int i = 0; i < UHCI_FRAME_COUNT; i++)
        priv->frame_list[i] = qh_ptr;

    uhci_outw(io_base, UHCI_USBSTS,  0x3F);
    uhci_outw(io_base, UHCI_USBINTR, 0x0F);    
    uhci_outw(io_base, UHCI_FRNUM,   0);
    uhci_outl(io_base, UHCI_FRBASEADD, (uint32_t)priv->frame_list);
    port_byte_out(io_base + UHCI_SOFMOD, 0x40);
    uhci_outw(io_base, UHCI_USBCMD,
              UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);

    usb_hc_t *hc = (usb_hc_t *)kmalloc(sizeof(usb_hc_t));
    if (!hc) { kfree_heap(priv); return -1; }
    memset(hc, 0, sizeof(usb_hc_t));

    hc->name               = "UHCI";
    hc->control_transfer   = uhci_control_transfer;
    hc->interrupt_transfer = uhci_interrupt_transfer;
    hc->bulk_transfer      = uhci_bulk_transfer;
    hc->port_reset         = uhci_port_reset;
    hc->port_get_status    = uhci_port_get_status;
    hc->num_ports          = 2;
    hc->priv               = priv;

    hc->irq_next  = uhci_hc_list;
    uhci_hc_list  = hc;

    usb_hc_register(hc);

    for (uint8_t p = 0; p < 2; p++) {
        uint8_t  reg = (p == 0) ? UHCI_PORTSC0 : UHCI_PORTSC1;
        uint16_t s   = uhci_inw(io_base, reg);
        if (s & UHCI_PORT_CCS) {
            uint8_t speed = (s & UHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
            usb_device_enumerate(hc, p, speed);
        }
    }

    return 0;
}

static int uhci_pci_probe(pci_device_t *pdev) {
    if (pdev->prog_if != 0x00) return -1;

    uint16_t io_base = 0;
    for (int i = 0; i < 6; i++) {
        if (pdev->bars[i].is_io && pdev->bars[i].base) {
            io_base = (uint16_t)pdev->bars[i].base; break;
        }
    }
    if (!io_base) {
        uint32_t bar4 = pci_read32(pdev->bus, pdev->dev, pdev->fn, 0x20);
        io_base = (uint16_t)(bar4 & 0xFFFC);
    }
    if (!io_base) { kprint("[UHCI] No IO BAR\n"); return -1; }

    uint32_t cmd = pci_read32(pdev->bus, pdev->dev, pdev->fn, 0x04);
    cmd |= 0x05;
    cmd &= ~(1 << 10);
    pci_write32(pdev->bus, pdev->dev, pdev->fn, 0x04, cmd);

    uint8_t cap_ptr = (uint8_t)(pci_read32(pdev->bus, pdev->dev, pdev->fn, 0x34) & 0xFF);
    while (cap_ptr && cap_ptr != 0xFF) {
        uint32_t cap    = pci_read32(pdev->bus, pdev->dev, pdev->fn, cap_ptr);
        uint8_t  cap_id = (uint8_t)(cap & 0xFF);
        if (cap_id == 0x05) {
            uint32_t msi_ctrl = pci_read32(pdev->bus, pdev->dev, pdev->fn, cap_ptr + 2);
            msi_ctrl &= ~(1 << 16);
            pci_write32(pdev->bus, pdev->dev, pdev->fn, cap_ptr + 2, msi_ctrl);
            break;
        }
        cap_ptr = (uint8_t)((cap >> 8) & 0xFF);
    }

    return uhci_init_one(io_base);
}

static pci_driver_t uhci_pci_driver = {
    .name       = "uhci_hcd",
    .vendor_id  = PCI_ANY_ID,
    .device_id  = PCI_ANY_ID,
    .class_code = 0x0C,
    .subclass   = 0x03,
    .probe      = uhci_pci_probe,
};

void uhci_pci_init(void) {
    pci_register_driver(&uhci_pci_driver);

    pci_device_t *d = pci_find_by_class(0x0C, 0x03);
    while (d) {
        if (d->prog_if == 0x00) uhci_pci_probe(d);
        d = d->next;
        while (d && !(d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x00))
            d = d->next;
    }
}