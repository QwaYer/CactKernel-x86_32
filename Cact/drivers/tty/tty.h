#ifndef TTY_H
#define TTY_H

#include <stdint.h>

/* Virtual terminals, exposed Unix-style as /dev/tty1 .. /dev/ttyN — but, as on
 * Linux, N is not fixed: VTs are allocated on demand up to the hard cap
 * TTY_MAX_VT and each allocation registers its own node (tty_alloc()).  The
 * tty driver owns that registration; devfs never hardcodes the family.
 *
 * /dev/tty0 follows Linux: it is an alias for whichever VT is currently
 * active, it is not a terminal of its own.  Everything below takes a VT
 * index where 0 means "the active VT" and 1..N names a concrete one. */

#define TTY_MAX_VT  63      /* hard cap, Linux MAX_NR_CONSOLES */
#define TTY_OUTBUF  4096    /* per-VT output scrollback, replayed on switch */

/* Register /dev/tty0 and the first VT; called once, early (from devfs_init()). */
void tty_init(void);

int  tty_count(void);               /* number of allocated VTs (1..tty_num) */
int  tty_active(void);              /* active VT, 1..tty_count() */
int  tty_alloc(void);               /* allocate the next VT and register /dev/tty<n>; n or -1 */
int  tty_free(int n);               /* release VT n (highest only, never the active one) */
int  tty_activate(int n);           /* switch the console to VT n, allocating it if needed */
int  tty_index(int idx);            /* resolve 0 -> active VT index */

/* Queue a VT switch from interrupt context (Alt+F<n>): the switch, and any VT
 * allocation it needs, is performed later in process context by the console
 * task, so a keystroke IRQ never registers a device node. */
void tty_activate_deferred(int n);

/* Spawn the console task that drains tty_activate_deferred() requests.  Call
 * once, after the scheduler is live (like usb_hid_repeat_init). */
void tty_console_task_init(void);

int  tty_read (int idx, uint32_t off, uint32_t size, char *buf);
int  tty_write(int idx, uint32_t off, uint32_t size, char *buf);
int  tty_ioctl(int idx, uint32_t cmd, void *arg);

/* Deliver a keystroke to a VT's input queue.  Called from the keyboard path
 * with idx == 0 so the byte lands on whichever VT is active at that moment —
 * that is what keeps input per-terminal instead of pooling it globally. */
void tty_input(int idx, char c);

/* Controlling terminal of the calling process: a VT index, or 0 when the
 * process has none (in which case tty_read/tty_write fall back to the active
 * VT, so the boot path keeps a working /dev/tty).  Keyed by pid because the
 * process metadata block is shared with Rust and changing its layout would
 * ripple through the ABI assertions there. */
int  tty_get_ctty(void);
void tty_set_ctty(int idx);
void tty_clear_ctty(uint32_t pid);

#endif
