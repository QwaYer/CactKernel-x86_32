#include "rtc.h"
#include "kernel.h"
#include "tick.h"

/* MC146818-compatible CMOS clock (the chip QEMU emulates, and the one every
 * PC has an equivalent of).  Registers are reached through the index/data
 * port pair; the values are BCD unless the chip says otherwise, and the
 * update cycle can tear a read, so the date is read until two reads agree.
 *
 * The RTC is treated as UTC.  There is no timezone database in the kernel, so
 * a host that starts QEMU with -rtc base=localtime hands the guest its local
 * time and the wall clock ends up off by the host's UTC offset — harmless for
 * certificate validity, which is measured in weeks. */

#define RTC_INDEX_PORT 0x70
#define RTC_DATA_PORT  0x71

#define RTC_REG_SECONDS 0x00
#define RTC_REG_MINUTES 0x02
#define RTC_REG_HOURS   0x04
#define RTC_REG_DAY     0x07
#define RTC_REG_MONTH   0x08
#define RTC_REG_YEAR    0x09
#define RTC_REG_STATUS_A 0x0a
#define RTC_REG_STATUS_B 0x0b
#define RTC_REG_CENTURY  0x32

#define RTC_A_UIP    0x80   /* status A: update in progress */
#define RTC_B_BINARY 0x04   /* status B: SET means the values are binary, not BCD */
#define RTC_B_24H    0x02   /* status B: SET means hours are 0..23, not 12h + PM */

/* The scheduler tick is the same 100 Hz base /proc/time counts in seconds. */
#define RTC_TICKS_PER_SEC 100u
#define RTC_USEC_PER_TICK (1000000u / RTC_TICKS_PER_SEC)

struct rtc_fields {
    uint8_t sec, min, hour, day, month, year, century;
};

static volatile uint64_t rtc_anchor_epoch;   /* civil time when we read it */
static volatile uint32_t rtc_anchor_ticks;
static int rtc_valid;

static uint8_t cmos_read(uint8_t reg)
{
    outb(RTC_INDEX_PORT, reg);
    return inb(RTC_DATA_PORT);
}

/* One snapshot of the date registers, refused when it may have been torn. */
static int rtc_read_fields(struct rtc_fields *f)
{
    if (cmos_read(RTC_REG_STATUS_A) & RTC_A_UIP) return -1;

    f->sec     = cmos_read(RTC_REG_SECONDS);
    f->min     = cmos_read(RTC_REG_MINUTES);
    f->hour    = cmos_read(RTC_REG_HOURS);
    f->day     = cmos_read(RTC_REG_DAY);
    f->month   = cmos_read(RTC_REG_MONTH);
    f->year    = cmos_read(RTC_REG_YEAR);
    f->century = cmos_read(RTC_REG_CENTURY);

    if (cmos_read(RTC_REG_STATUS_A) & RTC_A_UIP) return -1;
    return 0;
}

static uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)((v & 0x0f) + (v >> 4) * 10);
}

/* Days from 1970-01-01 to y-m-d in the proleptic Gregorian calendar. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);              /* [0, 399] */
    const unsigned mp  = (m + 9) % 12;                           /* Mar = 0 */
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;             /* [0, 365] */
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* [0, 146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

/* Registers -> seconds since the epoch.  -1 when the date is not a date. */
static int rtc_decode(const struct rtc_fields *f, uint64_t *out_epoch)
{
    uint8_t status_b = cmos_read(RTC_REG_STATUS_B);
    int binary = (status_b & RTC_B_BINARY) != 0;
    int h24    = (status_b & RTC_B_24H) != 0;

    unsigned sec = f->sec, min = f->min, hour = f->hour;
    unsigned day = f->day, month = f->month, year2 = f->year;

    if (!h24) {
        unsigned pm = hour & 0x80;          /* bit 7 is the PM flag in 12h mode */
        hour &= 0x7f;
        if (!binary) hour = bcd_to_bin((uint8_t)hour);
        hour %= 12;
        if (pm) hour += 12;
    }
    if (!binary) {
        if (h24) hour = bcd_to_bin((uint8_t)hour);
        sec   = bcd_to_bin((uint8_t)sec);
        min   = bcd_to_bin((uint8_t)min);
        day   = bcd_to_bin((uint8_t)day);
        month = bcd_to_bin((uint8_t)month);
        year2 = bcd_to_bin((uint8_t)year2);
    }

    unsigned century = binary ? f->century : bcd_to_bin(f->century);
    unsigned year;
    if (century >= 19 && century <= 21) {
        year = century * 100u + year2;
    } else {
        /* No (usable) century register: QEMU fills 0x32, real chips may not. */
        year = (year2 < 70u) ? 2000u + year2 : 1900u + year2;
    }

    if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 ||
        month < 1 || month > 12 || year < 1970 || year > 2199)
        return -1;

    *out_epoch = (uint64_t)days_from_civil((int64_t)year, month, day) * 86400ull +
                 hour * 3600u + min * 60u + sec;
    return 0;
}

void rtc_init(void)
{
    rtc_valid = 0;

    for (int attempt = 0; attempt < 8; attempt++) {
        struct rtc_fields a, b;
        if (rtc_read_fields(&a) != 0 || rtc_read_fields(&b) != 0) continue;
        if (a.sec != b.sec || a.min != b.min || a.hour != b.hour ||
            a.day != b.day || a.month != b.month || a.year != b.year)
            continue;                       /* rolled over between the reads */

        uint64_t epoch;
        if (rtc_decode(&b, &epoch) != 0) return;   /* not a date — retrying won't help */
        rtc_anchor_epoch = epoch;
        rtc_anchor_ticks = timer_ticks_get();
        rtc_valid = 1;
        return;
    }
}

int rtc_have_time(void)
{
    return rtc_valid;
}

uint64_t rtc_epoch_usec(void)
{
    if (!rtc_valid) return 0;
    uint32_t elapsed = timer_ticks_get() - rtc_anchor_ticks;   /* wraps cleanly */
    return rtc_anchor_epoch * 1000000ull + (uint64_t)elapsed * RTC_USEC_PER_TICK;
}

uint64_t rtc_epoch_sec(void)
{
    return rtc_epoch_usec() / 1000000ull;
}
