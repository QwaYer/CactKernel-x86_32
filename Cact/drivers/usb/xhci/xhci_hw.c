/* xHCI — host controller bring-up, root-port probing, and hotplug.
 *
 * The register programming is factored out of xhci_init_one() so the fatal
 * Host System Error path (which resets the controller from interrupt context)
 * can have the hotplug task put it back into service, and the port probe is a
 * single routine shared by the initial scan and by a later connect. */

#include "xhci.h"
#include "xhci_internal.h"
#include "usb.h"
#include "pci_enum.h"
#include "pci_driver.h"
#include "pci.h"
#include "kernel.h"
#include "task.h"
#include "memory.h"
#include "klib.h"
#include "sync.h"
#include "msi.h"

extern void sched_sleep_ticks(uint32_t ticks);

static int  xhci_probe_port(xhci_priv_t *priv, usb_hc_t *hc, uint8_t port);
static void xhci_probe_retry(xhci_priv_t *priv, usb_hc_t *hc, uint8_t port);
static usb_device_t *xhci_enumerate_hub_child(usb_hc_t *hc, usb_device_t *hub_dev,
                                              uint8_t port, uint8_t speed);
static int xhci_update_hub_hc(usb_hc_t *hc, usb_device_t *hub_dev,
                              uint8_t num_ports);
static void xhci_scan_ports(xhci_priv_t *priv, usb_hc_t *hc);
static void xhci_recover_after_reset(xhci_priv_t *priv, usb_hc_t *hc);

/* Program the ring registers and start the controller.  Assumes the host
 * memory (DCBAA, scratchpad, command/event rings) is allocated; safe to call
 * again after an HCRST, which is what the HSE recovery path does. */
static int xhci_program_regs(xhci_priv_t *priv) {
    /* The command ring and the interrupter state are controller-owned and were
     * wiped by the reset: re-initialise them before pointing the controller at
     * them again. */
    if (priv->cmd_ring.ring)
        xhci_ring_init(&priv->cmd_ring, priv->cmd_ring.ring, XHCI_CMD_RING_SIZE);
    if (priv->evt_ring) {
        memset(priv->evt_ring, 0, XHCI_EVT_RING_SIZE * sizeof(xhci_trb_t));
        priv->evt_dequeue = 0;
        priv->evt_cycle   = 1;
    }

    xhci_op_write32(priv, XHCI_OP_DNCTRL, 0x2);
    xhci_op_write32(priv, XHCI_OP_CONFIG, priv->max_slots);

    xhci_op_write32(priv, XHCI_OP_DCBAAP, xhci_va_to_pa(priv->dcbaa));
    xhci_op_write32(priv, XHCI_OP_DCBAAP + 4, 0);

    priv->cmd_ring_phys = xhci_va_to_pa(priv->cmd_ring.ring);
    xhci_op_write32(priv, XHCI_OP_CRCR, priv->cmd_ring_phys | 1);
    xhci_op_write32(priv, XHCI_OP_CRCR + 4, 0);

    xhci_rt_write32(priv, 0x20 + XHCI_ERSTSZ, XHCI_ERST_SIZE);
    xhci_rt_write32(priv, 0x20 + XHCI_ERDP, xhci_va_to_pa(priv->evt_ring) | (1u << 3));
    xhci_rt_write32(priv, 0x20 + XHCI_ERDP + 4, 0);
    xhci_rt_write32(priv, 0x20 + XHCI_ERSTBA, xhci_va_to_pa(priv->erst));
    xhci_rt_write32(priv, 0x20 + XHCI_ERSTBA + 4, 0);

    xhci_rt_write32(priv, 0x20 + XHCI_IMOD, 0x000003F8);
    xhci_rt_write32(priv, 0x20 + XHCI_IMAN, 0x3);

    uint32_t run_cmd = XHCI_CMD_RS | XHCI_CMD_INTE;
    if (priv->quirks & XHCI_QUIRK_SPURIOUS_REBOOT) {
        /* Intel 300-series can raise a spurious host-system-error that trips
         * a chipset reboot; keep HSEE disabled so it can never fire. */
        pr_info("  %-11s : HSE interrupt masked (spurious-reboot quirk)\n", "xhci");
    } else {
        run_cmd |= XHCI_CMD_HSEE;
    }
    xhci_op_write32(priv, XHCI_OP_USBCMD, run_cmd);

    for (int i = 0; i < 100; i++) {
        if (!(xhci_op_read32(priv, XHCI_OP_USBSTS) & XHCI_STS_HCH)) break;
        xhci_udelay(1000);
    }
    if (xhci_op_read32(priv, XHCI_OP_USBSTS) & XHCI_STS_HCH) {
        pr_warn("  %-11s : host controller did not start\n", "xhci");
        return -1;
    }
    return 0;
}

int xhci_init_one(uint32_t phys_base, uint32_t quirks) {
    extern uint32_t page_directory[1024];
    uint32_t map_size = 0x10000;
    /* Map xHCI MMIO as uncacheable (PCD|PWT). */
    for (uint32_t off = 0; off < map_size; off += 0x1000)
        vmm_map(page_directory, phys_base + off, phys_base + off,
                PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);

    xhci_priv_t *priv = (xhci_priv_t *)kmalloc(sizeof(xhci_priv_t));
    if (!priv) { pr_warn("  %-11s : state allocation failed\n", "xhci"); return -1; }
    memset(priv, 0, sizeof(xhci_priv_t));
    priv->scanning = 1;   /* stay off-limits to the hotplug task until scanned */
    spin_lock_init(&priv->ctx_lock);
    priv->quirks = quirks;

    priv->cap_phys = phys_base;
    priv->cap = (volatile uint32_t *)phys_base;

    uint8_t  cap_len    = (uint8_t)(xhci_cap_read32(priv, XHCI_CAP_CAPLENGTH) & 0xFF);
    uint32_t hcsparams1 = xhci_cap_read32(priv, XHCI_CAP_HCSPARAMS1);
    uint32_t hccparams1 = xhci_cap_read32(priv, XHCI_CAP_HCCPARAMS1);

    priv->max_slots = (uint8_t)(hcsparams1 & 0xFF);
    priv->max_intrs = (uint16_t)((hcsparams1 >> 8) & 0x7FF);
    priv->max_ports = (uint8_t)((hcsparams1 >> 24) & 0xFF);
    priv->context_size = (hccparams1 & (1u << 2)) ? 64 : 32;

    if (priv->max_slots > XHCI_MAX_SLOTS) priv->max_slots = XHCI_MAX_SLOTS;
    if (priv->max_ports > XHCI_MAX_PORTS) priv->max_ports = XHCI_MAX_PORTS;

    priv->op_off = cap_len;
    priv->op     = (volatile uint32_t *)(phys_base + cap_len);
    priv->rt_off = xhci_cap_read32(priv, XHCI_CAP_RTSOFF) & ~0x1F;
    priv->rt     = (volatile uint32_t *)(phys_base + priv->rt_off);
    priv->db_off = xhci_cap_read32(priv, XHCI_CAP_DBOFF) & ~0x3;
    priv->db     = (volatile uint32_t *)(phys_base + priv->db_off);

    uint32_t xecp_off = ((hccparams1 >> 16) & 0xFFFF) << 2;
    if (xecp_off) {
        uint32_t cur = xecp_off;
        for (int i = 0; i < 32 && cur; i++) {
            volatile uint32_t *ecap = (volatile uint32_t *)(phys_base + cur);
            uint8_t ecap_id = (uint8_t)(ecap[0] & 0xFF);
            if (ecap_id == 1) {
                ecap[0] |= (1u << 24);
                for (int w = 0; w < 100; w++) {
                    if ((ecap[0] & (1u << 24)) && !(ecap[0] & (1u << 16))) break;
                    xhci_udelay(10000);
                }
                if (ecap[0] & (1u << 16))
                    pr_warn("  %-11s : BIOS ownership handoff timed out\n", "xhci");
                ecap[1] = 0;
                break;
            }
            uint8_t next = (uint8_t)((ecap[0] >> 8) & 0xFF);
            if (!next) break;
            cur += (uint32_t)next << 2;
        }
    }

    xhci_op_write32(priv, XHCI_OP_USBCMD,
                     xhci_op_read32(priv, XHCI_OP_USBCMD) & ~XHCI_CMD_RS);
    for (int i = 0; i < 100; i++) {
        if (xhci_op_read32(priv, XHCI_OP_USBSTS) & XHCI_STS_HCH) break;
        xhci_udelay(1000);
    }
    if (!(xhci_op_read32(priv, XHCI_OP_USBSTS) & XHCI_STS_HCH))
        pr_warn("  %-11s : controller did not halt (USBSTS=0x%x)\n", "xhci",
                xhci_op_read32(priv, XHCI_OP_USBSTS));

    xhci_op_write32(priv, XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    for (int i = 0; i < 100; i++) {
        if (!(xhci_op_read32(priv, XHCI_OP_USBCMD) & XHCI_CMD_HCRST)) break;
        xhci_udelay(1000);
    }
    for (int i = 0; i < 100; i++) {
        if (!(xhci_op_read32(priv, XHCI_OP_USBSTS) & XHCI_STS_CNR)) break;
        xhci_udelay(1000);
    }
    if (xhci_op_read32(priv, XHCI_OP_USBCMD) & XHCI_CMD_HCRST)
        pr_warn("  %-11s : HCRST never self-cleared\n", "xhci");
    xhci_op_write32(priv, XHCI_OP_USBSTS, xhci_op_read32(priv, XHCI_OP_USBSTS));

    /* The ring registers (CRCR, DCBAAP) are only accepted while the controller
     * is halted; while it runs the writes are dropped with no error.  If the
     * reset left it running, force a stop before giving up. */
    uint32_t sts = xhci_op_read32(priv, XHCI_OP_USBSTS);
    if (!(sts & XHCI_STS_HCH) || (sts & XHCI_STS_CNR)) {
        xhci_op_write32(priv, XHCI_OP_USBCMD, 0);
        for (int i = 0; i < 100; i++) {
            if (xhci_op_read32(priv, XHCI_OP_USBSTS) & XHCI_STS_HCH) break;
            xhci_udelay(1000);
        }
        sts = xhci_op_read32(priv, XHCI_OP_USBSTS);
        if (!(sts & XHCI_STS_HCH) || (sts & XHCI_STS_CNR)) {
            pr_err("  %-11s : controller not halted/ready after reset (USBSTS=0x%x)\n",
                   "xhci", sts);
            kfree(priv);
            return -1;
        }
    }

    if (priv->quirks & XHCI_QUIRK_INTEL_HOST)
        xhci_udelay(5000);   /* Intel hosts need extra settle after reset */

    priv->dcbaa = (uint64_t *)kmalloc_aligned((priv->max_slots + 1) * sizeof(uint64_t), 64);
    if (!priv->dcbaa) { kfree(priv); return -1; }
    memset(priv->dcbaa, 0, (priv->max_slots + 1) * sizeof(uint64_t));

    /* Scratchpad buffers.  The controller says in HCSPARAMS2 how many it
     * needs; DCBAA[0] must then point at an array holding their addresses.
     * Without them the first operation that needs scratchpad memory faults
     * with a Host System Error and the controller halts.  QEMU asks for
     * none, so this only ever bites on real hardware. */
    uint32_t hcs2 = xhci_cap_read32(priv, XHCI_CAP_HCSPARAMS2);
    uint32_t sp_count = (((hcs2 >> 21) & 0x1Fu) << 4) | (hcs2 & 0xFu);
    if (sp_count) {
        priv->scratchpad_array = (uint64_t *)kmalloc_aligned(sp_count * sizeof(uint64_t), 64);
        priv->scratchpad_pool  = (uint8_t *)kmalloc_aligned(sp_count * 4096u, 4096);
        if (!priv->scratchpad_array || !priv->scratchpad_pool) {
            pr_err("  %-11s : scratchpad allocation failed (%u buffers)\n",
                   "xhci", (unsigned)sp_count);
            kfree(priv->scratchpad_array); kfree(priv->scratchpad_pool);
            kfree(priv->dcbaa); kfree(priv);
            return -1;
        }
        memset(priv->scratchpad_array, 0, sp_count * sizeof(uint64_t));
        memset(priv->scratchpad_pool, 0, sp_count * 4096u);
        for (uint32_t i = 0; i < sp_count; i++)
            priv->scratchpad_array[i] = xhci_va_to_pa(priv->scratchpad_pool + i * 4096u);
        priv->dcbaa[0] = xhci_va_to_pa(priv->scratchpad_array);
        pr_info("  %-11s : %u scratchpad buffers, pool=0x%x array=0x%x\n", "xhci",
                (unsigned)sp_count, xhci_va_to_pa(priv->scratchpad_pool),
                xhci_va_to_pa(priv->scratchpad_array));
    }

    priv->dev_ctx_pool = (uint8_t *)kmalloc_aligned((priv->max_slots + 1) * 2048, 64);
    if (!priv->dev_ctx_pool) { kfree(priv->dcbaa); kfree(priv); return -1; }
    memset(priv->dev_ctx_pool, 0, (priv->max_slots + 1) * 2048);

    priv->input_ctx_pool = (uint8_t *)kmalloc_aligned(2048, 64);
    if (!priv->input_ctx_pool) { kfree(priv->dev_ctx_pool); kfree(priv->dcbaa); kfree(priv); return -1; }

    xhci_trb_t *cmd_mem = (xhci_trb_t *)kmalloc_aligned(XHCI_CMD_RING_SIZE * sizeof(xhci_trb_t), 64);
    if (!cmd_mem) { kfree(priv->input_ctx_pool); kfree(priv->dev_ctx_pool); kfree(priv->dcbaa); kfree(priv); return -1; }
    xhci_ring_init(&priv->cmd_ring, cmd_mem, XHCI_CMD_RING_SIZE);

    priv->evt_ring = (xhci_trb_t *)kmalloc_aligned(XHCI_EVT_RING_SIZE * sizeof(xhci_trb_t), 64);
    if (!priv->evt_ring) { kfree(cmd_mem); kfree(priv->input_ctx_pool); kfree(priv->dev_ctx_pool); kfree(priv->dcbaa); kfree(priv); return -1; }
    memset(priv->evt_ring, 0, XHCI_EVT_RING_SIZE * sizeof(xhci_trb_t));
    priv->evt_dequeue = 0;
    priv->evt_cycle   = 1;

    priv->erst = (xhci_erst_entry_t *)kmalloc_aligned(sizeof(xhci_erst_entry_t) * XHCI_ERST_SIZE, 64);
    if (!priv->erst) { kfree(priv->evt_ring); kfree(cmd_mem); kfree(priv->input_ctx_pool); kfree(priv->dev_ctx_pool); kfree(priv->dcbaa); kfree(priv); return -1; }
    memset(priv->erst, 0, sizeof(xhci_erst_entry_t) * XHCI_ERST_SIZE);
    priv->erst[0].seg_addr_lo = xhci_va_to_pa(priv->evt_ring);
    priv->erst[0].seg_addr_hi = 0;
    priv->erst[0].seg_size    = XHCI_EVT_RING_SIZE;

    if (xhci_program_regs(priv) < 0) {
        kfree(priv->erst); kfree(priv->evt_ring); kfree(cmd_mem);
        kfree(priv->input_ctx_pool); kfree(priv->dev_ctx_pool);
        kfree(priv->dcbaa); kfree(priv);
        return -1;
    }

    usb_hc_t *hc = (usb_hc_t *)kmalloc(sizeof(usb_hc_t));
    if (!hc) { kfree(priv->erst); kfree(priv->evt_ring); kfree(cmd_mem); kfree(priv->input_ctx_pool); kfree(priv->dev_ctx_pool); kfree(priv->dcbaa); kfree(priv); return -1; }
    memset(hc, 0, sizeof(usb_hc_t));

    hc->name               = "XHCI";
    hc->control_transfer   = xhci_control_transfer;
    hc->interrupt_transfer = xhci_interrupt_transfer;
    hc->bulk_transfer      = xhci_bulk_transfer;
    hc->configure_endpoints = xhci_configure_device_endpoints;
    hc->enumerate_hub_child = xhci_enumerate_hub_child;
    hc->update_hub          = xhci_update_hub_hc;
    hc->port_reset         = xhci_port_reset;
    hc->port_get_status    = xhci_port_get_status;
    hc->device_removed     = xhci_device_removed;
    hc->num_ports          = priv->max_ports;
    hc->priv               = priv;

    hc->irq_next  = xhci_hc_list;
    xhci_hc_list  = hc;

    usb_hc_register(hc);

    xhci_scan_ports(priv, hc);

    return 0;
}

/* Read a device's descriptors, parse its configuration, bring up its endpoints
 * and register it.  The device must already have hc, address, port and speed
 * filled in (and `hub` for a device behind a hub).  Returns 0 or -1; on failure
 * the caller owns the slot. */
static int xhci_finish_enumeration(usb_hc_t *hc, usb_device_t *dev)
{
    if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0,
                            &dev->dev_desc, sizeof(usb_dev_desc_t)) < 0)
        return -1;

    usb_cfg_desc_t cfg_hdr;
    if (usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0,
                            &cfg_hdr, sizeof(usb_cfg_desc_t)) < 0)
        return -1;
    uint16_t total = cfg_hdr.wTotalLength;
    if (total > sizeof(dev->config_buf))
        total = sizeof(dev->config_buf);
    usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0, dev->config_buf, total);
    dev->config_len = total;

    uint8_t *cp   = dev->config_buf;
    uint8_t *cend = cp + dev->config_len;
    dev->ep_count = 0;
    while (cp + 2 <= cend) {
        uint8_t dlen = cp[0], dtype = cp[1];
        if (dlen < 2 || cp + dlen > cend) break;
        if (dtype == USB_DESC_INTERFACE && dlen >= 9) {
            usb_iface_desc_t *ifd = (usb_iface_desc_t *)cp;
            if (dev->class_code == 0) {
                dev->class_code = ifd->bInterfaceClass;
                dev->subclass   = ifd->bInterfaceSubClass;
                dev->protocol   = ifd->bInterfaceProtocol;
            }
        } else if (dtype == USB_DESC_ENDPOINT && dlen >= 7) {
            usb_ep_desc_t *epd = (usb_ep_desc_t *)cp;
            if (dev->ep_count < USB_MAX_ENDPOINTS) {
                usb_endpoint_t *epp = &dev->ep[dev->ep_count++];
                epp->address       = epd->bEndpointAddress & 0x0F;
                epp->direction     = (epd->bEndpointAddress & 0x80) ? USB_DIR_IN : USB_DIR_OUT;
                epp->transfer_type = epd->bmAttributes & 0x03;
                epp->max_packet    = epd->wMaxPacketSize & 0x7FF;
                epp->interval      = epd->bInterval;
                epp->toggle        = 0;
            }
        }
        cp += dlen;
    }

    usb_set_configuration(dev, cfg_hdr.bConfigurationValue);
    xhci_configure_device_endpoints(hc, dev);
    return 0;
}

/* Bring up one root port that already carries a device.  Shared by the initial
 * scan and by the hotplug task's connect path. */
static int xhci_probe_port(xhci_priv_t *priv, usb_hc_t *hc, uint8_t port)
{
    uint32_t sc = xhci_portsc_read(priv, port);
    if (!(sc & XHCI_PORTSC_CCS))
        return -1;

    if (xhci_port_reset(hc, port) != 0) {
        sc = xhci_portsc_read(priv, port);
        pr_warn("  %-11s : port %u did not enable (PORTSC=0x%x)\n",
                "xhci", (unsigned)port, sc);
        return -1;
    }

    sc = xhci_portsc_read(priv, port);
    uint8_t speed = xhci_port_speed_to_usb(sc);
    uint8_t slot_id;
    if (xhci_enable_slot(priv, &slot_id) != 0)
        return -1;
    priv->slot_used[slot_id] = 1;
    priv->slot_port[slot_id] = port;

    if (xhci_address_device(priv, slot_id, port, speed, 0, 0, 0) != 0) {
        pr_warn("  %-11s : port %u Address Device failed (speed %u, cc 0x%x)\n",
                "xhci", (unsigned)port, (unsigned)speed, (unsigned)priv->cmd_cc);
        goto fail;
    }

    usb_device_t *dev = (usb_device_t *)kmalloc(sizeof(usb_device_t));
    if (!dev)
        goto fail;
    memset(dev, 0, sizeof(usb_device_t));
    dev->hc      = hc;
    dev->port    = port;
    dev->speed   = speed;
    dev->address = slot_id;

    if (xhci_finish_enumeration(hc, dev) != 0) {
        pr_warn("  %-11s : port %u descriptor read failed\n",
                "xhci", (unsigned)port);
        goto fail_dev;
    }

    priv->port_dev[port] = dev;

    usb_register_device(dev);
    pr_info("  %-11s : port %u device %04x:%04x registered\n", "xhci",
            (unsigned)port, (unsigned)dev->dev_desc.idVendor,
            (unsigned)dev->dev_desc.idProduct);
    return 0;

fail_dev:
    kfree(dev);
fail:
    /* The slot was enabled but the device never came up.  Hand it to the
     * hotplug task to Disable Slot and free its rings; without this every
     * failed probe burned a slot until Enable Slot started failing. */
    priv->slot_used[slot_id] = 0;
    priv->slot_port[slot_id] = 0;
    __sync_fetch_and_or(&priv->slot_reap, 1u << slot_id);
    return -1;
}

/* Enumerate a device on a downstream port of a USB hub (the usb_hc_t hook).
 * The hub driver has already reset the downstream port; this does the xHCI
 * part: a fresh slot plus Address Device carrying the topology in the route
 * string, then the ordinary descriptor/configuration path.  Without this a
 * device behind a hub could never be brought up — the generic path issues
 * SET_ADDRESS over a control transfer, and a device at address 0 has no slot
 * for the xHC to issue it on. */
static usb_device_t *xhci_enumerate_hub_child(usb_hc_t *hc, usb_device_t *hub_dev,
                                              uint8_t port, uint8_t speed)
{
    xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
    uint8_t slot_id;
    if (xhci_enable_slot(priv, &slot_id) != 0)
        return NULL;
    priv->slot_used[slot_id] = 1;
    priv->slot_port[slot_id] = port;

    /* `hub_dev->port` is the root port the hub hangs off (Root Hub Port Number)
     * and `port` is the downstream port (Route String).  No TT is programmed:
     * an LS/FS device behind a high-speed hub would need it and is not
     * supported yet, but the SuperSpeed/high-speed cases do not. */
    if (xhci_address_device(priv, slot_id, hub_dev->port, speed, 0,
                            hub_dev->address, port) != 0) {
        pr_warn("  %-11s : hub port %u Address Device failed (speed %u, cc 0x%x)\n",
                "xhci", (unsigned)port, (unsigned)speed, (unsigned)priv->cmd_cc);
        goto fail;
    }

    usb_device_t *dev = (usb_device_t *)kmalloc(sizeof(usb_device_t));
    if (!dev)
        goto fail;
    memset(dev, 0, sizeof(usb_device_t));
    dev->hc      = hc;
    dev->hub     = hub_dev;
    dev->port    = port;
    dev->speed   = speed;
    dev->address = slot_id;

    if (xhci_finish_enumeration(hc, dev) != 0) {
        pr_warn("  %-11s : hub port %u descriptor read failed\n",
                "xhci", (unsigned)port);
        goto fail_dev;
    }

    usb_register_device(dev);
    pr_info("  %-11s : hub port %u device %04x:%04x registered\n", "xhci",
            (unsigned)port, (unsigned)dev->dev_desc.idVendor,
            (unsigned)dev->dev_desc.idProduct);
    return dev;

fail_dev:
    kfree(dev);
fail:
    priv->slot_used[slot_id] = 0;
    priv->slot_port[slot_id] = 0;
    __sync_fetch_and_or(&priv->slot_reap, 1u << slot_id);
    return NULL;
}

static int xhci_update_hub_hc(usb_hc_t *hc, usb_device_t *hub_dev,
                              uint8_t num_ports)
{
    return xhci_update_hub((xhci_priv_t *)hc->priv, hub_dev->address, num_ports);
}

/* Walk every port of the controller and bring up whatever is attached.
 * Shared by the initial bring-up and the post-reset re-enumeration. */
static void xhci_scan_ports(xhci_priv_t *priv, usb_hc_t *hc)
{
    /* Keep the hotplug task out while enumerating: the resets below raise
     * port-status changes, and a probe racing this one would try to address a
     * device that is already being addressed (and has not been reset again). */
    priv->scanning = 1;

    /* The controller has been posting port-status-change events since boot and
     * nothing has drained them, so every connected port still carries its
     * change latches.  Process them first. */
    xhci_poll_events(priv);

    /* Right after the controller is started a USB2 port sits in Polling while
     * the xHC finishes its own connect detection; a software write to PR in
     * that window does nothing (the reset engine is already busy).  Wait for
     * the connected ports to come out of Polling before touching them. */
    for (int i = 0; i < 400; i++) {
        int busy = 0;
        for (uint8_t p = 0; p < priv->max_ports; p++) {
            uint32_t sc = xhci_portsc_read(priv, p);
            if ((sc & XHCI_PORTSC_CCS) && (((sc >> 5) & 0xF) == 7))
                busy = 1;
        }
        if (!busy) break;
        xhci_udelay(1000);
    }

    for (uint8_t p = 0; p < priv->max_ports; p++) {
        uint32_t sc = xhci_portsc_read(priv, p);
        if (!(sc & XHCI_PORTSC_CCS))
            continue;
        pr_info("  %-11s : port %u connected (PED=%u speed=%u)\n", "xhci",
                (unsigned)p, (unsigned)((sc >> 1) & 1),
                (unsigned)((sc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT));
        if (priv->port_dev[p])
            continue;                 /* already enumerated */
        xhci_probe_retry(priv, hc, p);
    }

    priv->scanning = 0;
}

/* ── root-port hotplug ─────────────────────────────────────────────────────*/

void xhci_mark_port(xhci_priv_t *priv, uint8_t port) {
    if (port >= priv->max_ports || port >= 32)
        return;
    __sync_fetch_and_or(&priv->port_events, 1u << port);
}

/* Bring a port up, retrying a few times.  A freshly attached device can need
 * more than one attempt — the link may still be training when the first reset
 * lands, or the first Address Device can fail transiently — and a single
 * failure used to leave the port dead until the next unplug, because the
 * change latches had already been cleared and nothing re-armed the port.
 * Linux retries port init too. */
static void xhci_probe_retry(xhci_priv_t *priv, usb_hc_t *hc, uint8_t port) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (xhci_probe_port(priv, hc, port) == 0)
            return;
        xhci_udelay(20000);
    }
    pr_warn("  %-11s : port %u bring-up failed after retries\n",
            "xhci", (unsigned)port);
}

void xhci_handle_port_event(xhci_priv_t *priv, usb_hc_t *hc, uint8_t port) {
    uint32_t sc = xhci_portsc_read(priv, port);
    uint32_t changes = sc & XHCI_PORTSC_RW1C_BITS;
    if (changes)
        xhci_portsc_clear_change(priv, port, changes);

    int present = (sc & XHCI_PORTSC_CCS) != 0;
    usb_device_t *dev = (port < XHCI_MAX_PORTS) ? priv->port_dev[port] : NULL;

    if (present && !dev) {
        xhci_probe_retry(priv, hc, port);
    } else if (!present && dev) {
        usb_device_disconnect(hc, port);
    } else if (present && dev && !(sc & XHCI_PORTSC_PED)) {
        /* The recorded device is no longer enabled on the port, so a different
         * one replaced it.  (A connect-status change alone is not usable here:
         * the port reset itself raises CSC, so it cannot tell a replacement
         * from the change our own probe just caused.) */
        usb_device_disconnect(hc, port);
        xhci_probe_retry(priv, hc, port);
    }
}

/* The IRQ path reset the controller after a fatal HSE; everything the driver
 * knew about the topology is stale.  Drop the devices, tear down their slots,
 * re-program the registers and enumerate again. */
static void xhci_recover_after_reset(xhci_priv_t *priv, usb_hc_t *hc) {
    pr_info("  %-11s : recovering controller after reset\n", "xhci");

    for (uint8_t p = 0; p < priv->max_ports; p++)
        if (priv->port_dev[p])
            usb_device_disconnect(hc, p);   /* sets slot_reap */

    uint32_t reap = __sync_lock_test_and_set(&priv->slot_reap, 0);
    while (reap) {
        uint8_t s = (uint8_t)__builtin_ctz(reap);
        reap &= ~(1u << s);
        xhci_free_ep_rings(priv, s);
        priv->slot_used[s] = 0;
        priv->slot_port[s] = 0;
        if (priv->dcbaa) priv->dcbaa[s] = 0;
    }
    for (int i = 0; i < XHCI_MAX_INTR_EP; i++) {
        priv->intr_slots[i].active = 0;
        priv->intr_slots[i].ring   = NULL;
    }
    priv->intr_ep_count = 0;

    if (xhci_program_regs(priv) < 0) {
        pr_err("  %-11s : recovery failed, controller left halted\n", "xhci");
        return;
    }
    xhci_scan_ports(priv, hc);
}

/* One task drains port events and reaps dead slots.  Both need sleeping and
 * command submission, so neither can run from the interrupt handler. */
static void xhci_hotplug_task(void) {
    while (1) {
        for (usb_hc_t *hc = xhci_hc_list; hc; hc = hc->irq_next) {
            xhci_priv_t *priv = (xhci_priv_t *)hc->priv;
            if (!priv) continue;
            /* Leave the controller alone while it is being scanned; the port
             * events stay latched and are handled on the next round. */
            if (priv->scanning) continue;

            if (priv->reset_pending) {
                priv->reset_pending = 0;
                xhci_recover_after_reset(priv, hc);
            }

            uint32_t mask = __sync_lock_test_and_set(&priv->port_events, 0);
            while (mask) {
                uint8_t p = (uint8_t)__builtin_ctz(mask);
                mask &= ~(1u << p);
                xhci_handle_port_event(priv, hc, p);
            }

            uint32_t reap = __sync_lock_test_and_set(&priv->slot_reap, 0);
            while (reap) {
                uint8_t s = (uint8_t)__builtin_ctz(reap);
                reap &= ~(1u << s);
                if (s >= 1 && s <= priv->max_slots) {
                    xhci_disable_slot(priv, s);
                    xhci_free_ep_rings(priv, s);
                    priv->slot_used[s] = 0;
                    priv->slot_port[s] = 0;
                    if (priv->dcbaa) priv->dcbaa[s] = 0;
                }
            }
        }

        /* React within one tick: a hotplugged device should appear as fast as
         * on a desktop OS, and the loop body is a couple of atomics per
         * controller when there is nothing to do. */
        sched_sleep_ticks(1);
    }
}

void xhci_hotplug_init(void) {
    if (!create_task(xhci_hotplug_task)) {
        pr_warn("  %-11s : hotplug task could not be created\n", "xhci");
        return;
    }
    pr_info("  %-11s : root-port hotplug task up\n", "xhci");
}
