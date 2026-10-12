#include <asm/arch.h>
#include <asm/io.h>

#include <stdbool.h>

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS   0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

static uint8_t cmos(uint8_t reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

struct rtc_time {
    uint8_t sec, min, hour, day, month, year;
};

static void rtc_snapshot(struct rtc_time *t) {
    t->sec   = cmos(RTC_SECONDS);
    t->min   = cmos(RTC_MINUTES);
    t->hour  = cmos(RTC_HOURS);
    t->day   = cmos(RTC_DAY);
    t->month = cmos(RTC_MONTH);
    t->year  = cmos(RTC_YEAR);
}

static uint8_t bcd(uint8_t v) {
    return (v & 0x0f) + (v >> 4) * 10;
}

/* seconds since 1970 of a date in the gregorian calendar */
static uint64_t days_from_civil(uint64_t y, uint64_t m, uint64_t d) {
    if (m <= 2) y--;

    uint64_t era = y / 400;
    uint64_t yoe = y - era * 400;
    uint64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + doe - 719468;
}

/* the cmos clock, which qemu sets to the host's utc. read twice until two reads agree: it may tick in between */
uint64_t rtc_read_epoch(void) {
    struct rtc_time a, b;

    do {
        while (cmos(RTC_STATUS_A) & 0x80) {} /* an update is in progress */

        rtc_snapshot(&a);

        while (cmos(RTC_STATUS_A) & 0x80) {}

        rtc_snapshot(&b);
    } while (a.sec != b.sec || a.min != b.min || a.hour != b.hour || a.day != b.day);

    uint8_t status = cmos(RTC_STATUS_B);
    bool pm = a.hour & 0x80;

    if (!(status & 0x04)) { /* bcd */
        a.sec = bcd(a.sec);
        a.min = bcd(a.min);
        a.hour = bcd(a.hour & 0x7f) | (pm ? 0x80 : 0);
        a.day = bcd(a.day);
        a.month = bcd(a.month);
        a.year = bcd(a.year);
    }

    if (!(status & 0x02) && pm) { /* 12 hour clock */
        a.hour = ((a.hour & 0x7f) % 12) + 12;
    }

    uint64_t days = days_from_civil(2000 + a.year, a.month, a.day);

    return days * 86400 + a.hour * 3600ULL + a.min * 60ULL + a.sec;
}
