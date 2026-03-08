#ifndef USB_OHCI_H
#define USB_OHCI_H

#include <stdint.h>
#include "usb.h"
#include "pci_enum.h"


#define OHCI_REVISION        0x00
#define OHCI_CONTROL         0x04
#define OHCI_CMDSTATUS       0x08
#define OHCI_INTSTATUS       0x0C
#define OHCI_INTENABLE       0x10
#define OHCI_INTDISABLE      0x14
#define OHCI_HCCA            0x18
#define OHCI_PERIOD_CED      0x1C
#define OHCI_CTRL_HEAD_ED    0x20
#define OHCI_CTRL_CUR_ED     0x24
#define OHCI_BULK_HEAD_ED    0x28
#define OHCI_BULK_CUR_ED     0x2C
#define OHCI_DONE_HEAD       0x30
#define OHCI_FM_INTERVAL     0x34
#define OHCI_FM_REMAINING    0x38
#define OHCI_FM_NUMBER       0x3C
#define OHCI_PERIODIC_START  0x40
#define OHCI_LS_THRESHOLD    0x44
#define OHCI_RH_DESCRIPTOR_A 0x48
#define OHCI_RH_DESCRIPTOR_B 0x4C
#define OHCI_RH_STATUS       0x50
#define OHCI_RH_PORT_STATUS  0x54


#define OHCI_CTRL_CBSR   0x00000003
#define OHCI_CTRL_PLE    0x00000004
#define OHCI_CTRL_IE     0x00000008
#define OHCI_CTRL_CLE    0x00000010
#define OHCI_CTRL_BLE    0x00000020
#define OHCI_CTRL_HCFS   0x000000C0
#define OHCI_CTRL_IR     0x00000100
#define OHCI_HCFS_RESET       0x00000000
#define OHCI_HCFS_RESUME      0x00000040
#define OHCI_HCFS_OPERATIONAL 0x00000080
#define OHCI_HCFS_SUSPEND     0x000000C0


#define OHCI_CMD_HCR  0x00000001
#define OHCI_CMD_CLF  0x00000002
#define OHCI_CMD_BLF  0x00000004
#define OHCI_CMD_OCR  0x00000008


#define OHCI_INT_SO    0x00000001
#define OHCI_INT_WDH   0x00000002
#define OHCI_INT_SF    0x00000004
#define OHCI_INT_RD    0x00000008
#define OHCI_INT_UE    0x00000010
#define OHCI_INT_FNO   0x00000020
#define OHCI_INT_RHSC  0x00000040
#define OHCI_INT_OC    0x40000000
#define OHCI_INT_MIE   0x80000000


#define OHCI_PORT_CCS   0x00000001
#define OHCI_PORT_PES   0x00000002
#define OHCI_PORT_PSS   0x00000004
#define OHCI_PORT_PRS   0x00000010
#define OHCI_PORT_PPS   0x00000100
#define OHCI_PORT_LSDA  0x00000200
#define OHCI_PORT_CSC   0x00010000
#define OHCI_PORT_PESC  0x00020000
#define OHCI_PORT_PRSC  0x00100000

typedef struct ohci_ed {
    uint32_t ctrl;
    uint32_t tail_td;
    uint32_t head_td;
    uint32_t next_ed;
} __attribute__((packed, aligned(16))) ohci_ed_t;

#define OHCI_ED_ADDR(a)    ((a) & 0x7F)
#define OHCI_ED_EN(e)      (((e) & 0xF) << 7)
#define OHCI_ED_DIR_TD     (0x00 << 11)
#define OHCI_ED_DIR_OUT    (0x01 << 11)
#define OHCI_ED_DIR_IN     (0x02 << 11)
#define OHCI_ED_SPEED_LS   (1 << 13)
#define OHCI_ED_SKIP       (1 << 14)
#define OHCI_ED_MAXPKT(m)  (((m) & 0x7FF) << 16)
#define OHCI_ED_HEAD_HALT  0x00000001
#define OHCI_ED_HEAD_CARRY 0x00000002

typedef struct ohci_td {
    uint32_t ctrl;
    uint32_t cbp;
    uint32_t next_td;
    uint32_t be;
    struct ohci_td *next_sw;
    uint32_t        pad[3];
} __attribute__((packed, aligned(16))) ohci_td_t;

#define OHCI_TD_ROUNDING      (1 << 18)
#define OHCI_TD_DP_SETUP      (0x00 << 19)
#define OHCI_TD_DP_OUT        (0x01 << 19)
#define OHCI_TD_DP_IN         (0x02 << 19)
#define OHCI_TD_DI(n)         (((n) & 7) << 21)
#define OHCI_TD_DI_NONE       OHCI_TD_DI(7)
#define OHCI_TD_TOGGLE_DATA0  (0x02 << 24)
#define OHCI_TD_TOGGLE_DATA1  (0x03 << 24)
#define OHCI_TD_TOGGLE_CARRY  (0x00 << 24)
#define OHCI_TD_CC_MASK       (0x0F << 28)
#define OHCI_TD_CC_NOERR      0x00000000
#define OHCI_TD_CC_NOTACC     0xF0000000

typedef struct ohci_hcca {
    uint32_t interrupt_table[32];
    uint16_t frame_no;
    uint16_t pad1;
    uint32_t done_head;
    uint8_t  reserved[116];
} __attribute__((packed, aligned(256))) ohci_hcca_t;

#define OHCI_MAX_INTR_ED  8

struct usb_device;
typedef void (*usb_irq_notify_fn_t)(struct usb_device *dev,
                                     void *buf, uint16_t len,
                                     void *priv);

typedef struct {
    ohci_ed_t           *ed;
    ohci_td_t           *td;        
    ohci_td_t           *td_tail;  
    void                *buf;
    uint16_t             len;
    struct usb_device   *dev;
    uint8_t              ep_num;
    uint8_t              active;
    usb_irq_notify_fn_t  notify;
    void                *notify_priv;
} ohci_intr_ed_slot_t;

#define OHCI_TD_POOL_SIZE 128
#define OHCI_ED_POOL_SIZE  32

typedef struct {
    volatile uint32_t *regs;
    ohci_hcca_t       *hcca;
    ohci_ed_t         *ed_pool;
    ohci_td_t         *td_pool;
    uint32_t           ed_alloc_mask;
    uint32_t           td_alloc_mask[OHCI_TD_POOL_SIZE / 32];
    uint8_t            num_ports;
    ohci_ed_t         *ctrl_ed;

    volatile uint8_t   xfer_done;  
    volatile uint8_t   xfer_error;

    ohci_intr_ed_slot_t intr_slots[OHCI_MAX_INTR_ED];
    int                 intr_ed_count;
} ohci_priv_t;


//Public api
void ohci_pci_init(void);
void ohci_irq_handler(void);   

int  ohci_register_interrupt_ed(struct usb_hc *hc, struct usb_device *dev,
                                  uint8_t ep_num, void *buf, uint16_t len,
                                  usb_irq_notify_fn_t notify, void *notify_priv);

#endif