#include "usb.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"


static usb_hc_t     *hc_list      = NULL;
static usb_driver_t *driver_list  = NULL;
static usb_device_t *device_list  = NULL;

static uint8_t  addr_bitmap[USB_MAX_DEVICES / 8 + 1];
static uint32_t device_count = 0;
uint8_t usb_alloc_address(void) {
    for (uint8_t a = 1; a < USB_MAX_DEVICES; a++) {
        uint8_t byte = a >> 3, bit = a & 7;
        if (!(addr_bitmap[byte] & (1 << bit))) {
            addr_bitmap[byte] |= (1 << bit);
            return a;
        }
    }
    return 0;
}

void usb_free_address(uint8_t addr) {
    if (!addr || addr >= USB_MAX_DEVICES) return;
    addr_bitmap[addr >> 3] &= ~(1 << (addr & 7));
}

int usb_hc_register(usb_hc_t *hc) {
    if (!hc) return -1;
    hc->next = hc_list;
    hc_list  = hc;
    return 0;
}

int usb_driver_register(usb_driver_t *drv) {
    if (!drv) return -1;
    drv->next   = driver_list;
    driver_list = drv;
    return 0;
}

static usb_driver_t *usb_find_driver(usb_device_t *dev) {
    for (usb_driver_t *d = driver_list; d; d = d->next) {
        if (d->class_code != 0xFF && d->class_code != dev->class_code) continue;
        if (d->subclass   != 0xFF && d->subclass   != dev->subclass)   continue;
        if (d->protocol   != 0xFF && d->protocol   != dev->protocol)   continue;
        return d;
    }
    return NULL;
}

int usb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t idx,
                        void *buf, uint16_t len)
{
    usb_setup_pkt_t setup = {
        .bmRequestType = USB_RT_DEV_TO_HOST,
        .bRequest      = USB_REQ_GET_DESCRIPTOR,
        .wValue        = (uint16_t)((type << 8) | idx),
        .wIndex        = 0,
        .wLength       = len
    };
    return dev->hc->control_transfer(dev->hc, dev, &setup, buf, len);
}

int usb_set_address(usb_device_t *dev, uint8_t addr) {
    usb_setup_pkt_t setup = {
        .bmRequestType = USB_RT_HOST_TO_DEV,
        .bRequest      = USB_REQ_SET_ADDRESS,
        .wValue        = addr,
        .wIndex        = 0,
        .wLength       = 0
    };
    return dev->hc->control_transfer(dev->hc, dev, &setup, NULL, 0);
}

int usb_set_configuration(usb_device_t *dev, uint8_t cfg) {
    usb_setup_pkt_t setup = {
        .bmRequestType = USB_RT_HOST_TO_DEV,
        .bRequest      = USB_REQ_SET_CONFIGURATION,
        .wValue        = cfg,
        .wIndex        = 0,
        .wLength       = 0
    };
    return dev->hc->control_transfer(dev->hc, dev, &setup, NULL, 0);
}

int usb_set_interface(usb_device_t *dev, uint8_t iface, uint8_t alt) {
    usb_setup_pkt_t setup = {
        .bmRequestType = USB_RT_HOST_TO_DEV | USB_RT_INTERFACE,
        .bRequest      = USB_REQ_SET_INTERFACE,
        .wValue        = alt,
        .wIndex        = iface,
        .wLength       = 0
    };
    return dev->hc->control_transfer(dev->hc, dev, &setup, NULL, 0);
}

static void usb_parse_config(usb_device_t *dev) {
    uint8_t *p   = dev->config_buf;
    uint8_t *end = p + dev->config_len;
    dev->ep_count = 0;

    while (p + 2 <= end) {
        uint8_t len  = p[0];
        uint8_t type = p[1];
        if (len < 2 || p + len > end) break;

        if (type == USB_DESC_INTERFACE && len >= 9) {
            usb_iface_desc_t *iface = (usb_iface_desc_t *)p;
            if (dev->class_code == 0) {
                dev->class_code = iface->bInterfaceClass;
                dev->subclass   = iface->bInterfaceSubClass;
                dev->protocol   = iface->bInterfaceProtocol;
            }
        } else if (type == USB_DESC_ENDPOINT && len >= 7) {
            usb_ep_desc_t *ep_d = (usb_ep_desc_t *)p;
            if (dev->ep_count < USB_MAX_ENDPOINTS) {
                usb_endpoint_t *ep = &dev->ep[dev->ep_count++];
                ep->address       = ep_d->bEndpointAddress & 0x0F;
                ep->direction     = (ep_d->bEndpointAddress & 0x80)
                                  ? USB_DIR_IN : USB_DIR_OUT;
                ep->transfer_type = ep_d->bmAttributes & 0x03;
                ep->max_packet    = ep_d->wMaxPacketSize & 0x7FF;
                ep->interval      = ep_d->bInterval;
                ep->toggle        = 0;
            }
        }
        p += len;
    }
}

usb_device_t *usb_device_enumerate(usb_hc_t *hc, uint8_t port, uint8_t speed)
{
    if (hc->port_reset(hc, port) != 0) {
        printk("[USB] Port reset failed\n");
        return NULL;
    }

    usb_device_t *dev = (usb_device_t *)kmalloc(sizeof(usb_device_t));
    if (!dev) return NULL;
    memset(dev, 0, sizeof(usb_device_t));

    dev->hc      = hc;
    dev->port    = port;
    dev->speed   = speed;
    dev->address = USB_DEFAULT_ADDRESS;

    if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0, &dev->dev_desc, 8) < 0) {
        printk("[USB] Failed to read device descriptor\n");
        kfree(dev);
        return NULL;
    }

    uint8_t new_addr = usb_alloc_address();
    if (!new_addr || usb_set_address(dev, new_addr) < 0) {
        printk("[USB] SET_ADDRESS failed\n");
        usb_free_address(new_addr);
        kfree(dev);
        return NULL;
    }
    dev->address = new_addr;

    if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0,
                            &dev->dev_desc, sizeof(usb_dev_desc_t)) < 0) {
        usb_free_address(new_addr);
        kfree(dev);
        return NULL;
    }

    usb_cfg_desc_t cfg_hdr;
    if (usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0,
                            &cfg_hdr, sizeof(usb_cfg_desc_t)) < 0) {
        usb_free_address(new_addr);
        kfree(dev);
        return NULL;
    }

    uint16_t total = cfg_hdr.wTotalLength;
    if (total > sizeof(dev->config_buf)) total = sizeof(dev->config_buf);

    if (usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0,
                            dev->config_buf, total) < 0) {
        usb_free_address(new_addr);
        kfree(dev);
        return NULL;
    }
    dev->config_len = total;

    usb_parse_config(dev);
    usb_set_configuration(dev, cfg_hdr.bConfigurationValue);

    dev->next   = device_list;
    device_list = dev;
    device_count++;

    usb_driver_t *drv = usb_find_driver(dev);
    if (drv) {
        drv->probe(dev);
    }

    return dev;
}


void usb_device_disconnect(usb_hc_t *hc, uint8_t port) {
    usb_device_t **pp = &device_list;
    while (*pp) {
        usb_device_t *dev = *pp;
        if (dev->hc == hc && dev->port == port) {
            if (hc->device_removed)
                hc->device_removed(hc, dev);
            usb_driver_t *drv = usb_find_driver(dev);
            if (drv && drv->remove) drv->remove(dev);
            usb_free_address(dev->address);
            *pp = dev->next;
            kfree(dev);
            device_count--;
            return;
        }
        pp = &(*pp)->next;
    }
}


void usb_init(void) {
    memset(addr_bitmap, 0, sizeof(addr_bitmap));
    hc_list      = NULL;
    driver_list  = NULL;
    device_list  = NULL;
    device_count = 0;

    extern void usb_hid_init(void);
    extern void usb_hub_init(void);
    usb_hid_init();
    usb_hub_init();

    extern void xhci_pci_init(void);
    xhci_pci_init();
    pr_info("USB subsystem initialized (HID, hub, xHCI PCI)");
}


int usb_register_device(usb_device_t *dev) {
    if (!dev) return -1;
    dev->next   = device_list;
    device_list = dev;
    device_count++;
    usb_driver_t *drv = usb_find_driver(dev);
    if (drv && drv->probe) drv->probe(dev);
    return 0;
}

void usb_dump_devices(void) {
    (void)device_count;
}