#include "usb_hub.h"
#include "usb.h"
#include "xhci.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "task.h"

extern void sched_sleep_ticks(uint32_t ticks);

/* Every hub this driver has claimed; usb_hub_task() services their port
 * changes.  Appended on probe. */
static usb_hub_priv_t *hub_list;


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
            printk("[HUB] Port reset fail port="); printk_hex(port); printk("\n");
            return;
        }

        hub_get_port_status(priv->dev, port, &ps);
        uint8_t speed;
        if (ps.wPortStatus & HUB_PORT_STS_LOW_SPEED)
            speed = USB_SPEED_LOW;
        else if (ps.wPortStatus & HUB_PORT_STS_HIGH_SPEED)
            speed = USB_SPEED_HIGH;
        else if (priv->dev->speed == USB_SPEED_SUPER
                 || priv->dev->speed == USB_SPEED_SUPER_PLUS)
            /* A SuperSpeed hub's downstream port has no USB2 speed bits set and
             * the device on its SS lane is SuperSpeed. */
            speed = USB_SPEED_SUPER;
        else
            speed = USB_SPEED_FULL;

        printk("[HUB] Connect port="); printk_hex(port);
        printk(" speed="); printk_hex(speed); printk("\n");

        usb_device_t *child;
        if (priv->dev->hc->enumerate_hub_child) {
            /* The controller assigns the address itself (xHCI: Enable Slot +
             * Address Device with the hub topology in the route string), so
             * the generic SET_ADDRESS path below cannot be used. */
            child = priv->dev->hc->enumerate_hub_child(priv->dev->hc,
                                                       priv->dev, port, speed);
        } else {
            hub_hc_wrapper_t *wrap = (hub_hc_wrapper_t *)
                kmalloc(sizeof(hub_hc_wrapper_t));
            if (!wrap) {
                pr_err("[HUB] wrapper alloc failed for port %u\n", (unsigned)port);
                return;
            }
            memset(wrap, 0, sizeof(hub_hc_wrapper_t));
            wrap->hc_wrapper            = *priv->dev->hc;
            wrap->hc_wrapper.port_reset = hub_port_reset_wrapper;
            wrap->hub_dev               = priv->dev;
            wrap->hub_port              = port;

            child = usb_device_enumerate(&wrap->hc_wrapper, port, speed);

            if (child) {
                child->hub = priv->dev;
                child->hc  = priv->dev->hc;
            }
            kfree(wrap);
        }

    } else {
        printk("[HUB] Disconnect port="); printk_hex(port); printk("\n");
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
    if (priv->removed) return;
    uint8_t *mask = (uint8_t *)buf;
    uint32_t ev = 0;

    /* This runs in the controller's event-drain context with its event lock
     * held, so it must not issue any transfer: just latch which ports the hub
     * flagged and let usb_hub_task() do the reset/enumeration in task
     * context.  Doing the work here would re-enter the event lock (the port
     * reset and enumeration are control transfers/commands) and wedge. */
    for (uint8_t p = 1; p <= priv->num_ports && p < 32; p++) {
        uint8_t byte = p / 8, bit = p % 8;
        if (byte >= len) break;
        if (mask[byte] & (1 << bit))
            ev |= 1u << p;
    }
    if (ev)
        __sync_fetch_and_or(&priv->port_events, ev);
    (void)dev;
}

static void usb_hub_task(void) {
    while (1) {
        for (usb_hub_priv_t *h = hub_list; h; h = h->next) {
            if (h->removed)
                continue;
            uint32_t ev = __sync_lock_test_and_set(&h->port_events, 0);
            while (ev) {
                uint8_t p = (uint8_t)__builtin_ctz(ev);
                ev &= ~(1u << p);
                hub_handle_port(h, p);
            }
        }
        sched_sleep_ticks(1);
    }
}

void usb_hub_hotplug_init(void) {
    if (!create_task(usb_hub_task))
        pr_warn("[HUB] hub hotplug task could not be created\n");
    else
        pr_info("  %-11s : hub port-change task up\n", "usb-hub");
}

static int hub_probe(usb_device_t *dev) {
    pr_info("[HUB] Hub detected\n");

    usb_hub_priv_t *priv = (usb_hub_priv_t *)kmalloc(sizeof(usb_hub_priv_t));
    if (!priv) {
        pr_err("[HUB] hub private data alloc failed\n");
        return -1;
    }
    memset(priv, 0, sizeof(usb_hub_priv_t));
    priv->dev = dev;

    if (hub_get_descriptor(dev, &priv->desc) < 0) {
        pr_err("[HUB] Failed to get hub descriptor\n");
        kfree(priv); return -1;
    }
    priv->num_ports = priv->desc.bNbrPorts;
    if (priv->num_ports > USB_MAX_PORTS)
        priv->num_ports = USB_MAX_PORTS;

    /* Mark the device as a hub in the controller before touching its ports
     * (xHCI needs the Hub bit / port count in the slot context). */
    if (dev->hc->update_hub)
        dev->hc->update_hub(dev->hc, dev, priv->num_ports);

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
            pr_warn("[HUB] Failed to register interrupt EP\n");
    }

    priv->next = hub_list;
    hub_list   = priv;

    printk("[HUB] Ports="); printk_hex(priv->num_ports); printk("\n");
    return 0;
}

static void hub_remove(usb_device_t *dev) {
    if (dev && dev->driver_priv) {
        usb_hub_priv_t *priv = (usb_hub_priv_t *)dev->driver_priv;
        /* Mark it dead and leave it linked: usb_hub_task() may be iterating the
         * list right now, so freeing here would be a use-after-free.  An
         * unplugged hub leaks this small struct rather than risk that. */
        priv->removed = 1;
        __sync_synchronize();
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
    pr_info("  %-11s : hub driver registered\n", "usb-hub");
}