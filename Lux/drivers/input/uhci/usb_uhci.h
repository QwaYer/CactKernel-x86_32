#ifndef USB_UHCI_H
#define USB_UHCI_H

#include <stdint.h>
#include "usb.h"
#include "pci_enum.h"


#define UHCI_USBCMD     0x00
#define UHCI_USBSTS     0x02
#define UHCI_USBINTR    0x04
#define UHCI_FRNUM      0x06
#define UHCI_FRBASEADD  0x08
#define UHCI_SOFMOD     0x0C
#define UHCI_PORTSC0    0x10
#define UHCI_PORTSC1    0x12


#define UHCI_CMD_RS      0x0001
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_GRESET  0x0004
#define UHCI_CMD_CF      0x0040
#define UHCI_CMD_MAXP    0x0080


#define UHCI_STS_USBINT  0x0001
#define UHCI_STS_ERROR   0x0002
#define UHCI_STS_RD      0x0004
#define UHCI_STS_HSE     0x0008
#define UHCI_STS_HCPE    0x0010
#define UHCI_STS_HCH     0x0020


#define UHCI_PORT_CCS    0x0001
#define UHCI_PORT_CSC    0x0002
#define UHCI_PORT_PED    0x0004
#define UHCI_PORT_PEDC   0x0008
#define UHCI_PORT_LSDA   0x0100
#define UHCI_PORT_RESET  0x0200


typedef struct uhci_td {
    uint32_t link;
    uint32_t ctrl_sts;
    uint32_t token;
    uint32_t buf_ptr;
    struct uhci_td *next_sw;
    uint32_t        pad[3];
} __attribute__((packed, aligned(16))) uhci_td_t;

#define UHCI_TD_ACTLEN_MASK  0x000007FF
#define UHCI_TD_CRC_TO       (1 << 18)
#define UHCI_TD_BABBLE       (1 << 20)
#define UHCI_TD_DBUFERR      (1 << 21)
#define UHCI_TD_STALLED      (1 << 22)
#define UHCI_TD_ACTIVE       (1 << 23)
#define UHCI_TD_IOC          (1 << 24)
#define UHCI_TD_LS           (1 << 26)
#define UHCI_TD_ERR_LIMIT(n) ((n) << 27)
#define UHCI_TD_SPD          (1 << 29)

#define UHCI_TD_PID_SETUP    0x2D
#define UHCI_TD_PID_IN       0x69
#define UHCI_TD_PID_OUT      0xE1

#define UHCI_TD_TOKEN(maxlen, toggle, ep, addr, pid) \
    (((uint32_t)((maxlen)-1) & 0x7FF) << 21 | \
     ((uint32_t)(toggle) << 19) | \
     ((uint32_t)(ep)     << 15) | \
     ((uint32_t)(addr)   <<  8) | \
     (uint32_t)(pid))

#define UHCI_LINK_TERM  0x00000001
#define UHCI_LINK_QH    0x00000002
#define UHCI_LINK_VF    0x00000004


typedef struct uhci_qh {
    uint32_t        head_link;
    uint32_t        elem_link;
    struct uhci_qh *next_sw;
    uint32_t        pad;
} __attribute__((packed, aligned(16))) uhci_qh_t;

#define UHCI_FRAME_COUNT  1024
#define UHCI_MAX_INTR_TD  8
#define UHCI_TD_POOL_SIZE 256
#define UHCI_QH_POOL_SIZE  64


struct usb_device;
typedef void (*usb_irq_notify_fn_t)(struct usb_device *dev,
                                     void *buf, uint16_t len,
                                     void *priv);

typedef struct {
    uhci_td_t           *td;
    void                *buf;
    uint16_t             len;
    struct usb_device   *dev;
    uint8_t              ep_num;
    uint8_t              active;
    usb_irq_notify_fn_t  notify;
    void                *notify_priv;
} uhci_intr_td_slot_t;


typedef struct {
    uint16_t   io_base;
    uint32_t  *frame_list;
    uhci_qh_t *qh_pool;
    uhci_td_t *td_pool;
    uint32_t   qh_alloc_mask;
    uint32_t   td_alloc_mask[UHCI_TD_POOL_SIZE / 32];
    uhci_qh_t *async_qh;

    volatile uint8_t xfer_done;   
    volatile uint8_t xfer_error;  

    uhci_intr_td_slot_t intr_slots[UHCI_MAX_INTR_TD];
    int                 intr_td_count;
} uhci_priv_t;

//Public api
void uhci_pci_init(void);
void uhci_irq_handler(void);   

int  uhci_register_interrupt_td(struct usb_hc *hc, struct usb_device *dev,
                                  uint8_t ep_num, void *buf, uint16_t len,
                                  usb_irq_notify_fn_t notify, void *notify_priv);

#endif