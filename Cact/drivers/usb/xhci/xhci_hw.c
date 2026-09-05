/* xHCI — host controller bring-up: MMIO mapping, reset, ring/context
 * allocation, and initial port + device enumeration. */

#include "xhci.h"
#include "xhci_internal.h"
#include "usb.h"
#include "pci_enum.h"
#include "pci_driver.h"
#include "pci.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "sync.h"
#include "msi.h"

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

    xhci_op_write32(priv, XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    for (int i = 0; i < 100; i++) {
        if (!(xhci_op_read32(priv, XHCI_OP_USBCMD) & XHCI_CMD_HCRST)) break;
        xhci_udelay(1000);
    }
    for (int i = 0; i < 100; i++) {
        if (!(xhci_op_read32(priv, XHCI_OP_USBSTS) & XHCI_STS_CNR)) break;
        xhci_udelay(1000);
    }
    xhci_op_write32(priv, XHCI_OP_USBSTS, xhci_op_read32(priv, XHCI_OP_USBSTS));

    if (priv->quirks & XHCI_QUIRK_INTEL_HOST)
        xhci_udelay(5000);   /* Intel hosts need extra settle after reset */

    xhci_op_write32(priv, XHCI_OP_DNCTRL, 0x2);
    xhci_op_write32(priv, XHCI_OP_CONFIG, priv->max_slots);

    priv->dcbaa = (uint64_t *)kmalloc_aligned((priv->max_slots + 1) * sizeof(uint64_t), 64);
    if (!priv->dcbaa) { kfree(priv); return -1; }
    memset(priv->dcbaa, 0, (priv->max_slots + 1) * sizeof(uint64_t));
    xhci_op_write32(priv, XHCI_OP_DCBAAP, xhci_va_to_pa(priv->dcbaa));
    xhci_op_write32(priv, XHCI_OP_DCBAAP + 4, 0);

    priv->dev_ctx_pool = (uint8_t *)kmalloc_aligned((priv->max_slots + 1) * 2048, 64);
    if (!priv->dev_ctx_pool) { kfree(priv->dcbaa); kfree(priv); return -1; }
    memset(priv->dev_ctx_pool, 0, (priv->max_slots + 1) * 2048);

    priv->input_ctx_pool = (uint8_t *)kmalloc_aligned(2048, 64);
    if (!priv->input_ctx_pool) { kfree(priv->dev_ctx_pool); kfree(priv->dcbaa); kfree(priv); return -1; }

    xhci_trb_t *cmd_mem = (xhci_trb_t *)kmalloc_aligned(XHCI_CMD_RING_SIZE * sizeof(xhci_trb_t), 64);
    if (!cmd_mem) { kfree(priv->input_ctx_pool); kfree(priv->dev_ctx_pool); kfree(priv->dcbaa); kfree(priv); return -1; }
    xhci_ring_init(&priv->cmd_ring, cmd_mem, XHCI_CMD_RING_SIZE);

    xhci_op_write32(priv, XHCI_OP_CRCR, xhci_va_to_pa(cmd_mem) | 1);
    xhci_op_write32(priv, XHCI_OP_CRCR + 4, 0);

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
    hc->port_reset         = xhci_port_reset;
    hc->port_get_status    = xhci_port_get_status;
    hc->device_removed     = xhci_device_removed;
    hc->num_ports          = priv->max_ports;
    hc->priv               = priv;

    hc->irq_next  = xhci_hc_list;
    xhci_hc_list  = hc;

    usb_hc_register(hc);

    for (uint8_t p = 0; p < priv->max_ports; p++) {
        uint32_t sc = xhci_portsc_read(priv, p);
        if (sc & XHCI_PORTSC_CCS) {
            if (!(sc & XHCI_PORTSC_PED)) {
                xhci_portsc_set(priv, p, XHCI_PORTSC_PR);
                for (int i = 0; i < 500; i++) {
                    xhci_udelay(1000);
                    sc = xhci_portsc_read(priv, p);
                    /* Reset is done once PRC is raised; some controllers (and
                     * QEMU's xHCI model) finish by clearing PR and enabling
                     * the port without ever raising PRC — catch that too, or
                     * the wait burns the whole 500-iteration budget. */
                    if (sc & XHCI_PORTSC_PRC) break;
                    if (!(sc & XHCI_PORTSC_PR) && (sc & XHCI_PORTSC_PED)) break;
                }
                if (sc & XHCI_PORTSC_PRC)
                    xhci_portsc_clear_change(priv, p, XHCI_PORTSC_PRC);
                if (sc & XHCI_PORTSC_CSC)
                    xhci_portsc_clear_change(priv, p, XHCI_PORTSC_CSC);
                xhci_udelay(10000);
                sc = xhci_portsc_read(priv, p);
            }

            if (sc & XHCI_PORTSC_PED) {
                uint8_t speed = xhci_port_speed_to_usb(sc);
                uint8_t slot_id;
                if (xhci_enable_slot(priv, &slot_id) == 0) {
                    priv->slot_used[slot_id] = 1;
                    priv->slot_port[slot_id] = p;

                    if (xhci_address_device(priv, slot_id, p, speed, 0) == 0) {
                        usb_device_t *dev = (usb_device_t *)kmalloc(sizeof(usb_device_t));
                        if (dev) {
                            memset(dev, 0, sizeof(usb_device_t));
                            dev->hc      = hc;
                            dev->port    = p;
                            dev->speed   = speed;
                            dev->address = slot_id;

                            if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0,
                                                    &dev->dev_desc,
                                                    sizeof(usb_dev_desc_t)) >= 0) {
                                usb_cfg_desc_t cfg_hdr;
                                if (usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0,
                                                        &cfg_hdr, sizeof(usb_cfg_desc_t)) >= 0) {
                                    uint16_t total = cfg_hdr.wTotalLength;
                                    if (total > sizeof(dev->config_buf))
                                        total = sizeof(dev->config_buf);
                                    usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0,
                                                        dev->config_buf, total);
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
                                }

                                usb_register_device(dev);
                            }
                        }
                    }
                }
            }
        }
    }

    return 0;
}
