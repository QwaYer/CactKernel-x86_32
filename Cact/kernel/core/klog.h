#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

/* Kernel message log ("/dev/kmsg").
 *
 * Every line that reaches the kernel console (printk() / printk_color())
 * is captured into a fixed-size ring buffer as plain text and can be read
 * back through the /dev/kmsg VFS node — a real boot log of the kernel,
 * the same transcript that scrolled on the framebuffer/serial console.
 *
 * The buffer keeps the most recent KLOG_BUF_SIZE bytes; when it overflows
 * the oldest bytes are dropped (byte granularity).  Reads are offset-based
 * (VFS node semantics): offset 0 always means the oldest still-retained
 * byte, so cat /dev/kmsg returns the whole log in chronological order. */

/* Feed raw console text into the log.  ANSI escape sequences are stripped,
 * "\r" is ignored, lines are accumulated until "\n" (or EOF, see klog_read),
 * and a line becomes one log record. */
void klog_feed(const char *text, uint32_t len);

/* Offset-based read of the retained log text.  Returns the number of bytes
 * written into buf, or 0 at end of log. */
int klog_read(uint32_t off, uint32_t size, char *buf);

/* Total bytes of log text currently retained (usable as file size). */
uint32_t klog_available(void);

/* Number of completed lines retained (informational). */
uint32_t klog_line_count(void);

/* Number of bytes dropped because the ring buffer was full. */
uint32_t klog_dropped_bytes(void);

#endif
