#ifndef TTY_H
#define TTY_H

#include <stdint.h>

/* Virtual terminals, exposed Unix-style as /dev/tty1 .. /dev/tty<TTY_MAX>.
 *
 * /dev/tty0 follows Linux: it is an alias for whichever VT is currently
 * active, it is not a terminal of its own.  Everything below takes a VT
 * index where 0 means "the active VT" and 1..TTY_MAX names a concrete one. */

#define TTY_MAX     6       /* /dev/tty1 .. /dev/tty6 */
#define TTY_OUTBUF  4096    /* per-VT output scrollback, replayed on switch */

void tty_init(void);

int  tty_count(void);               /* number of real VTs (TTY_MAX) */
int  tty_active(void);              /* active VT, 1..TTY_MAX */
int  tty_activate(int n);           /* switch the console to VT n */
int  tty_index(int idx);            /* resolve 0 -> active VT index */

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
