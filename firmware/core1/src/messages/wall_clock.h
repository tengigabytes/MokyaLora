/* wall_clock.h — Software wall-clock for MokyaLora.
 *
 * Rev A has no RTC chip; this module keeps a software clock anchored to
 * a Unix epoch that the user (or future GNSS sync) sets. Internally it
 * tracks `(set_unix, set_tick)` and reports
 *   now_unix = set_unix + (xTaskGetTickCount() - set_tick) * tick_ms / 1000
 *
 * Persisted to /clock.bin every 5 min so a reboot recovers approximately
 * the right wall time (with up to 5 min loss + a few seconds per power
 * cycle of drift). When GNSS time becomes available, wall_clock_set()
 * is called to re-anchor.
 *
 * Returns 0xFFFF from wall_clock_minute_of_day() when never set, so
 * downstream logic (Quiet Hours, status bar) can show "—:—".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_WALL_CLOCK_H
#define MOKYA_CORE1_WALL_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set the current wall-clock from a UTC Unix epoch. Marks the clock
 * as synced. Called from the manual setter UI; also from the GNSS
 * sync path when wall_clock_gnss_sync_is_enabled() returns true. */
void     wall_clock_set_unix(uint64_t unix_secs);

/* GNSS-driven set: only applies when GNSS sync is enabled (user
 * setting). Returns true if the time was applied. */
bool     wall_clock_set_unix_from_gnss(uint64_t unix_secs);

/* Bool: has the clock ever been set? */
bool     wall_clock_is_synced(void);

/* Current UTC Unix epoch. 0 if never set. */
uint64_t wall_clock_now_unix(void);

/* Local Unix epoch = UTC + tz_offset. 0 if never set. */
uint64_t wall_clock_now_local(void);

/* Time-zone offset in minutes from UTC (e.g. +480 = UTC+8 = Taiwan). */
int16_t  wall_clock_get_tz_offset_min(void);
void     wall_clock_set_tz_offset_min(int16_t offset_min);

/* GNSS auto-sync toggle. When disabled, GNSS-derived time is dropped
 * and only the manual setter or other explicit sources can write the
 * clock. */
bool     wall_clock_gnss_sync_is_enabled(void);
void     wall_clock_gnss_sync_set_enabled(bool en);

/* Minute-of-day in local time (0..1439). 0xFFFF if not synced. */
uint16_t wall_clock_minute_of_day(void);

/* Minute-of-day from a specific Unix epoch + tz offset. Used for tests. */
uint16_t wall_clock_minute_of_day_from(uint64_t unix_secs,
                                       int16_t  tz_offset_min);

/* Decompose a Unix epoch (UTC, no leap seconds) into civil
 * date/time fields. Tested for years 1970..2099. */
typedef struct {
    uint16_t year;          /* e.g. 2026 */
    uint8_t  month;         /* 1..12 */
    uint8_t  day;           /* 1..31 */
    uint8_t  hour;          /* 0..23 */
    uint8_t  minute;        /* 0..59 */
    uint8_t  second;        /* 0..59 */
    uint8_t  reserved;
} wall_clock_civil_t;

void     wall_clock_unix_to_civil(uint64_t unix_secs, wall_clock_civil_t *out);
uint64_t wall_clock_civil_to_unix(const wall_clock_civil_t *c);

/* Persist module init — loads /clock.bin (if present) and starts the
 * 5-min flush timer. Must run after c1_storage_init(). */
void     wall_clock_init(void);

/* Force-flush the current time + TZ + GNSS-sync flag to LFS. */
bool     wall_clock_flush_now(void);

#ifdef __cplusplus
}
#endif
#endif /* MOKYA_CORE1_WALL_CLOCK_H */
