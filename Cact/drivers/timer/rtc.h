#ifndef CACT_RTC_H
#define CACT_RTC_H

#include <stdint.h>

/* CMOS real-time clock — the machine's only source of civil time.
 *
 * Everything else in the kernel counts since boot (tick.c's 100 Hz jiffies,
 * ktime's microseconds).  That is enough for timeouts, but not for anything
 * that has to agree with the outside world: an X.509 validity window
 * (notBefore/notAfter) cannot be compared against an uptime counter, and with
 * a since-boot clock every certificate on the internet looks like it was
 * issued in the future.  So the date is read once at boot and the wall clock
 * then runs as "date at boot + elapsed ticks". */
void     rtc_init(void);

/* 1 when a plausible date was read, i.e. rtc_epoch_*() is civil time. */
int      rtc_have_time(void);

/* Seconds (or microseconds) since the Unix epoch.  Zero when the RTC yielded
 * no usable date — callers then have nothing better than the boot clock. */
uint64_t rtc_epoch_sec(void);
uint64_t rtc_epoch_usec(void);

#endif /* CACT_RTC_H */
