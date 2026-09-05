#include "klog.h"
#include "klib.h"

/* klog.c — kernel message log ring buffer.
 *
 * Printk output is funnelled here (from printk_color in fb_text.c) before it
 * reaches the framebuffer/serial console.  We keep plain-text lines in a
 * power-of-two ring so that a VFS node (/dev/kmsg) can present the boot log
 * with plain offset semantics: offsets are relative to the oldest byte that
 * is still retained, so reading sequentially reproduces the console order. */

#define KLOG_BUF_SIZE_LOG2 17u
#define KLOG_BUF_SIZE      (1u << KLOG_BUF_SIZE_LOG2)
#define KLOG_BUF_MASK      (KLOG_BUF_SIZE - 1u)
#define KLOG_LINE_MAX      512u

static char     klog_buf[KLOG_BUF_SIZE];   /* ring storage                  */
static uint32_t klog_first = 0;            /* logical idx of oldest byte    */
static uint32_t klog_next  = 0;            /* logical idx of next write     */
static uint32_t klog_lines = 0;            /* completed line records        */
static uint32_t klog_lost  = 0;            /* bytes dropped (ring overflow) */

static char     klog_pend[KLOG_LINE_MAX];  /* current partial line          */
static uint32_t klog_pend_len = 0;
static uint32_t klog_pend_trunc = 0;       /* chars dropped (line too long) */

static uint32_t klog_irq_save(void) {
    uint32_t flags;
    __asm__ __volatile__("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static void klog_irq_restore(uint32_t flags) {
    __asm__ __volatile__("pushl %0\n\tpopfl" : : "r"(flags) : "memory", "cc");
}

/* Append raw bytes to the ring, dropping oldest bytes on overflow. */
static void klog_append(const char *s, uint32_t n) {
    uint32_t flags = klog_irq_save();
    for (uint32_t i = 0; i < n; i++) {
        if (klog_next - klog_first >= KLOG_BUF_SIZE) {
            klog_first++;
            klog_lost++;
        }
        klog_buf[klog_next & KLOG_BUF_MASK] = s[i];
        klog_next++;
    }
    klog_irq_restore(flags);
}

/* Complete the current pending line (if any) as one log record. */
static void klog_flush_line(void) {
    if (klog_pend_len == 0) {
        klog_pend_len = 0;
        klog_pend_trunc = 0;
        return;
    }
    klog_append(klog_pend, klog_pend_len);
    klog_append("\n", 1);
    klog_lines++;
    klog_pend_len = 0;
    klog_pend_trunc = 0;
}

void klog_feed(const char *text, uint32_t len) {
    if (!text || len == 0) return;

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

        if (klog_pend_len < KLOG_LINE_MAX) {
            klog_pend[klog_pend_len++] = c;
        } else {
            klog_pend_trunc++;
        }
    }
}

uint32_t klog_available(void) {
    return klog_next - klog_first;
}

uint32_t klog_line_count(void) {
    return klog_lines;
}

uint32_t klog_dropped_bytes(void) {
    return klog_lost;
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
