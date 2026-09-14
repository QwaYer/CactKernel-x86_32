#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

/* Kernel message log ("/dev/kmsg") — Linux-compatible record stream.
 *
 * Every record is stored in the ring already rendered in the Linux
 * /dev/kmsg read format:
 *
 *     <level>,<seq>,<usec>,<flags>;<message>\n
 *
 *   level : KERN_EMERG(0) .. KERN_DEBUG(7)
 *   seq   : per-record sequence number, 1-based, never reused
 *   usec  : monotonic timestamp (ktime_get_usec; 0 until timekeeping is up)
 *   flags : KLOG_F_* bits below
 *
 * Reads keep plain offset semantics (offset 0 == oldest retained byte), so
 * cat /dev/kmsg reproduces the whole transcript in order.  Userspace may
 * also write records: their level comes from the leading "<N>" or
 * "N,seq,usec,flags,facility;" prefix, and KLOG_F_USER is set.
 *
 * The console (framebuffer + serial) is a SEPARATE channel: printk() feeds
 * the log with the message level and renders plain (colored) text to the
 * console.  Records written to /dev/kmsg from userspace are kept in the log
 * only — they are never echoed back to the console. */

/* Syslog levels (same numbering as KERN_<level>). */
#define KLOG_LEVEL_EMERG  0
#define KLOG_LEVEL_ALERT  1
#define KLOG_LEVEL_CRIT   2
#define KLOG_LEVEL_ERR    3
#define KLOG_LEVEL_WARN   4
#define KLOG_LEVEL_NOTICE 5
#define KLOG_LEVEL_INFO   6
#define KLOG_LEVEL_DEBUG  7

/* Level used for messages without an explicit one. */
#define KLOG_LEVEL_DEFAULT KLOG_LEVEL_INFO

/* Record flags.  The low bits mirror Linux's LOG_* record flags. */
#define KLOG_F_NEWLINE 0x02u    /* record ends a line                        */
#define KLOG_F_CONT    0x08u    /* continuation of the previous record       */
#define KLOG_F_USER    0x10u    /* written by userspace (facility = LOG_USER) */

/* Feed console text at `level` into the log.  ANSI escape sequences are
 * stripped, "\r" is ignored, and lines are accumulated until '\n'; each
 * completed line becomes one record.  A partial line is flushed when read. */
void klog_feed(int level, const char *text, uint32_t len);

/* Append userspace-written text (a write(2) on /dev/kmsg).  Parses an
 * optional "<level>" / "level,seq,usec,flags,facility;" prefix and splits
 * the remainder into records flagged KLOG_F_USER.  Returns the number of
 * input bytes consumed. */
int klog_write_user(const char *text, uint32_t len);

/* Offset-based read of the retained records.  Returns the number of bytes
 * written into buf, or 0 at end of log. */
int klog_read(uint32_t off, uint32_t size, char *buf);

/* Total bytes of rendered records currently retained (usable as file size). */
uint32_t klog_available(void);

/* Number of records appended since boot (sequence high-water mark). */
uint32_t klog_line_count(void);

/* Number of bytes dropped because the ring buffer was full. */
uint32_t klog_dropped_bytes(void);

/* Next sequence number to be assigned. */
uint32_t klog_seq(void);

#endif
