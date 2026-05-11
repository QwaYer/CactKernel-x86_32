#include "usb_hid.h"
#include "usb.h"
#include "xhci.h"
#include "kernel.h"
#include "task.h"
#include "mouse.h"
#include "memory.h"
#include "klib.h"

/* HID usage IDs for Ctrl-combo keys */
#define HID_KEY_C      0x06   /* 'c'  — Ctrl-C → SIGINT  */
#define HID_KEY_BSLASH 0x31   /* '\'  — Ctrl-\ → SIGQUIT */


volatile char usb_last_char    = 0;
volatile int  usb_key_event    = 0;
volatile int  usb_mouse_dx     = 0;
volatile int  usb_mouse_dy     = 0;
volatile int  usb_mouse_buttons = 0;

static const char hid_keymap[0x80] = {
    0,0,0,0,
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    '\n',0x1B,'\b','\t',' ','-','=','[',']','\\',0,';','\'','`',',','.','/',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char hid_keymap_shift[0x80] = {
    0,0,0,0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n',0x1B,'\b','\t',' ','_','+','{','}','|',0,':','"','~','<','>','?',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

#define HID_KEY_CAPSLOCK 0x39

static void hid_process_keyboard(hid_priv_t *priv, hid_kbd_report_t *rep) {
    uint8_t shift = (rep->modifier & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) ? 1 : 0;
    uint8_t ctrl  = (rep->modifier & (HID_MOD_LCTRL  | HID_MOD_RCTRL))  ? 1 : 0;

    for (int i = 0; i < 6; i++) {
        if (rep->keycode[i] != HID_KEY_CAPSLOCK) continue;
        int already = 0;
        for (int j = 0; j < 6; j++)
            if (priv->prev_kbd.keycode[j] == HID_KEY_CAPSLOCK) { already = 1; break; }
        if (!already) priv->caps_lock ^= 1;
    }

    for (int i = 0; i < 6; i++) {
        uint8_t kc = rep->keycode[i];
        if (!kc || kc >= 0x80) continue;

        int already = 0;
        for (int j = 0; j < 6; j++)
            if (priv->prev_kbd.keycode[j] == kc) { already = 1; break; }
        if (already) continue;

        /* TTY-level signal generation: Ctrl-C → SIGINT, Ctrl-\ → SIGQUIT */
        if (ctrl) {
            uint32_t fg = terminal_fg_pid;
            if (fg) {
                if (kc == HID_KEY_C) {
                    task_signal(fg, SIGINT);
                    continue;
                }
                if (kc == HID_KEY_BSLASH) {
                    task_signal(fg, SIGQUIT);
                    continue;
                }
            }
        }

        int use_shift = shift;
        if (kc >= 0x04 && kc <= 0x1D)
            use_shift = shift ^ priv->caps_lock;

        char c = use_shift ? hid_keymap_shift[kc] : hid_keymap[kc];
        if (!c) continue;

        usb_last_char = c;
        usb_key_event = 1;

        extern volatile char last_char;
        extern volatile int  key_event_happened;
        last_char          = c;
        key_event_happened = 1;
    }

    priv->prev_kbd = *rep;
}

static void hid_process_mouse(hid_mouse_report_t *rep) {
    extern int mouse_x, mouse_y, mouse_buttons;
    extern uint32_t fb_get_width(void);
    extern uint32_t fb_get_height(void);

    usb_mouse_dx      = rep->x;
    usb_mouse_dy      = rep->y;
    usb_mouse_buttons = rep->buttons & 0x07;

    mouse_x += rep->x;
    mouse_y -= rep->y;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= (int)fb_get_width())  mouse_x = (int)fb_get_width()  - 1;
    if (mouse_y >= (int)fb_get_height()) mouse_y = (int)fb_get_height() - 1;
    mouse_buttons = usb_mouse_buttons;
}

static void hid_process_tablet(hid_tablet_report_t *rep) {
    extern int mouse_x, mouse_y, mouse_buttons;
    extern uint32_t fb_get_width(void);
    extern uint32_t fb_get_height(void);

    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    mouse_x = (int)((uint32_t)rep->x * w / 32768);
    mouse_y = (int)((uint32_t)rep->y * h / 32768);
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= (int)w) mouse_x = (int)w - 1;
    if (mouse_y >= (int)h) mouse_y = (int)h - 1;

    mouse_buttons      = rep->buttons & 0x07;
    usb_mouse_buttons  = mouse_buttons;
    usb_mouse_dx       = 0;
    usb_mouse_dy       = 0;
}


static void hid_irq_notify(usb_device_t *dev, void *buf,
                             uint16_t len, void *priv_ptr)
{
    hid_priv_t *priv = (hid_priv_t *)priv_ptr;

    if (priv->type == HID_TYPE_KEYBOARD && len >= (uint16_t)sizeof(hid_kbd_report_t)) {
        hid_process_keyboard(priv, (hid_kbd_report_t *)buf);
    } else if (priv->type == HID_TYPE_MOUSE && len >= 3) {
        hid_process_mouse((hid_mouse_report_t *)buf);
    } else if (priv->type == HID_TYPE_TABLET && len >= (uint16_t)sizeof(hid_tablet_report_t)) {
        hid_process_tablet((hid_tablet_report_t *)buf);
    }
    (void)dev;
}

static int hid_set_protocol(usb_device_t *dev, uint8_t iface, uint8_t proto) {
    usb_setup_pkt_t setup = {
        .bmRequestType = USB_RT_HOST_TO_DEV | USB_RT_CLASS | USB_RT_INTERFACE,
        .bRequest      = HID_REQ_SET_PROTOCOL,
        .wValue        = proto,
        .wIndex        = iface,
        .wLength       = 0
    };
    return dev->hc->control_transfer(dev->hc, dev, &setup, NULL, 0);
}

static int hid_set_idle(usb_device_t *dev, uint8_t iface) {
    usb_setup_pkt_t setup = {
        .bmRequestType = USB_RT_HOST_TO_DEV | USB_RT_CLASS | USB_RT_INTERFACE,
        .bRequest      = HID_REQ_SET_IDLE,
        .wValue        = 0,
        .wIndex        = iface,
        .wLength       = 0
    };
    return dev->hc->control_transfer(dev->hc, dev, &setup, NULL, 0);
}

static int hid_probe(usb_device_t *dev) {
    hid_type_t type = HID_TYPE_UNKNOWN;

    if (dev->subclass == USB_HID_SUBCLASS_BOOT) {
        if      (dev->protocol == USB_HID_PROTOCOL_KBD)   type = HID_TYPE_KEYBOARD;
        else if (dev->protocol == USB_HID_PROTOCOL_MOUSE)  type = HID_TYPE_MOUSE;
    }

    if (type == HID_TYPE_UNKNOWN && dev->class_code == USB_CLASS_HID) {
        for (int i = 0; i < dev->ep_count; i++) {
            if (dev->ep[i].direction     == USB_DIR_IN &&
                dev->ep[i].transfer_type == USB_TRANSFER_INTERRUPT &&
                dev->ep[i].max_packet    >= 6) {
                type = HID_TYPE_TABLET;
                break;
            }
        }
    }

    if (type == HID_TYPE_UNKNOWN) {
        klog(LOG_WARN, "USB HID: unsupported protocol");
        return -1;
    }


    hid_priv_t *priv = (hid_priv_t *)kmalloc(sizeof(hid_priv_t));
    if (!priv) return -1;
    memset(priv, 0, sizeof(hid_priv_t));
    priv->type = type;

    for (int i = 0; i < dev->ep_count; i++) {
        if (dev->ep[i].direction     == USB_DIR_IN &&
            dev->ep[i].transfer_type == USB_TRANSFER_INTERRUPT) {
            priv->intr_ep = dev->ep[i].address;
            break;
        }
    }
    if (!priv->intr_ep) {
        klog(LOG_WARN, "USB HID: interrupt IN endpoint not found");
        kfree_heap(priv);
        return -1;
    }

    uint8_t *p   = dev->config_buf;
    uint8_t *end = p + dev->config_len;
    while (p < end) {
        if (p[0] >= 9 && p[1] == USB_DESC_INTERFACE) {
            usb_iface_desc_t *iface = (usb_iface_desc_t *)p;
            if (iface->bInterfaceClass    == USB_CLASS_HID &&
                iface->bInterfaceSubClass == dev->subclass  &&
                iface->bInterfaceProtocol == dev->protocol) {
                priv->iface_num = iface->bInterfaceNumber;
                break;
            }
        }
        if (!p[0]) break;
        p += p[0];
    }

    hid_set_protocol(dev, priv->iface_num, HID_PROTO_BOOT);
    hid_set_idle(dev, priv->iface_num);

    dev->driver_priv = priv;

    uint16_t report_len;
    void    *report_buf;
    if (type == HID_TYPE_KEYBOARD) {
        report_len = sizeof(hid_kbd_report_t);
        report_buf = (void *)&priv->report_buf.kbd;
    } else if (type == HID_TYPE_TABLET) {
        report_len = sizeof(hid_tablet_report_t);
        report_buf = (void *)&priv->report_buf.tablet;
    } else {
        report_len = sizeof(hid_mouse_report_t);
        report_buf = (void *)&priv->report_buf.mouse;
    }

    int rc = xhci_register_interrupt_ep(dev->hc, dev,
                                         priv->intr_ep,
                                         report_buf, report_len,
                                         hid_irq_notify, priv);

    if (rc != 0) {
        klog(LOG_WARN, "USB HID: interrupt transfer registration failed");
        kfree_heap(priv);
        dev->driver_priv = NULL;
        return -1;
    }

    return 0;
}


static void hid_remove(usb_device_t *dev) {
    if (dev && dev->driver_priv) {
        kfree_heap(dev->driver_priv);
        dev->driver_priv = NULL;
    }
}

static usb_driver_t hid_driver = {
    .name       = "usb_hid",
    .class_code = USB_CLASS_HID,
    .subclass   = 0xFF,
    .protocol   = 0xFF,
    .probe      = hid_probe,
    .remove     = hid_remove,
    .next       = NULL
};

void usb_hid_init(void) {
    usb_driver_register(&hid_driver);
}