#ifndef USB_XHCI_H
#define USB_XHCI_H

#include <stdint.h>
#include "usb.h"
#include "pci_enum.h"
#include "sync.h"
#include "xhci_quirks.h"

#define XHCI_CAP_CAPLENGTH   0x00
#define XHCI_CAP_HCIVERSION  0x02
#define XHCI_CAP_HCSPARAMS1  0x04
#define XHCI_CAP_HCSPARAMS2  0x08
#define XHCI_CAP_HCSPARAMS3  0x0C
#define XHCI_CAP_HCCPARAMS1  0x10
#define XHCI_CAP_DBOFF       0x14
#define XHCI_CAP_RTSOFF      0x18

#define XHCI_OP_USBCMD       0x00
#define XHCI_OP_USBSTS       0x04
#define XHCI_OP_PAGESIZE     0x08
#define XHCI_OP_DNCTRL       0x14
#define XHCI_OP_CRCR         0x18
#define XHCI_OP_DCBAAP       0x30
#define XHCI_OP_CONFIG       0x38
#define XHCI_OP_PORTSC_BASE  0x400

#define XHCI_CMD_RS     (1u << 0)
#define XHCI_CMD_HCRST  (1u << 1)
#define XHCI_CMD_INTE   (1u << 2)
#define XHCI_CMD_HSEE   (1u << 3)

#define XHCI_STS_HCH   (1u << 0)
#define XHCI_STS_HSE   (1u << 2)
#define XHCI_STS_EINT  (1u << 3)
#define XHCI_STS_PCD   (1u << 4)
#define XHCI_STS_CNR   (1u << 11)

#define XHCI_PORTSC_CCS  (1u << 0)
#define XHCI_PORTSC_PED  (1u << 1)
#define XHCI_PORTSC_OCA  (1u << 3)
#define XHCI_PORTSC_PR   (1u << 4)
#define XHCI_PORTSC_PP   (1u << 9)
#define XHCI_PORTSC_LWS  (1u << 16)   /* Link State Write Strobe (needed for any PLS write) */
#define XHCI_PORTSC_CSC  (1u << 17)
#define XHCI_PORTSC_PEC  (1u << 18)
#define XHCI_PORTSC_WRC  (1u << 19)   /* warm reset complete */
#define XHCI_PORTSC_OCC  (1u << 20)
#define XHCI_PORTSC_PRC  (1u << 21)
#define XHCI_PORTSC_PLC  (1u << 22)   /* port link state change */
#define XHCI_PORTSC_CEC  (1u << 23)   /* port config error change */
#define XHCI_PORTSC_CAS  (1u << 24)   /* Cold Attach Status: warm reset required */
#define XHCI_PORTSC_WR   (1u << 31)   /* initiate a warm port reset */
#define XHCI_PORTSC_SPEED_MASK (0xFu << 10)
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_PLS_MASK   (0xFu << 5)
#define XHCI_PORT_SPEED_FS  1
#define XHCI_PORT_SPEED_LS  2
#define XHCI_PORT_SPEED_HS  3
#define XHCI_PORT_SPEED_SS  4
#define XHCI_PORT_SPEED_SSP 5

/* Port Link State field values (bits 8:5). */
#define XHCI_PORT_PLS_U0         0x0
#define XHCI_PORT_PLS_U3         0x3
#define XHCI_PORT_PLS_DISABLED   0x4
#define XHCI_PORT_PLS_RXDETECT   0x5
#define XHCI_PORT_PLS_INACTIVE   0x6
#define XHCI_PORT_PLS_POLLING    0x7
#define XHCI_PORT_PLS_COMPLIANCE 0xA

#define XHCI_IMAN          0x00
#define XHCI_IMOD          0x04
#define XHCI_ERSTSZ        0x08
#define XHCI_ERSTBA        0x10
#define XHCI_ERDP          0x18

#define XHCI_TRB_NORMAL         1
#define XHCI_TRB_SETUP          2
#define XHCI_TRB_DATA           3
#define XHCI_TRB_STATUS         4
#define XHCI_TRB_LINK           6
#define XHCI_TRB_ENABLE_SLOT    9
#define XHCI_TRB_DISABLE_SLOT   10
#define XHCI_TRB_ADDRESS_DEV    11
#define XHCI_TRB_CONFIG_EP      12
#define XHCI_TRB_EVAL_CTX       13
#define XHCI_TRB_RESET_EP       14
#define XHCI_TRB_STOP_EP        15
#define XHCI_TRB_SET_TR_DEQ     16
#define XHCI_TRB_NOOP_CMD       23
#define XHCI_TRB_TRANSFER_EVT   32
#define XHCI_TRB_CMD_COMPLETE   33
#define XHCI_TRB_PORT_STATUS    34
#define XHCI_TRB_HOST_CTRL      37

#define XHCI_TRB_TYPE_SHIFT     10
#define XHCI_TRB_TYPE_MASK      (0x3Fu << 10)
#define XHCI_TRB_CYCLE          (1u << 0)
#define XHCI_TRB_IOC            (1u << 5)
#define XHCI_TRB_IDT            (1u << 6)
#define XHCI_TRB_DIR_IN         (1u << 16)
#define XHCI_TRB_BSR            (1u << 9)

#define XHCI_CC_SUCCESS         1
#define XHCI_CC_SHORT_PACKET    13

#define XHCI_MAX_SLOTS          32
#define XHCI_MAX_PORTS          16
#define XHCI_CMD_RING_SIZE      64
#define XHCI_EVT_RING_SIZE      64
#define XHCI_EP_RING_SIZE       64
#define XHCI_MAX_INTR_EP        8
#define XHCI_ERST_SIZE          1

#define XHCI_EP_CTX_TYPE_CTRL_BI    4
#define XHCI_EP_CTX_TYPE_INTR_IN    7
#define XHCI_EP_CTX_TYPE_BULK_OUT   2
#define XHCI_EP_CTX_TYPE_BULK_IN    6

typedef struct {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} __attribute__((packed, aligned(16))) xhci_trb_t;

typedef struct {
    uint32_t seg_addr_lo;
    uint32_t seg_addr_hi;
    uint32_t seg_size;
    uint32_t rsvd;
} __attribute__((packed)) xhci_erst_entry_t;

typedef struct {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t rsvd[5];
    uint32_t cfg_info;
} __attribute__((packed)) xhci_input_ctrl_ctx_t;

typedef struct {
    uint32_t ctx[8];
} __attribute__((packed)) xhci_slot_ctx_t;

typedef struct {
    uint32_t ctx[8];
} __attribute__((packed)) xhci_ep_ctx_t;

typedef struct {
    xhci_trb_t           *ring;
    uint32_t              enqueue;
    /* Next TRB the controller will consume.  Advanced past a TD when its
     * transfer event arrives, so it is the value a Set TR Dequeue Pointer (or
     * a re-issued Configure Endpoint) must carry. */
    uint32_t              dequeue;
    uint32_t              cycle;
    uint32_t              size;
    /* Completion is per endpoint, not one flag for the whole controller.  The
     * module drives its registers with control transfers, and with a single
     * shared flag those waits swallowed a bulk-IN completion — after which the
     * endpoint still looked armed and RX stayed dead for the rest of the scan. */
    volatile uint8_t      done;
    volatile uint8_t      err;
    /* Completion code and "Transfer Residual" (bytes not moved) of the event
     * that completed the last TD.  A short bulk-IN leaves residual > 0; the
     * caller needs the real byte count, not the requested length. */
    volatile uint8_t      last_cc;
    volatile uint32_t     last_residual;
    /* At most one transfer in flight per endpoint.  A bulk-IN that times out
     * keeps its TRB armed (the frame may still land in the caller's buffer), so
     * enqueuing a second transfer for the same endpoint would leave the chip
     * free to rewrite that buffer while the caller is reading it.  A retry with
     * the same buffer waits on the armed transfer instead of adding a TRB. */
    void                 *xfer_buf;
    uint16_t              xfer_len;
    uint8_t               xfer_armed;
} xhci_ring_t;

typedef struct {
    xhci_trb_t           *td;
    void                 *buf;
    uint16_t              len;
    struct usb_device    *dev;
    uint8_t               ep_num;
    uint8_t               active;
    /* A completion that is not Success/Short Packet means the report in the
     * buffer is unusable, but the endpoint still needs its next TRB.  Report
     * the first bad code per slot and stay quiet after that: a device that is
     * really broken would otherwise print on every poll interval. */
    uint8_t               err_logged;
    usb_irq_notify_fn_t   notify;
    void                 *notify_priv;
    uint8_t               slot_id;
    uint8_t               dci;
    /* Points at the slot's real ring in xhci_priv_t::ep_rings.  It used to be
     * a by-value copy, so the interrupt path advanced one set of enqueue/
     * cycle counters while Configure Endpoint read another — the two drifted
     * apart and the endpoint eventually fetched a stale TRB. */
    xhci_ring_t          *ring;
} xhci_intr_ep_slot_t;

typedef struct {
    volatile uint32_t    *cap;
    volatile uint32_t    *op;
    volatile uint32_t    *rt;
    volatile uint32_t    *db;
    uint32_t              cap_phys;
    uint32_t              op_off;
    uint32_t              rt_off;
    uint32_t              db_off;

    uint8_t               max_slots;
    uint8_t               max_ports;
    uint16_t              max_intrs;

    uint64_t             *dcbaa;
    uint8_t              *dev_ctx_pool;
    uint8_t              *input_ctx_pool;
    uint64_t             *scratchpad_array;
    uint8_t              *scratchpad_pool;

    xhci_ring_t           cmd_ring;
    xhci_trb_t           *evt_ring;
    uint32_t              evt_dequeue;
    uint32_t              evt_cycle;
    xhci_erst_entry_t    *erst;

    volatile uint8_t      cmd_done;
    volatile uint8_t      cmd_error;
    /* Completion code of the last Command Completion Event (kept so a failed
     * Address Device / Enable Slot can report *why* it failed). */
    volatile uint8_t      cmd_cc;
    volatile uint32_t     cmd_result;

    uint8_t               slot_used[XHCI_MAX_SLOTS + 1];
    uint8_t               slot_port[XHCI_MAX_SLOTS + 1];
    xhci_ring_t           ep_rings[XHCI_MAX_SLOTS + 1][31];

    /* Root-port devices, indexed by port.  Kept apart from slot_dev/slot_port
     * (which are the controller-wide view) so a hotplug event on a root port
     * can never be confused with a device sitting on a hub's downstream port. */
    usb_device_t         *port_dev[XHCI_MAX_PORTS];

    /* Ports with an unhandled status change.  A Port Status Change Event (or
     * the PCD latch) cannot be serviced in interrupt context — a reset and the
     * enumeration that follows sleep and issue commands — so the IRQ path only
     * records the port here and a task does the work. */
    volatile uint32_t     port_events;
    uint8_t               hotplug_started;
    /* Set while xhci_scan_ports() is enumerating.  A port-status change raised
     * by the scan's own resets would otherwise make the hotplug task probe the
     * same port concurrently, and a second probe of an already-addressed device
     * (no port reset) cannot succeed. */
    volatile uint8_t      scanning;
    /* Set by the IRQ path after a fatal Host System Error put the controller
     * back into reset.  Re-programming the registers and re-enumerating has to
     * happen in task context, so the hotplug task owns it. */
    volatile uint8_t      reset_pending;

    /* Slots whose device went away.  Torn down by the hotplug task, not by the
     * disconnect path: that path can run with the event lock held (a hub's
     * interrupt report is processed inside xhci_drain_events), and Disable Slot
     * is a command that would deadlock on the same lock. */
    volatile uint32_t     slot_reap;

    /* Value last written to CRCR, so a Command Abort can be issued without
     * relying on CRCR read-back (broken on some controllers). */
    uint32_t              cmd_ring_phys;

    xhci_intr_ep_slot_t   intr_slots[XHCI_MAX_INTR_EP];
    int                   intr_ep_count;

    spinlock_t            ctx_lock;

    uint32_t              context_size;
    uint32_t              quirks;
} xhci_priv_t;

void xhci_pci_init(void);
void xhci_irq_handler(void);

int  xhci_register_interrupt_ep(struct usb_hc *hc, struct usb_device *dev,
                                 uint8_t ep_num, void *buf, uint16_t len,
                                 usb_irq_notify_fn_t notify, void *notify_priv);

#endif