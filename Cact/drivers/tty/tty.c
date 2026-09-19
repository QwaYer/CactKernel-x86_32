#include "tty.h"
#include "kernel.h"
#include "klib.h"
#include "task.h"
#include "fb.h"
#include "validate.h"
#include "ioctl_abi.h"

// tty.c — virtual terminal core.
//
// One physical console (keyboard in, framebuffer out) is multiplexed into
// TTY_MAX virtual terminals.  The active VT owns the console: its input is
// delivered to readers and its output is drawn.  A background VT keeps its
// output in a ring of recent bytes and gets it replayed when it is activated,
// which is what gives each /dev/ttyN a screen of its own without a real VT
// switch in the video hardware.

typedef struct {
    char     out[TTY_OUTBUF];
    uint32_t out_len;
} tty_vt_t;

static tty_vt_t vts[TTY_MAX + 1];   // index 1..TTY_MAX
static int      tty_ready = 0;
static int      tty_cur   = 1;

// Per-VT input queues.  A keystroke is queued on the VT that was active when
// it arrived, so keys typed on one terminal never surface on another.
#define TTY_IBUF 256
static volatile char     in_buf[TTY_MAX + 1][TTY_IBUF];
static volatile uint32_t in_wr[TTY_MAX + 1];
static volatile uint32_t in_rd[TTY_MAX + 1];

// pid -> controlling VT.  Small fixed table: terminals here are a single-user
// console, so a handful of sessions is more than enough.
#define TTY_CTTY_MAX 32
static struct { uint32_t pid; int idx; } ctty_tbl[TTY_CTTY_MAX];

int tty_count(void)  { return TTY_MAX; }
int tty_active(void) { return tty_cur; }

int tty_index(int idx) { return (idx <= 0) ? tty_cur : idx; }

void tty_input(int idx, char c) {
    if (!c) return;
    int i = tty_index(idx);
    if (i < 1 || i > TTY_MAX) return;
    uint32_t next = (in_wr[i] + 1) % TTY_IBUF;
    if (next != in_rd[i]) {              // drop when full
        in_buf[i][in_wr[i]] = c;
        __asm__ __volatile__("" ::: "memory");
        in_wr[i] = next;
    }
}

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
        // waits until its terminal is switched to.
        if (i != tty_cur) { schedule(); continue; }
        if (in_rd[i] == in_wr[i]) { schedule(); continue; }

        __asm__ __volatile__("" ::: "memory");
        char c = in_buf[i][in_rd[i]];
        in_rd[i] = (in_rd[i] + 1) % TTY_IBUF;

        buf[k++] = c;
        // One syscall = one key for size==1 (readline); larger reads are
        // line-oriented, exactly like the old single /dev/tty.
        if (size <= 1) break;
        if (c == '\n') break;
    }
    return (int)k;
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
        in_wr[i] = 0;
        in_rd[i] = 0;
    }
    tty_cur   = 1;
    tty_ready = 1;
}
