#include "usb_hid.h"
#include "usb.h"
#include "xhci.h"
#include "kernel.h"
#include "task.h"
#include "mouse.h"
#include "memory.h"
#include "klib.h"
#include "keyboard.h"
#include "tty.h"
#include "sync.h"
#include "tick.h"

extern void sched_sleep_ticks(uint32_t ticks);

/* Ctrl+<key> → the control byte Linux delivers on that combination (0 = no
 * mapping).  The byte is handed to the tty line discipline; whether it turns
 * into a signal (VINTR/VQUIT/VSUSP) or a raw byte for the reader is decided
 * there, because only the terminal knows if the reader is in canonical mode. */
static const char hid_ctrl_map[0x80] = {
    [0x04] = 0x01, [0x05] = 0x02, [0x06] = 0x03, [0x07] = 0x04,   /* ^A..^D */
    [0x08] = 0x05, [0x09] = 0x06, [0x0A] = 0x07, [0x0B] = 0x08,   /* ^E..^H */
    [0x0C] = 0x09, [0x0D] = 0x0A, [0x0E] = 0x0B, [0x0F] = 0x0C,   /* ^I..^L */
    [0x10] = 0x0D, [0x11] = 0x0E, [0x12] = 0x0F, [0x13] = 0x10,   /* ^M..^P */
    [0x14] = 0x11, [0x15] = 0x12, [0x16] = 0x13, [0x17] = 0x14,   /* ^Q..^T */
    [0x18] = 0x15, [0x19] = 0x16, [0x1A] = 0x17, [0x1B] = 0x18,   /* ^U..^X */
    [0x1C] = 0x19, [0x1D] = 0x1A,                                 /* ^Y..^Z */
    [0x1F] = 0x00,   /* ^2 / ^@ → NUL */
    [0x23] = 0x1E,   /* ^6 / ^^ → RS  */
    [0x2C] = 0x00,   /* ^Space  → NUL */
    [0x2D] = 0x1F,   /* ^- / ^_ → US  */
    [0x2F] = 0x1B,   /* ^[      → ESC */
    [0x30] = 0x1D,   /* ^]      → GS  */
    [0x31] = 0x1C,   /* ^\      → FS  */
    [0x38] = 0x7F,   /* ^/ / ^? → DEL */
};

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

/* Extended (non-character) keys, emitted as the xterm CSI sequences every
 * terminal program already parses — cactsole's readline handles exactly these,
 * and full-screen tools (ced) rely on them for navigation.  HID 0x49..0x52. */
static const char *const hid_ext_keymap[0x80] = {
    [0x49] = "\033[2~",   /* Insert   */
    [0x4A] = "\033[H",    /* Home     */
    [0x4B] = "\033[5~",   /* PageUp   */
    [0x4C] = "\033[3~",   /* Delete   */
    [0x4D] = "\033[F",    /* End      */
    [0x4E] = "\033[6~",   /* PageDown */
    [0x4F] = "\033[C",    /* Right    */
    [0x50] = "\033[D",    /* Left     */
    [0x51] = "\033[B",    /* Down     */
    [0x52] = "\033[A",    /* Up       */
};

/* ── Software key repeat ──────────────────────────────────────────────────
 * The keyboard is told SET_IDLE(0), so it reports only when its report
 * changes: a held key produces exactly one report and then silence, and only
 * a device-side repeat could produce more (SET_IDLE with a duration is the
 * other way to get them, but it makes the keyboard talk every few
 * milliseconds for as long as it is plugged in).  The repeat is generated
 * here instead, which works the same on every keyboard and costs no USB
 * traffic while nothing is held.
 *
 * The report handler records what the last newly-pressed key emitted; the
 * task below walks those bytes back out on the 100 Hz tick and stops as soon
 * as a report no longer contains that key.  Only keys that emit something
 * repeat: Caps Lock, the control combos and Alt+F<n> turn the repeat off
 * rather than repeating the key before them. */

#define HID_REPEAT_DELAY_TICKS 25   /* 250 ms hold before the first repeat  */
#define HID_REPEAT_RATE_TICKS   3   /* then one every 30 ms                 */
#define HID_REPEAT_IDLE_TICKS   4   /* task tick when nothing is repeating  */
#define HID_REPEAT_SEQ_MAX      4   /* "\033[3~" is the longest unit        */

static irq_spinlock_t hid_repeat_lock;
static struct {
    hid_priv_t *owner;              /* keyboard whose key is down           */
    uint8_t     kc;                 /* HID usage, matched against reports   */
    uint8_t     out_len;
    char        out[HID_REPEAT_SEQ_MAX];
    uint32_t    due_tick;           /* when the next repeat is owed         */
} hid_repeat;

/* Start repeating what the key that just went down emitted. */
static void hid_repeat_arm(hid_priv_t *priv, uint8_t kc,
                           const char *out, uint8_t out_len) {
    if (out_len > HID_REPEAT_SEQ_MAX)
        out_len = HID_REPEAT_SEQ_MAX;

    irq_spinlock_acquire(&hid_repeat_lock);
    hid_repeat.owner   = priv;
    hid_repeat.kc      = kc;
    hid_repeat.out_len = out_len;
    for (uint8_t i = 0; i < out_len; i++)
        hid_repeat.out[i] = out[i];
    hid_repeat.due_tick = timer_ticks_get() + HID_REPEAT_DELAY_TICKS;
    irq_spinlock_release(&hid_repeat_lock);
}

/* A newly pressed key takes the repeat over: whatever was repeating stops. */
static void hid_repeat_clear(void) {
    irq_spinlock_acquire(&hid_repeat_lock);
    hid_repeat.owner = NULL;
    irq_spinlock_release(&hid_repeat_lock);
}

/* Forget a keyboard that is going away, so the repeat can never outlive the
 * device its state points into. */
static void hid_repeat_forget(hid_priv_t *priv) {
    irq_spinlock_acquire(&hid_repeat_lock);
    if (hid_repeat.owner == priv)
        hid_repeat.owner = NULL;
    irq_spinlock_release(&hid_repeat_lock);
}

/* The repeating key is still held only while it is still in the report; its
 * release (or a roll-over onto another key) ends the repeat. */
static void hid_repeat_sync(hid_priv_t *priv, const hid_kbd_report_t *rep) {
    irq_spinlock_acquire(&hid_repeat_lock);
    if (hid_repeat.owner == priv) {
        int held = 0;
        for (int i = 0; i < 6; i++)
            if (rep->keycode[i] == hid_repeat.kc) { held = 1; break; }
        if (!held)
            hid_repeat.owner = NULL;
    }
    irq_spinlock_release(&hid_repeat_lock);
}

/* One pass per repeat due.  It sleeps exactly as long as the next repeat is
 * away, so an idle keyboard costs a wake-up every 40 ms and a stationary
 * held key never misses its slot. */
static void hid_repeat_task(void) {
    while (1) {
        uint32_t wait = HID_REPEAT_IDLE_TICKS;
        uint32_t now;

        irq_spinlock_acquire(&hid_repeat_lock);
        now = timer_ticks_get();
        if (hid_repeat.owner && hid_repeat.out_len) {
            int32_t left = (int32_t)(hid_repeat.due_tick - now);
            if (left <= 0) {
                /* One character per overdue tick at most: a task that was
                 * held off for a while should carry on repeating, not dump a
                 * burst of characters into the console. */
                for (uint8_t i = 0; i < hid_repeat.out_len; i++)
                    keyboard_post_key(hid_repeat.out[i]);
                hid_repeat.due_tick = now + HID_REPEAT_RATE_TICKS;
                wait = HID_REPEAT_RATE_TICKS;
            } else {
                wait = (uint32_t)left;
            }
        }
        irq_spinlock_release(&hid_repeat_lock);

        sched_sleep_ticks(wait);
    }
}

/* Spawned from the boot sequence once the scheduler is live: usb_init() runs
 * before task_init(), and a task created earlier is wiped with the task
 * list. */
void usb_hid_repeat_init(void) {
    if (!create_task(hid_repeat_task)) {
        pr_warn("USB HID: key repeat task could not be created");
        return;
    }
    pr_info("  %-11s : key repeat up (%u ms delay, %u ms rate)\n", "usb-hid",
            (unsigned)(HID_REPEAT_DELAY_TICKS * 10),
            (unsigned)(HID_REPEAT_RATE_TICKS * 10));
}

static void hid_post_sequence(hid_priv_t *priv, uint8_t kc, const char *seq) {
    char    last = 0;
    uint8_t len  = 0;
    for (const char *q = seq; *q; q++) {
        keyboard_post_key(*q);
        last = *q;
        if (len < HID_REPEAT_SEQ_MAX)
            len++;
    }
    usb_last_char  = last;
    usb_key_event  = 1;
    hid_repeat_arm(priv, kc, seq, len);
}

static void hid_process_keyboard(hid_priv_t *priv, hid_kbd_report_t *rep) {
    uint8_t shift = (rep->modifier & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) ? 1 : 0;
    uint8_t ctrl  = (rep->modifier & (HID_MOD_LCTRL  | HID_MOD_RCTRL))  ? 1 : 0;
    uint8_t alt   = (rep->modifier & (HID_MOD_LALT   | HID_MOD_RALT))   ? 1 : 0;

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

        /* This key becomes the one that repeats.  The paths below that emit
         * nothing (Caps Lock, the control combos, Alt+F<n>) leave the repeat
         * off instead of keeping the previous key repeating under them. */
        hid_repeat_clear();

        /* Alt+F1..F12 -> switch virtual terminal, as on a Linux console.
         * HID 0x3A..0x45 are F1..F12; tty_activate() ignores VTs that do not
         * exist, so the higher F-keys are simply no-ops. */
        if (alt && kc >= 0x3A && kc <= 0x45) {
            tty_activate(kc - 0x3A + 1);
            continue;
        }

        /* Ctrl+<key> → the control byte Linux delivers (^A..^Z, ^[, ^\, ^], ^^,
         * ^_, ^@, ^?).  Delivering the byte — rather than signalling here — is
         * what lets the tty line discipline choose: in canonical mode it turns
         * VINTR/VQUIT/VSUSP into signals, in raw mode the byte reaches the
         * reader (cactsole's readline handles ^C itself). */
        if (ctrl && hid_ctrl_map[kc]) {
            char cb = hid_ctrl_map[kc];
            keyboard_post_key(cb);
            usb_last_char = cb;
            usb_key_event = 1;
            last_char     = cb;
            key_event_happened = 1;
            continue;
        }

        /* Arrows / Home / End / PgUp / PgDn / Del / Ins → xterm CSI bytes. */
        if (!ctrl && hid_ext_keymap[kc]) {
            hid_post_sequence(priv, kc, hid_ext_keymap[kc]);
            continue;
        }

        int use_shift = shift;
        if (kc >= 0x04 && kc <= 0x1D)
            use_shift = shift ^ priv->caps_lock;

        char c = use_shift ? hid_keymap_shift[kc] : hid_keymap[kc];
        if (!c) continue;

        keyboard_post_key(c);
        hid_repeat_arm(priv, kc, &c, 1);

        usb_last_char = c;
        usb_key_event = 1;

        extern volatile char last_char;
        extern volatile int  key_event_happened;
        last_char          = c;
        key_event_happened = 1;
    }

    hid_repeat_sync(priv, rep);
    priv->prev_kbd = *rep;
}

static void hid_process_mouse(hid_mouse_report_t *rep) {
    extern volatile int mouse_x, mouse_y, mouse_buttons;
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
    extern volatile int mouse_x, mouse_y, mouse_buttons;
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

    if (priv->removed) return;
    __sync_synchronize();

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
        pr_warn("USB HID: unsupported protocol");
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
        pr_warn("USB HID: interrupt IN endpoint not found");
        kfree(priv);
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
        pr_warn("USB HID: interrupt transfer registration failed");
        kfree(priv);
        dev->driver_priv = NULL;
        return -1;
    }

    return 0;
}


static void hid_remove(usb_device_t *dev) {
    if (dev && dev->driver_priv) {
        hid_priv_t *priv = (hid_priv_t *)dev->driver_priv;
        priv->removed = 1;
        __sync_synchronize();
        hid_repeat_forget(priv);
        kfree(priv);
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
    /* The repeat lock must exist before the first keyboard report; the task
     * that drains it is spawned from the boot sequence (usb_hid_repeat_init)
     * because it needs the scheduler. */
    irq_spinlock_init(&hid_repeat_lock);
    usb_driver_register(&hid_driver);
}