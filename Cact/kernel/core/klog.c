#include "klog.h"
#include "klib.h"
#include "ktime.h"

/* klog.c — kernel message log ring buffer (Linux-compatible records).
 *
 * printk() feeds console text here together with its KERN_<level>; the text
 * of every completed line is stored rendered as:
 *
 *     <level>,<seq>,<usec>,<flags>;<message>\n
 *
 * and /dev/kmsg reads the ring back with plain offset semantics.  The ring is
 * a power of two so offset 0 always means "oldest retained byte"; overflow
 * drops the oldest bytes, so the log keeps the most recent KLOG_BUF_SIZE
 * bytes of the transcript. */

#define KLOG_BUF_SIZE_LOG2 17u
#define KLOG_BUF_SIZE      (1u << KLOG_BUF_SIZE_LOG2)
#define KLOG_BUF_MASK      (KLOG_BUF_SIZE - 1u)
#define KLOG_LINE_MAX      512u
#define KLOG_REC_MAX       (KLOG_LINE_MAX + 64u)   /* line + record prefix */

static char     klog_buf[KLOG_BUF_SIZE];   /* ring storage                  */
static uint32_t klog_first = 0;            /* logical idx of oldest byte    */
static uint32_t klog_next  = 0;            /* logical idx of next write     */
static uint32_t klog_recs  = 0;            /* records appended              */
static uint32_t klog_next_seq = 1;         /* sequence of the next record   */
static uint32_t klog_lost  = 0;            /* bytes dropped (ring overflow) */

static char     klog_pend[KLOG_LINE_MAX];  /* current partial line          */
static uint32_t klog_pend_len = 0;
static uint32_t klog_pend_trunc = 0;       /* chars dropped (line too long) */
static int      klog_pend_level = KLOG_LEVEL_DEFAULT;

static uint32_t klog_irq_save(void) {
    uint32_t flags;
    __asm__ __volatile__("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static void klog_irq_restore(uint32_t flags) {
    __asm__ __volatile__("pushl %0\n\tpopfl" : : "r"(flags) : "memory", "cc");
}

/* Append raw bytes to the ring, dropping oldest bytes on overflow.  Caller
 * holds the log lock (interrupts disabled). */
static void klog_append(const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (klog_next - klog_first >= KLOG_BUF_SIZE) {
            klog_first++;
            klog_lost++;
        }
        klog_buf[klog_next & KLOG_BUF_MASK] = s[i];
        klog_next++;
    }
}

/* Append one record: "<level>,<seq>,<usec>,<flags>;<text>\n". */
static void klog_emit(int level, uint32_t flags, const char *text, uint32_t len) {
    if (len == 0) return;

    char     rec[KLOG_REC_MAX];
    uint32_t total;

    uint32_t lock = klog_irq_save();
    uint32_t seq  = klog_next_seq;
    int hn = snprintf(rec, sizeof(rec), "%u,%u,%llu,%x;",
                      (unsigned)(level & 7), (unsigned)seq,
                      (unsigned long long)ktime_get_usec(), (unsigned)flags);
    if (hn < 0) hn = 0;
    if ((uint32_t)hn > sizeof(rec) - 2) hn = (int)sizeof(rec) - 2;
    total = (uint32_t)hn;

    if (len > sizeof(rec) - total - 1) len = sizeof(rec) - total - 1;
    memcpy(rec + total, text, len);
    total += len;
    rec[total++] = '\n';

    klog_append(rec, total);
    klog_next_seq++;
    klog_recs++;
    klog_irq_restore(lock);
}

/* Complete the current pending line (if any) as one log record. */
static void klog_flush_line(void) {
    if (klog_pend_len == 0) {
        klog_pend_trunc = 0;
        return;
    }
    klog_emit(klog_pend_level, KLOG_F_NEWLINE, klog_pend, klog_pend_len);
    klog_pend_len = 0;
    klog_pend_trunc = 0;
}

void klog_feed(int level, const char *text, uint32_t len) {
    if (!text || len == 0) return;
    if (level < 0 || level > 7) level = KLOG_LEVEL_DEFAULT;

    for (uint32_t i = 0; i < len; i++) {
        char c = text[i];

        /* ANSI escape handling: ESC, CSI "[..", OSC "]..BEL", other. */
        static int esc_state;
        if (esc_state == 1) {
            if (c == '[' || c == ']') {
                esc_state = 2;
            } else if (c >= 0x40 && c <= 0x7E) {
                esc_state = 0;
            } else if (c == 0x1B) {
                /* two ESC in a row — keep skipping */
            } else {
                esc_state = 0;
            }
            continue;
        }
        if (esc_state == 2) {
            if (c == 0x07) {            /* OSC terminated by BEL      */
                esc_state = 0;
            } else if (c == 0x1B) {     /* possibly ESC \ (ST)        */
                esc_state = 3;
            } else if (c >= 0x40 && c <= 0x7E) {
                esc_state = 0;
            }
            continue;
        }
        if (esc_state == 3) {
            esc_state = (c == '\\') ? 0 : 2;
            continue;
        }
        if (c == 0x1B) {
            esc_state = 1;
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            klog_flush_line();
            continue;
        }
        if (c < 0x20 && c != '\t') continue;   /* drop other control chars */

        if (klog_pend_len == 0)
            klog_pend_level = level;           /* level of the line's first span */

        if (klog_pend_len < KLOG_LINE_MAX) {
            klog_pend[klog_pend_len++] = c;
        } else {
            klog_pend_trunc++;
        }
    }
}

int klog_write_user(const char *text, uint32_t len) {
    if (!text || len == 0) return 0;

    int      level = KLOG_LEVEL_DEFAULT;
    uint32_t pos   = 0;

    if (text[0] == '<') {
        /* "<N>message" */
        uint32_t i = 1;
        int      lv = 0, have = 0;
        while (i < len && text[i] >= '0' && text[i] <= '9') {
            lv = lv * 10 + (text[i] - '0');
            i++; have = 1;
        }
        if (have && i < len && text[i] == '>' && lv <= 7) {
            level = lv;
            pos   = i + 1;
        }
    } else {
        /* "level[,seq[,usec[,flags[,facility]]]];message" — a leading run of
         * digits and commas terminated by ';' inside the first 40 bytes. */
        uint32_t sem = 0;
        int      ok  = 0;
        for (uint32_t i = 0; i < len && i < 40; i++) {
            if (text[i] == ';') { sem = i; ok = 1; break; }
            if (!((text[i] >= '0' && text[i] <= '9') || text[i] == ',')) break;
        }
        if (ok && sem > 0) {
            int      lv = 0;
            uint32_t i  = 0;
            while (i < sem && text[i] >= '0' && text[i] <= '9') {
                lv = lv * 10 + (text[i] - '0');
                i++;
            }
            if (lv <= 7) level = lv;
            pos = sem + 1;
        }
    }

    uint32_t start = pos;
    for (uint32_t i = pos; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            uint32_t n = i - start;
            if (n > 0)
                klog_emit(level, KLOG_F_USER | KLOG_F_NEWLINE, text + start, n);
            start = i + 1;
        }
    }
    return (int)len;
}

uint32_t klog_available(void) {
    return klog_next - klog_first;
}

uint32_t klog_line_count(void) {
    return klog_recs;
}

uint32_t klog_dropped_bytes(void) {
    return klog_lost;
}

uint32_t klog_seq(void) {
    return klog_next_seq;
}

int klog_read(uint32_t off, uint32_t size, char *buf) {
    if (!buf || size == 0) return 0;

    /* Include a trailing line that has not seen '\n' yet. */
    klog_flush_line();

    uint32_t flags = klog_irq_save();
    uint32_t first = klog_first;
    uint32_t next  = klog_next;
    uint32_t avail = next - first;
    if (off >= avail) {
        klog_irq_restore(flags);
        return 0;
    }
    if (size > avail - off) size = avail - off;

    uint32_t copied = 0;
    uint32_t idx    = first + off;
    while (copied < size) {
        uint32_t phys  = idx & KLOG_BUF_MASK;
        uint32_t chunk = KLOG_BUF_SIZE - phys;
        if (chunk > size - copied) chunk = size - copied;
        memcpy(buf + copied, klog_buf + phys, chunk);
        copied += chunk;
        idx    += chunk;
    }
    klog_irq_restore(flags);
    return (int)copied;
}
