#include "usb_hub.h"
#include "usb.h"
#include "xhci.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"


static int hub_set_port_feature(usb_device_t *dev, uint8_t port, uint16_t feat) {
    usb_setup_pkt_t s = {
        .bmRequestType = USB_RT_HOST_TO_DEV | USB_RT_CLASS | USB_RT_OTHER,
        .bRequest      = HUB_REQ_SET_FEATURE,
        .wValue        = feat, .wIndex = port, .wLength = 0
    };
    return dev->hc->control_transfer(dev->hc, dev, &s, NULL, 0);
}

static int hub_clear_port_feature(usb_device_t *dev, uint8_t port, uint16_t feat) {
    usb_setup_pkt_t s = {
        .bmRequestType = USB_RT_HOST_TO_DEV | USB_RT_CLASS | USB_RT_OTHER,
        .bRequest      = HUB_REQ_CLEAR_FEATURE,
        .wValue        = feat, .wIndex = port, .wLength = 0
    };
    return dev->hc->control_transfer(dev->hc, dev, &s, NULL, 0);
}

static int hub_get_port_status(usb_device_t *dev, uint8_t port,
                                hub_port_status_t *st)
{
    usb_setup_pkt_t s = {
        .bmRequestType = USB_RT_DEV_TO_HOST | USB_RT_CLASS | USB_RT_OTHER,
        .bRequest      = HUB_REQ_GET_STATUS,
        .wValue        = 0, .wIndex = port, .wLength = 4
    };
    return dev->hc->control_transfer(dev->hc, dev, &s, st, 4);
}

static int hub_get_descriptor(usb_device_t *dev, usb_hub_desc_t *h) {
    usb_setup_pkt_t s = {
        .bmRequestType = USB_RT_DEV_TO_HOST | USB_RT_CLASS,
        .bRequest      = HUB_REQ_GET_DESCRIPTOR,
        .wValue        = (USB_DESC_HUB << 8),
        .wIndex        = 0, .wLength = sizeof(usb_hub_desc_t)
    };
    return dev->hc->control_transfer(dev->hc, dev, &s, h, sizeof(usb_hub_desc_t));
}

static int hub_port_reset(usb_device_t *hub_dev, uint8_t port) {
    hub_set_port_feature(hub_dev, port, PORT_RESET);

    for (int i = 0; i < 50; i++) {
        for (volatile uint32_t d = 0; d < 50000; d++);
        hub_port_status_t ps;
        if (hub_get_port_status(hub_dev, port, &ps) < 0) return -1;
        if (ps.wPortChange & HUB_PORT_CHG_RESET) {
            hub_clear_port_feature(hub_dev, port, C_PORT_RESET);
            return (ps.wPortStatus & HUB_PORT_STS_ENABLE) ? 0 : -1;
        }
    }
    return -1;
}

typedef struct {
    usb_hc_t      hc_wrapper;
    usb_device_t *hub_dev;
    uint8_t       hub_port;
} hub_hc_wrapper_t;

static int hub_port_reset_wrapper(usb_hc_t *hc, uint8_t port) {
    hub_hc_wrapper_t *w = (hub_hc_wrapper_t *)hc;
    return hub_port_reset(w->hub_dev, port);
    (void)port;
}

static void hub_handle_port(usb_hub_priv_t *priv, uint8_t port) {
    hub_port_status_t ps;
    if (hub_get_port_status(priv->dev, port, &ps) < 0) return;

    if (!(ps.wPortChange & HUB_PORT_CHG_CONNECTION)) goto clear_rest;

    hub_clear_port_feature(priv->dev, port, C_PORT_CONNECTION);

    if (ps.wPortStatus & HUB_PORT_STS_CONNECTION) {
        hub_set_port_feature(priv->dev, port, PORT_POWER);
        for (volatile uint32_t i = 0; i < 5000000; i++)
            __asm__ __volatile__("pause");

        if (hub_port_reset(priv->dev, port) != 0) {
            kprint("[HUB] Port reset fail port="); kprint_hex(port); kprint("\n");
            return;
        }

        hub_get_port_status(priv->dev, port, &ps);
        uint8_t speed = (ps.wPortStatus & HUB_PORT_STS_LOW_SPEED)
                      ? USB_SPEED_LOW : USB_SPEED_FULL;

        kprint("[HUB] Connect port="); kprint_hex(port);
        kprint(" speed="); kprint_hex(speed); kprint("\n");

        hub_hc_wrapper_t *wrap = (hub_hc_wrapper_t *)
            kmalloc(sizeof(hub_hc_wrapper_t));
        if (!wrap) return;
        memset(wrap, 0, sizeof(hub_hc_wrapper_t));
        wrap->hc_wrapper            = *priv->dev->hc;
        wrap->hc_wrapper.port_reset = hub_port_reset_wrapper;
        wrap->hub_dev               = priv->dev;
        wrap->hub_port              = port;

        usb_device_t *child = usb_device_enumerate(
            &wrap->hc_wrapper, port, speed);

        if (child) {
            child->hub = priv->dev;
            child->hc  = priv->dev->hc;
        }
        kfree_heap(wrap);

    } else {
        kprint("[HUB] Disconnect port="); kprint_hex(port); kprint("\n");
        usb_device_disconnect(priv->dev->hc, port);
    }

clear_rest:
    if (ps.wPortChange & HUB_PORT_CHG_RESET)
        hub_clear_port_feature(priv->dev, port, C_PORT_RESET);
}

static void hub_irq_notify(usb_device_t *dev, void *buf,
                            uint16_t len, void *priv_ptr)
{
    usb_hub_priv_t *priv = (usb_hub_priv_t *)priv_ptr;
    uint8_t *mask = (uint8_t *)buf;

    for (uint8_t p = 1; p <= priv->num_ports; p++) {
        uint8_t byte = p / 8, bit = p % 8;
        if (byte >= len) break;
        if (mask[byte] & (1 << bit))
            hub_handle_port(priv, p);
    }
    (void)dev;
}

static int hub_probe(usb_device_t *dev) {
    kprint("[HUB] Hub detected\n");

    usb_hub_priv_t *priv = (usb_hub_priv_t *)kmalloc(sizeof(usb_hub_priv_t));
    if (!priv) return -1;
    memset(priv, 0, sizeof(usb_hub_priv_t));
    priv->dev = dev;

    if (hub_get_descriptor(dev, &priv->desc) < 0) {
        kprint("[HUB] Failed to get hub descriptor\n");
        kfree_heap(priv); return -1;
    }
    priv->num_ports = priv->desc.bNbrPorts;
    if (priv->num_ports > USB_MAX_PORTS)
        priv->num_ports = USB_MAX_PORTS;

    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->ep[i].direction     == USB_DIR_IN &&
            dev->ep[i].transfer_type == USB_TRANSFER_INTERRUPT) {
            priv->intr_ep = dev->ep[i].address;
            break;
        }
    }

    for (uint8_t p = 1; p <= priv->num_ports; p++)
        hub_set_port_feature(dev, p, PORT_POWER);

    uint32_t ms = priv->desc.bPwrOn2PwrGood * 2;
    for (volatile uint32_t i = 0; i < ms * 100000; i++)
        __asm__ __volatile__("pause");

    dev->driver_priv = priv;

    for (uint8_t p = 1; p <= priv->num_ports; p++)
        hub_handle_port(priv, p);

    if (priv->intr_ep) {
        int rc = xhci_register_interrupt_ep(dev->hc, dev,
                                             priv->intr_ep,
                                             priv->status_buf,
                                             sizeof(priv->status_buf),
                                             hub_irq_notify, priv);
        if (rc != 0)
            kprint("[HUB] Failed to register interrupt EP\n");
    }

    kprint("[HUB] Ports="); kprint_hex(priv->num_ports); kprint("\n");
    return 0;
}

static void hub_remove(usb_device_t *dev) {
    if (dev && dev->driver_priv) {
        kfree_heap(dev->driver_priv);
        dev->driver_priv = NULL;
    }
}


static usb_driver_t hub_driver = {
    .name       = "usb_hub",
    .class_code = USB_CLASS_HUB,
    .subclass   = 0xFF,
    .protocol   = 0xFF,
    .probe      = hub_probe,
    .remove     = hub_remove,
    .next       = NULL
};

void usb_hub_init(void) {
    usb_driver_register(&hub_driver);
}