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
    [0x32] = 0x1C,   /* the ISO \| key is 0x32 (see hid_keymap) → FS too */
    [0x38] = 0x7F,   /* ^/ / ^? → DEL */
};

volatile char usb_last_char    = 0;
volatile int  usb_key_event    = 0;
volatile int  usb_mouse_dx     = 0;
volatile int  usb_mouse_dy     = 0;
volatile int  usb_mouse_buttons = 0;

/* US layout.  HID 0x32 ("Keyboard Non-US # and ~") is the position an ISO
 * keyboard — including Russian ones — puts its backslash/bar key in, the one
 * an ANSI board reports as 0x31, so it emits \ and | too.  Linux maps both
 * usages to KEY_BACKSLASH; QEMU's usb-kbd has no key at 0x32, so getting this
 * wrong is invisible there and shows up as '#' on real hardware.  '#' and '~'
 * stay reachable as Shift+3 and Shift+` . */
static const char hid_keymap[0x80] = {
    0,0,0,0,
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    '\n',0x1B,'\b','\t',' ','-','=','[',']','\\','\\',';','\'','`',',','.','/',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,   /* F1-F12 (hid_ext_keymap), ScrollLock, Pause, Ins..Down, NumLock */
    '/','*','-','+','\n','1','2','3','4','5','6','7','8','9','0','.',   /* keypad */
    '\\'                                                      /* non-US \ and | */
};

static const char hid_keymap_shift[0x80] = {
    0,0,0,0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n',0x1B,'\b','\t',' ','_','+','{','}','|','|',':','"','~','<','>','?',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,   /* F1-F12 (hid_ext_keymap), ScrollLock, Pause, Ins..Down, NumLock */
    '/','*','-','+','\n','1','2','3','4','5','6','7','8','9','0','.',   /* keypad */
    '|'                                                       /* non-US \ and | */
};

#define HID_KEY_CAPSLOCK   0x39
#define HID_KEY_SCROLLLOCK 0x47
#define HID_KEY_NUMLOCK    0x53

/* Extended (non-character) keys, emitted as the xterm CSI sequences every
 * terminal program already parses — cactsole's readline handles exactly these,
 * and full-screen tools (ced) rely on them for navigation.  HID 0x3A..0x45
 * (F1..F12, which reach this table only with Shift held: bare/Alt/Ctrl+Alt
 * F<n> switch VT earlier) and HID 0x49..0x52.  The F-keys use the rxvt/Linux
 * numbering (11~..15~, 17~..21~, 23~, 24~) rather than xterm's SS3 for
 * F1..F4, so they stay in the same ESC[<n>~ family as Insert/Delete/PgUp/PgDn. */
static const char *const hid_ext_keymap[0x80] = {
    [0x3A] = "\033[11~",  /* F1       */
    [0x3B] = "\033[12~",  /* F2       */
    [0x3C] = "\033[13~",  /* F3       */
    [0x3D] = "\033[14~",  /* F4       */
    [0x3E] = "\033[15~",  /* F5       */
    [0x3F] = "\033[17~",  /* F6       */
    [0x40] = "\033[18~",  /* F7       */
    [0x41] = "\033[19~",  /* F8       */
    [0x42] = "\033[20~",  /* F9       */
    [0x43] = "\033[21~",  /* F10      */
    [0x44] = "\033[23~",  /* F11      */
    [0x45] = "\033[24~",  /* F12      */
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
#define HID_REPEAT_SEQ_MAX      5   /* "\033[11~" is the longest unit        */

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
        pr_warn("usb-hid: key repeat task could not be created");
        return;
    }
    pr_info("%s: key repeat up (%u ms delay, %u ms rate)\n", "usb-hid",
            (unsigned)(HID_REPEAT_DELAY_TICKS * 10),
            (unsigned)(HID_REPEAT_RATE_TICKS * 10));
}

/* ── Keyboard indicator LEDs (Num / Caps / Scroll) ────────────────────────
 * The report handler runs inside the controller's event drain, often in
 * interrupt context, where the SET_REPORT control transfer that programs the
 * LEDs may not run: it sleeps and re-enters the event lock.  So the handler
 * only leaves the wanted LED byte in a one-slot mailbox and hid_led_task() —
 * spawned after the scheduler is live, like the key-repeat task — sends it.
 * The task never touches the keyboard's hid_priv_t, so that struct can be
 * freed normally when the device is unplugged. */

static irq_spinlock_t hid_led_lock;
static struct {
    usb_device_t *dev;       /* keyboard owing an LED update, or NULL */
    uint8_t       iface;
    uint8_t       want;      /* full LED byte to send */
    uint8_t       pending;
} hid_led;

/* HID boot-protocol LED output report: one byte, bit0 Num Lock, bit1 Caps
 * Lock, bit2 Scroll Lock, report id 0. */
static int hid_set_leds(usb_device_t *dev, uint8_t iface, uint8_t leds) {
    usb_setup_pkt_t setup = {
        .bmRequestType = USB_RT_HOST_TO_DEV | USB_RT_CLASS | USB_RT_INTERFACE,
        .bRequest      = HID_REQ_SET_REPORT,
        .wValue        = (HID_REPORT_OUTPUT << 8) | 0,
        .wIndex        = iface,
        .wLength       = 1
    };
    return dev->hc->control_transfer(dev->hc, dev, &setup, &leds, 1);
}

/* Hand a wanted LED byte to the task.  Called from the report handler, so it
 * only takes the (non-sleeping) spinlock. */
static void hid_led_request(hid_priv_t *priv, uint8_t want) {
    irq_spinlock_acquire(&hid_led_lock);
    hid_led.dev     = priv->dev;
    hid_led.iface   = priv->iface_num;
    hid_led.want    = want;
    hid_led.pending = 1;
    irq_spinlock_release(&hid_led_lock);
}

static void hid_led_task(void) {
    while (1) {
        usb_device_t *dev = NULL;
        uint8_t iface = 0, want = 0;

        irq_spinlock_acquire(&hid_led_lock);
        if (hid_led.pending && hid_led.dev) {
            dev   = hid_led.dev;
            iface = hid_led.iface;
            want  = hid_led.want;
            hid_led.pending = 0;
        }
        irq_spinlock_release(&hid_led_lock);

        /* A lost update (a send that fails, or a second toggle within the
         * same 20 ms) is harmless: the next lock key re-sends the whole LED
         * byte, which always reflects the current state. */
        if (dev)
            hid_set_leds(dev, iface, want);

        sched_sleep_ticks(2);
    }
}

void usb_hid_led_init(void) {
    if (!create_task(hid_led_task)) {
        pr_warn("usb-hid: LED task could not be created");
        return;
    }
    pr_info("%s: keyboard indicator LEDs up\n", "usb-hid");
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

/* PrintScreen fallback: when the report was drained from a task instead of
 * delivered by the xHCI interrupt there is no entry frame to report, so the
 * driver dumps its own register set — the same dump, with less useful context.
 * ESP is handed over separately because this frame was not pushed by a stub,
 * and the selectors are moved as 16-bit values into a zeroed frame so their
 * upper halves stay clean the way the CPU pushes them. */
static void hid_dump_registers(void) {
    struct context_frame cf;
    uint32_t esp;

    memset(&cf, 0, sizeof(cf));

    /* One statement per register: a single block with a dozen memory outputs
     * runs clang out of address registers.  Nothing can run between these
     * statements (no clobbers, no dependencies), so they read one register
     * set.  pushfl/popl is balanced, so ESP is unchanged afterwards. */
    __asm__ __volatile__("movl %%eax, %0" : "=m"(cf.eax));
    __asm__ __volatile__("movl %%ebx, %0" : "=m"(cf.ebx));
    __asm__ __volatile__("movl %%ecx, %0" : "=m"(cf.ecx));
    __asm__ __volatile__("movl %%edx, %0" : "=m"(cf.edx));
    __asm__ __volatile__("movl %%esi, %0" : "=m"(cf.esi));
    __asm__ __volatile__("movl %%edi, %0" : "=m"(cf.edi));
    __asm__ __volatile__("movl %%ebp, %0" : "=m"(cf.ebp));
    __asm__ __volatile__("movl %%esp, %0" : "=m"(esp));
    __asm__ __volatile__("movw %%cs, %0"    : "=m"(cf.cs));
    __asm__ __volatile__("movw %%ds, %0"    : "=m"(cf.ds));
    __asm__ __volatile__("movw %%es, %0"    : "=m"(cf.es));
    __asm__ __volatile__("pushfl\n\tpopl %0" : "=m"(cf.eflags));
    __asm__ __volatile__("1: movl $1b, %0"   : "=m"(cf.eip));

    dump_registers("PrintScreen (own regs)", &cf, esp);
}

static void hid_process_keyboard(hid_priv_t *priv, hid_kbd_report_t *rep) {
    uint8_t shift = (rep->modifier & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) ? 1 : 0;
    uint8_t ctrl  = (rep->modifier & (HID_MOD_LCTRL  | HID_MOD_RCTRL))  ? 1 : 0;
    uint8_t alt   = (rep->modifier & (HID_MOD_LALT   | HID_MOD_RALT))   ? 1 : 0;

    /* Lock keys toggle on the press edge and drive the keyboard's indicator
     * LEDs.  They are only reported, not acted on, so Num/Scroll Lock change
     * nothing in the keymap (only Caps Lock does, further down). */
    for (int i = 0; i < 6; i++) {
        uint8_t lk = rep->keycode[i];
        if (lk != HID_KEY_CAPSLOCK && lk != HID_KEY_NUMLOCK
            && lk != HID_KEY_SCROLLLOCK)
            continue;
        int already = 0;
        for (int j = 0; j < 6; j++)
            if (priv->prev_kbd.keycode[j] == lk) { already = 1; break; }
        if (already) continue;

        if (lk == HID_KEY_CAPSLOCK)      priv->caps_lock   ^= 1;
        else if (lk == HID_KEY_NUMLOCK)  priv->num_lock    ^= 1;
        else                             priv->scroll_lock ^= 1;

        hid_led_request(priv,
              (priv->num_lock    ? HID_LED_NUM_LOCK    : 0)
            | (priv->caps_lock   ? HID_LED_CAPS_LOCK   : 0)
            | (priv->scroll_lock ? HID_LED_SCROLL_LOCK : 0));
    }

    for (int i = 0; i < 6; i++) {
        uint8_t kc = rep->keycode[i];
        if (!kc || kc >= 0x80) continue;

        int already = 0;
        for (int j = 0; j < 6; j++)
            if (priv->prev_kbd.keycode[j] == kc) { already = 1; break; }
        if (already) continue;

        /* This key becomes the one that repeats.  The paths below that emit
         * nothing (Caps Lock, the control combos, the VT-switching F-keys)
         * leave the repeat off instead of keeping the previous key repeating
         * under them. */
        hid_repeat_clear();

        /* F1..F12 -> switch virtual terminal.  HID 0x3A..0x45 are F1..F12.
         * Bare F<n>, Alt+F<n> and Ctrl+Alt+F<n> all switch, as on a Linux
         * console: the Ctrl+Alt form is the one a full-screen program cannot
         * swallow.  Shift+F<n> deliberately falls through to
         * hid_ext_keymap, so a tool that wants a function key still has one.
         * This runs in interrupt context, so the switch is queued and the VT
         * (allocated on demand, up to the cap) is brought up by the tty
         * console task. */
        if (kc >= 0x3A && kc <= 0x45 && !shift && (!ctrl || alt)) {
            tty_activate_deferred(kc - 0x3A + 1);
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

        /* Shift+Tab is back-tab (CBT) — the ESC[Z rxvt and xterm both send,
         * which is what the apps that accept a "previous field" key parse.
         * Plain Tab stays ^I through the keymap below. */
        if (kc == 0x2B && shift && !ctrl) {
            hid_post_sequence(priv, kc, "\033[Z");
            continue;
        }

        /* PrintScreen -> the Linux console's Show_Registers: the register set
         * and call trace of the context the interrupt interrupted.  A keypress
         * is delivered from the xHCI interrupt, so the MSI dispatcher has that
         * entry frame published; a report drained from a task instead (the poll
         * loop around a synchronous control transfer) has no interrupt behind
         * it and falls back to this handler's own registers.  SysRq
         * (Alt+PrintScreen in Linux) has no counterpart here, so every
         * PrintScreen press dumps. */
        if (kc == 0x46) {
            struct context_frame *irq = get_irq_regs();
            if (irq)
                dump_registers("PrintScreen", irq, context_frame_esp(irq));
            else
                hid_dump_registers();
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
        pr_warn("usb-hid: unsupported protocol");
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
        pr_warn("usb-hid: interrupt IN endpoint not found");
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
        pr_warn("usb-hid: interrupt transfer registration failed");
        kfree(priv);
        dev->driver_priv = NULL;
        return -1;
    }

    /* Keyboards have indicator LEDs; remember the device for the LED path and
     * set the initial state (all off).  This runs in task context, so the
     * SET_REPORT right here is fine. */
    if (type == HID_TYPE_KEYBOARD) {
        priv->dev = dev;
        hid_set_leds(dev, priv->iface_num, 0);
    }

    return 0;
}


static void hid_remove(usb_device_t *dev) {
    if (dev && dev->driver_priv) {
        hid_priv_t *priv = (hid_priv_t *)dev->driver_priv;
        priv->removed = 1;
        __sync_synchronize();
        hid_repeat_forget(priv);

        /* Drop a pending LED update for this device before it is freed.  The
         * task copies the device pointer out under this lock and sends
         * outside it, so it never dereferences hid_priv_t. */
        irq_spinlock_acquire(&hid_led_lock);
        if (hid_led.dev == dev) {
            hid_led.dev     = NULL;
            hid_led.pending = 0;
        }
        irq_spinlock_release(&hid_led_lock);

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
    /* The repeat and LED locks must exist before the first keyboard report;
     * the tasks that use them are spawned from the boot sequence
     * (usb_hid_repeat_init / usb_hid_led_init) because they need the
     * scheduler. */
    irq_spinlock_init(&hid_repeat_lock);
    irq_spinlock_init(&hid_led_lock);
    usb_driver_register(&hid_driver);
}