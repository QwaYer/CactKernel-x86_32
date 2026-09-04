#ifndef USB_XHCI_INTERNAL_H
#define USB_XHCI_INTERNAL_H

#include "xhci.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "sync.h"
#include "msi.h"

extern uint32_t page_directory[1024];

extern usb_hc_t *xhci_hc_list;
extern irq_spinlock_t xhci_evt_lock;

/* MMIO access helpers — shared by all xHCI modules. */

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

/* Ring / command / event core (xhci_ring.c). */
void xhci_ring_init(xhci_ring_t *ring, xhci_trb_t *mem, uint32_t size);
void xhci_ring_enqueue(xhci_ring_t *ring, xhci_trb_t *trb);
int  xhci_send_cmd(xhci_priv_t *priv, xhci_trb_t *trb);
int  xhci_wait_cmd(xhci_priv_t *priv, uint32_t timeout_ms);
void xhci_handle_irq(usb_hc_t *hc);

/* Device/context management (xhci_dev.c). */
int  xhci_enable_slot(xhci_priv_t *priv, uint8_t *slot_id);
uint8_t *xhci_get_dev_ctx(xhci_priv_t *priv, uint8_t slot);
uint8_t *xhci_get_input_ctx(xhci_priv_t *priv);
void xhci_setup_ep_ring(xhci_priv_t *priv, uint8_t slot, uint8_t dci);
int  xhci_address_device(xhci_priv_t *priv, uint8_t slot, uint8_t port,
                         uint8_t speed, int bsr);
int  xhci_configure_endpoint(xhci_priv_t *priv, uint8_t slot, uint8_t dci,
                             uint8_t ep_type, uint16_t mps, uint8_t interval);
uint8_t xhci_port_speed_to_usb(uint32_t portsc);
int  xhci_port_reset(usb_hc_t *hc, uint8_t port);
int  xhci_port_get_status(usb_hc_t *hc, uint8_t port);
void xhci_device_removed(usb_hc_t *hc, usb_device_t *dev);

/* Transfers (xhci_xfer.c). */
int  xhci_control_transfer(usb_hc_t *hc, usb_device_t *dev,
                           usb_setup_pkt_t *setup, void *data, uint16_t len);
int  xhci_interrupt_transfer(usb_hc_t *hc, usb_device_t *dev,
                             uint8_t ep_num, void *buf, uint16_t len);
int  xhci_bulk_transfer(usb_hc_t *hc, usb_device_t *dev,
                        uint8_t ep_num, uint8_t dir, void *buf, uint16_t len);

/* Host bring-up (xhci_hw.c). */
int  xhci_init_one(uint32_t phys_base, uint32_t quirks);

#endif
