#include "pty.h"
#include "klib.h"
#include "task.h"
#include "validate.h"

// pty.c — pseudo-terminal pairs.
//
// Deliberately small: each pair is a bidirectional byte channel with the two
// directions kept in separate rings.  A master write goes to the slave's read
// side, a slave write to the master's read side.  There is no line
// discipline/termios here yet — /dev/ttyN carries that — the pair only has to
// be a faithful transport that a terminal emulator can sit on.

typedef struct {
    int      used;
    int      locked;
    int      master_open;
    int      slave_open;
    char     m2s[PTY_BUF];   // master -> slave
    uint32_t m2s_r, m2s_w;
    char     s2m[PTY_BUF];   // slave -> master
    uint32_t s2m_r, s2m_w;
} pty_t;

static pty_t ptys[PTY_MAX];

static uint32_t ring_used(uint32_t r, uint32_t w) {
    return (w >= r) ? (w - r) : (PTY_BUF - r + w);
}

static uint32_t ring_space(uint32_t r, uint32_t w) {
    return PTY_BUF - 1 - ring_used(r, w);
}

int pty_alloc(void) {
    for (int i = 0; i < PTY_MAX; i++) {
        if (!ptys[i].used) {
            memset(&ptys[i], 0, sizeof(pty_t));
            ptys[i].used        = 1;
            ptys[i].locked      = 1;
            ptys[i].master_open = 1;
            return i;
        }
    }
    return -1;
}

int  pty_used(int i)     { return (i >= 0 && i < PTY_MAX && ptys[i].used); }
int  pty_unlocked(int i) { return (i >= 0 && i < PTY_MAX && ptys[i].used && !ptys[i].locked); }
void pty_lock(int i, int locked) {
    if (i >= 0 && i < PTY_MAX) ptys[i].locked = locked ? 1 : 0;
}

int pty_master_read(int i, uint32_t size, char *buf) {
    if (i < 0 || i >= PTY_MAX || !ptys[i].used) return -1;
    if (!buf || !validate_user_ptr(buf, size)) return -1;
    uint32_t n = 0;
    while (n < size) {
        if (ring_used(ptys[i].s2m_r, ptys[i].s2m_w) == 0) {
            if (!ptys[i].slave_open) return (int)n;   // no writer left
            schedule();
            continue;
        }
        buf[n++] = ptys[i].s2m[ptys[i].s2m_r];
        ptys[i].s2m_r = (ptys[i].s2m_r + 1) % PTY_BUF;
    }
    return (int)n;
}

int pty_master_write(int i, uint32_t size, char *buf) {
    if (i < 0 || i >= PTY_MAX || !ptys[i].used) return -1;
    if (!buf || !validate_user_ptr(buf, size)) return -1;
    uint32_t n = 0;
    while (n < size) {
        if (ring_space(ptys[i].m2s_r, ptys[i].m2s_w) == 0) {
            if (!ptys[i].slave_open) return (int)n;
            schedule();
            continue;
        }
        ptys[i].m2s[ptys[i].m2s_w] = buf[n++];
        ptys[i].m2s_w = (ptys[i].m2s_w + 1) % PTY_BUF;
    }
    return (int)n;
}

int pty_slave_read(int i, uint32_t size, char *buf) {
    if (i < 0 || i >= PTY_MAX || !ptys[i].used) return -1;
    if (!buf || !validate_user_ptr(buf, size)) return -1;
    uint32_t n = 0;
    while (n < size) {
        if (ring_used(ptys[i].m2s_r, ptys[i].m2s_w) == 0) {
            if (!ptys[i].master_open) return (int)n;
            schedule();
            continue;
        }
        buf[n++] = ptys[i].m2s[ptys[i].m2s_r];
        ptys[i].m2s_r = (ptys[i].m2s_r + 1) % PTY_BUF;
    }
    return (int)n;
}

int pty_slave_write(int i, uint32_t size, char *buf) {
    if (i < 0 || i >= PTY_MAX || !ptys[i].used) return -1;
    if (!buf || !validate_user_ptr(buf, size)) return -1;
    uint32_t n = 0;
    while (n < size) {
        if (ring_space(ptys[i].s2m_r, ptys[i].s2m_w) == 0) {
            if (!ptys[i].master_open) return (int)n;
            schedule();
            continue;
        }
        ptys[i].s2m[ptys[i].s2m_w] = buf[n++];
        ptys[i].s2m_w = (ptys[i].s2m_w + 1) % PTY_BUF;
    }
    return (int)n;
}

void pty_close_master(int i) {
    if (i < 0 || i >= PTY_MAX) return;
    ptys[i].master_open = 0;
    if (!ptys[i].slave_open) ptys[i].used = 0;
}

void pty_close_slave(int i) {
    if (i < 0 || i >= PTY_MAX) return;
    ptys[i].slave_open = 0;
    if (!ptys[i].master_open) ptys[i].used = 0;
}

void pty_slave_opened(int i) {
    if (i >= 0 && i < PTY_MAX) ptys[i].slave_open = 1;
}
