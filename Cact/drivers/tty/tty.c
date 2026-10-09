#include "tty.h"
#include "kernel.h"
#include "klib.h"
#include "task.h"
#include "fb.h"
#include "validate.h"
#include "ioctl_abi.h"

extern void sched_sleep_ticks(uint32_t ticks);
extern uint32_t terminal_fg_pid;

#ifndef EINTR
#define EINTR 4
#endif

// tty.c — virtual terminal core and line discipline.
//
// One physical console (keyboard in, framebuffer out) is multiplexed into
// TTY_MAX virtual terminals.  The active VT owns the console: its input is
// delivered to readers and its output is drawn.  A background VT keeps its
// output in a ring of recent bytes and gets it replayed when it is activated,
// which is what gives each /dev/ttyN a screen of its own without a real VT
// switch in the video hardware.
//
// Each VT also carries a termios and a small line discipline.  In canonical
// mode the kernel edits the line (erase/kill/word-erase/EOF) and echoes it; in
// raw mode bytes go straight to the reader.  Interactive programs (cactsole)
// put the terminal in raw mode and do their own editing, exactly as on Linux —
// which is why the control characters have to be interpreted here rather than
// in the keyboard driver: only the terminal knows whether the reader wants a
// signal or the raw byte.

// ── termios ABI (must match CactLibc/include/termios.h) ─────────────────────
#define TTY_TCGETS   0x5401
#define TTY_TCSETS   0x5402
#define TTY_TCSETSW  0x5403
#define TTY_TCSETSF  0x5404

#define TTY_ISIG   0x0001
#define TTY_ICANON 0x0002
#define TTY_ECHO   0x0008
#define TTY_ECHOE  0x0010
#define TTY_ECHOK  0x0020
#define TTY_ECHONL 0x0040

#define TTY_NCCS 19
// i386 c_cc[] indices, matching Linux.
#define TTY_VINTR    0
#define TTY_VQUIT    1
#define TTY_VERASE   2
#define TTY_VKILL    3
#define TTY_VEOF     4
#define TTY_VTIME    5
#define TTY_VMIN     6
#define TTY_VSTART   8
#define TTY_VSTOP    9
#define TTY_VSUSP   10
#define TTY_VEOL    11
#define TTY_VWERASE 14
#define TTY_VLNEXT  15
#define TTY_VEOL2   16

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[TTY_NCCS];
} tty_termios_t;

typedef struct {
    char     out[TTY_OUTBUF];
    uint32_t out_len;
} tty_vt_t;

static tty_vt_t vts[TTY_MAX + 1];   // index 1..TTY_MAX
static int      tty_ready = 0;
static int      tty_cur   = 1;

// Per-VT termios.  tio[0] is unused; the active VT is resolved at call time.
static tty_termios_t tio[TTY_MAX + 1];

// Per-VT cooked queue: in raw mode a keystroke lands here directly; in
// canonical mode a whole committed line does.  tty_read() drains it.
#define TTY_IBUF 1024
static volatile char     in_buf[TTY_MAX + 1][TTY_IBUF];
static volatile uint32_t in_wr[TTY_MAX + 1];
static volatile uint32_t in_rd[TTY_MAX + 1];

// Canonical line edit buffer.  Capped two bytes below the queue so that even a
// full line plus its '\n' always fits in the ring without a drop.
#define TTY_LINE_MAX (TTY_IBUF - 2)
static char     line_buf[TTY_MAX + 1][TTY_LINE_MAX];
static uint16_t line_len[TTY_MAX + 1];
static uint8_t  eof_pending[TTY_MAX + 1];

// pid -> controlling VT.  Small fixed table: terminals here are a single-user
// console, so a handful of sessions is more than enough.
#define TTY_CTTY_MAX 32
static struct { uint32_t pid; int idx; } ctty_tbl[TTY_CTTY_MAX];

int tty_count(void)  { return TTY_MAX; }
int tty_active(void) { return tty_cur; }

int tty_index(int idx) { return (idx <= 0) ? tty_cur : idx; }

static void tty_termios_default(tty_termios_t *t) {
    memset(t, 0, sizeof(*t));
    t->c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | TTY_ECHOK;
    t->c_cc[TTY_VINTR]   = 0x03;   // ^C
    t->c_cc[TTY_VQUIT]   = 0x1C;   /* ^\ */
    t->c_cc[TTY_VERASE]  = 0x7F;   // DEL
    t->c_cc[TTY_VKILL]   = 0x15;   // ^U
    t->c_cc[TTY_VEOF]    = 0x04;   // ^D
    t->c_cc[TTY_VTIME]   = 0;
    t->c_cc[TTY_VMIN]    = 1;
    t->c_cc[TTY_VSTART]  = 0x11;   // ^Q
    t->c_cc[TTY_VSTOP]   = 0x13;   // ^S
    t->c_cc[TTY_VSUSP]   = 0x1A;   // ^Z
    t->c_cc[TTY_VEOL]    = 0;
    t->c_cc[TTY_VWERASE] = 0x17;   // ^W
    t->c_cc[TTY_VLNEXT]  = 0x16;   // ^V
    t->c_cc[TTY_VEOL2]   = 0;
}

// ── echo ────────────────────────────────────────────────────────────────────
// Echo only ever goes to the active VT, and only through the framebuffer path.
// console_puts() also records it into that VT's scrollback, so a later VT
// switch replays the typed line.  Nothing is echoed while the reader is in raw
// mode (ECHO is off there), which keeps the common interactive path free of
// framebuffer writes from interrupt context.
static void vt_echo_char(int i, char c) {
    if (i != tty_cur) return;
    if (fb_get_width() == 0 || fb_get_height() == 0) return;
    char s[2];
    s[0] = c;
    s[1] = '\0';
    console_puts(s, COLOR_WHITE);
}

static void vt_echo_str(int i, const char *s) {
    while (*s)
        vt_echo_char(i, *s++);
}

// ── input queue ─────────────────────────────────────────────────────────────
static void in_push(int i, char c) {
    uint32_t next = (in_wr[i] + 1) % TTY_IBUF;
    if (next == in_rd[i])
        return;                       // full: drop
    in_buf[i][in_wr[i]] = c;
    __asm__ __volatile__("" ::: "memory");
    in_wr[i] = next;
}

static void line_clear(int i) { line_len[i] = 0; }

static void line_commit(int i, int add_newline) {
    for (uint16_t k = 0; k < line_len[i]; k++)
        in_push(i, line_buf[i][k]);
    if (add_newline)
        in_push(i, '\n');
    line_clear(i);
}

// ── line discipline ─────────────────────────────────────────────────────────
static void tty_signal_fg(uint32_t sig) {
    if (terminal_fg_pid)
        task_signal(terminal_fg_pid, sig);
}

// Word erase: drop trailing blanks, then the word before the cursor.
static void line_werase(int i) {
    uint16_t n = line_len[i];
    while (n > 0 && (line_buf[i][n - 1] == ' ' || line_buf[i][n - 1] == '\t'))
        n--;
    while (n > 0 && line_buf[i][n - 1] != ' ' && line_buf[i][n - 1] != '\t')
        n--;
    if (tio[i].c_lflag & TTY_ECHO)
        while (line_len[i] > n) {
            line_len[i]--;
            vt_echo_str(i, "\b \b");
        }
    line_len[i] = n;
}

void tty_input(int idx, char c) {
    if (!c) return;
    int i = tty_index(idx);
    if (i < 1 || i > TTY_MAX) return;

    tty_termios_t *t = &tio[i];

    if (!(t->c_lflag & TTY_ICANON)) {
        in_push(i, c);                // raw: straight to the reader
        return;
    }

    unsigned char uc = (unsigned char)c;

    if (t->c_lflag & TTY_ISIG) {
        if (uc == t->c_cc[TTY_VINTR]) {
            if (terminal_fg_pid) {
                tty_signal_fg(SIGINT);
                line_clear(i);
                if (t->c_lflag & TTY_ECHO) vt_echo_str(i, "^C\r\n");
            } else {
                in_push(i, c);        // no foreground process: hand the byte through
            }
            return;
        }
        if (uc == t->c_cc[TTY_VQUIT]) {
            if (terminal_fg_pid) {
                tty_signal_fg(SIGQUIT);
                line_clear(i);
                if (t->c_lflag & TTY_ECHO) vt_echo_str(i, "^\\\r\n");
            } else {
                in_push(i, c);
            }
            return;
        }
        if (uc == t->c_cc[TTY_VSUSP]) {
            if (terminal_fg_pid) {
                tty_signal_fg(SIGSTOP);
                line_clear(i);
                if (t->c_lflag & TTY_ECHO) vt_echo_str(i, "^Z\r\n");
            } else {
                in_push(i, c);
            }
            return;
        }
    }

    if (uc == t->c_cc[TTY_VEOF]) {
        // EOF at the start of a line is a zero-length read; mid-line it
        // delivers what has been typed without a newline (both as on Linux).
        if (line_len[i] == 0)
            eof_pending[i] = 1;
        else
            line_commit(i, 0);
        return;
    }

    if (uc == '\n' || uc == '\r') {
        if (t->c_lflag & TTY_ECHO) vt_echo_str(i, "\r\n");
        line_commit(i, 1);
        return;
    }

    if (uc == t->c_cc[TTY_VERASE] || uc == 0x08) {
        if (line_len[i] > 0) {
            line_len[i]--;
            if (t->c_lflag & TTY_ECHO) vt_echo_str(i, "\b \b");
        }
        return;
    }

    if (uc == t->c_cc[TTY_VKILL]) {
        if (t->c_lflag & TTY_ECHO)
            while (line_len[i] > 0) {
                line_len[i]--;
                vt_echo_str(i, "\b \b");
            }
        line_clear(i);
        return;
    }

    if (uc == t->c_cc[TTY_VWERASE]) {
        line_werase(i);
        return;
    }

    if (line_len[i] < TTY_LINE_MAX) {
        line_buf[i][line_len[i]++] = c;
        if ((t->c_lflag & TTY_ECHO) && uc >= 0x20 && uc < 0x7f)
            vt_echo_char(i, c);
    }
}

// ── output scrollback ───────────────────────────────────────────────────────
// Keep the most recent TTY_OUTBUF bytes of a VT's output.
static void vt_append(int i, const char *buf, uint32_t n) {
    tty_vt_t *v = &vts[i];

    if (n >= TTY_OUTBUF) {
        memcpy(v->out, buf + (n - TTY_OUTBUF), TTY_OUTBUF);
        v->out_len = TTY_OUTBUF;
        return;
    }
    if (v->out_len + n > TTY_OUTBUF) {
        uint32_t drop = v->out_len + n - TTY_OUTBUF;
        uint32_t keep = v->out_len - drop;
        for (uint32_t k = 0; k < keep; k++)   // dst < src: forward copy is safe
            v->out[k] = v->out[drop + k];
        v->out_len = keep;
    }
    memcpy(v->out + v->out_len, buf, n);
    v->out_len += n;
}

// Every console render (kernel log via printk(), userspace via the active VT)
// lands here so the active VT's scrollback matches the screen.
void console_on_write(const char *buf, uint32_t len) {
    if (len) vt_append(tty_cur, buf, len);
}

// Repaint the console from a VT's scrollback.  console_replay() renders the
// bytes without touching the serial port or the kernel log, so switching VTs
// does not duplicate a screenful into the boot log.
static void vt_render(int i) {
    if (fb_get_width() == 0 || fb_get_height() == 0) return;
    clear_screen();
    if (vts[i].out_len)
        console_replay(vts[i].out, vts[i].out_len, COLOR_WHITE);
}

int tty_activate(int n) {
    if (n < 1 || n > TTY_MAX) return -1;
    if (n == tty_cur) return 0;
    tty_cur = n;
    vt_render(n);
    return 0;
}

int tty_write(int idx, uint32_t off, uint32_t size, char *buf) {
    (void)off;
    if (!size) return 0;
    if (!buf || !validate_user_ptr(buf, size)) return -1;

    int i = tty_index(idx);
    if (i < 1 || i > TTY_MAX) return -1;

    if (i == tty_cur) {
        // Rendering through console_puts() records the bytes in the active VT's
        // scrollback via console_on_write(), so do not append here as well.
        char tmp[256];
        uint32_t p = 0;
        while (p < size) {
            uint32_t c = size - p;
            if (c >= sizeof(tmp)) c = sizeof(tmp) - 1;
            memcpy(tmp, buf + p, c);
            tmp[c] = '\0';
            console_puts(tmp, COLOR_WHITE);
            p += c;
        }
    } else {
        // Hidden VT: keep the output for its repaint, draw nothing.
        vt_append(i, buf, size);
    }
    return (int)size;
}

int tty_read(int idx, uint32_t off, uint32_t size, char *buf) {
    (void)off;
    if (!size) return 0;

    int i = tty_index(idx);
    if (i < 1 || i > TTY_MAX) return -1;

    uint32_t k = 0;
    while (k < size) {
        // Input belongs to the active VT: a reader on a background terminal
        // waits until its terminal is switched to.  Sleep a tick rather than
        // busy-yielding: a spinning reader hammered the one global scheduler
        // lock and starved the master core's timer.
        if (i != tty_cur) { sched_sleep_ticks(1); continue; }

        if (in_rd[i] == in_wr[i]) {
            // A zero-length read left by ^D is reported only once the queue is
            // drained, so EOF cannot overtake the bytes typed before it.
            if (eof_pending[i]) {
                eof_pending[i] = 0;
                return (int)k;
            }
            // A signal with a user handler has to reach the process, and the
            // scheduler-side delivery path cannot run while we sit in here, so
            // report EINTR: the syscall return then builds the handler frame.
            // The job-control notifications are excluded — SIGCHLD/SIGWINCH are
            // reported to the app, not delivered, and must not abort a read.
            if (task_signal_pending_current() & ~(SIGCHLD | SIGWINCH))
                return -EINTR;
            sched_sleep_ticks(1);
            continue;
        }

        __asm__ __volatile__("" ::: "memory");
        char c = in_buf[i][in_rd[i]];
        in_rd[i] = (in_rd[i] + 1) % TTY_IBUF;

        buf[k++] = c;

        // Canonical: one read returns the whole line, up to `size`.  Raw: one
        // read returns whatever is available, without waiting for a newline.
        if (c == '\n') break;
        if (size <= 1) break;
        if (!(tio[i].c_lflag & TTY_ICANON)) {
            if (in_rd[i] == in_wr[i]) break;
        }
    }
    return (int)k;
}

static void tty_termios_get(int i, tty_termios_t *out) {
    *out = tio[i];
}

static void tty_termios_set(int i, const tty_termios_t *in) {
    tio[i] = *in;
}

int tty_ioctl(int idx, uint32_t cmd, void *arg) {
    switch (cmd) {
    case CACT_TTYCTL_GET_INDEX: {
        // The node's own index, not the resolved one: /dev/tty0 reports 0
        // ("the active-VT alias") while /dev/ttyN reports N.  Use
        // CACT_TTYCTL_VT_GETSTATE to learn which VT is active right now.
        if (!arg || !validate_user_ptr(arg, sizeof(int))) return -1;
        *(int *)arg = idx;
        return 0;
    }

    case CACT_TTYCTL_VT_ACTIVATE: {
        int n;
        if (!arg || copy_from_user(&n, arg, sizeof(n)) != 0) return -1;
        return tty_activate(n);
    }

    case CACT_TTYCTL_VT_GETSTATE: {
        if (!arg || !validate_user_ptr(arg, sizeof(cact_vt_state_t))) return -1;
        cact_vt_state_t st;
        st.v_active = (uint16_t)tty_cur;
        st.v_count  = (uint16_t)TTY_MAX;
        return copy_to_user(arg, &st, sizeof(st));
    }

    case CACT_TTYCTL_SET_CTTY: {
        int n;
        if (!arg || copy_from_user(&n, arg, sizeof(n)) != 0) return -1;
        if (n < 0 || n > TTY_MAX) return -1;
        tty_set_ctty(n);
        return 0;
    }

    case CACT_TTYCTL_GET_CTTY: {
        if (!arg || !validate_user_ptr(arg, sizeof(int))) return -1;
        *(int *)arg = tty_get_ctty();
        return 0;
    }

    case TTY_TCGETS: {
        if (!arg || !validate_user_ptr(arg, sizeof(tty_termios_t))) return -1;
        tty_termios_t t;
        tty_termios_get(tty_index(idx), &t);
        return copy_to_user(arg, &t, sizeof(t));
    }

    case TTY_TCSETS:
    case TTY_TCSETSW:
    case TTY_TCSETSF: {
        tty_termios_t t;
        if (!arg || copy_from_user(&t, arg, sizeof(t)) != 0) return -1;
        int i = tty_index(idx);
        if (i < 1 || i > TTY_MAX) return -1;
        if (cmd == TTY_TCSETSF) {
            // Flush pending input, as Linux does, so a mode change out of
            // canonical does not deliver a half-edited line afterwards.
            line_clear(i);
            in_rd[i] = in_wr[i];
            eof_pending[i] = 0;
        }
        tty_termios_set(i, &t);
        return 0;
    }

    default:
        return -1;
    }
}

int tty_get_ctty(void) {
    if (!current_task) return 0;
    for (int i = 0; i < TTY_CTTY_MAX; i++)
        if (ctty_tbl[i].pid == current_task->pid)
            return ctty_tbl[i].idx;
    return 0;
}

void tty_set_ctty(int idx) {
    if (!current_task) return;
    if (idx <= 0) idx = tty_cur;

    int free_slot = -1;
    for (int i = 0; i < TTY_CTTY_MAX; i++) {
        if (ctty_tbl[i].pid == current_task->pid) {
            ctty_tbl[i].idx = idx;
            return;
        }
        if (free_slot < 0 && ctty_tbl[i].pid == 0)
            free_slot = i;
    }
    if (free_slot < 0) free_slot = 0;   // table full: recycle the oldest slot
    ctty_tbl[free_slot].pid = current_task->pid;
    ctty_tbl[free_slot].idx = idx;
}

void tty_clear_ctty(uint32_t pid) {
    for (int i = 0; i < TTY_CTTY_MAX; i++) {
        if (ctty_tbl[i].pid == pid) {
            ctty_tbl[i].pid = 0;
            ctty_tbl[i].idx = 0;
        }
    }
}

void tty_init(void) {
    if (tty_ready) return;
    // vts[] is intentionally not cleared: console output produced before this
    // point (early boot log) has already been recorded there, and keeping it
    // means a VT switch back shows the boot messages.
    memset(ctty_tbl, 0, sizeof(ctty_tbl));
    for (int i = 0; i <= TTY_MAX; i++) {
        tty_termios_default(&tio[i]);
        in_wr[i] = 0;
        in_rd[i] = 0;
        line_len[i] = 0;
        eof_pending[i] = 0;
    }
    tty_cur   = 1;
    tty_ready = 1;
}
