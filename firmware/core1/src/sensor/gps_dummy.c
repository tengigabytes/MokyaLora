/* gps_dummy.c — Fixed-position NMEA GGA injector for IpcGpsBuf validation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Builds one $GPGGA sentence per second with a runtime-computed checksum
 * and a slowly-incrementing UTC field, then commits it into the shared-SRAM
 * IpcGpsBuf double-buffer.
 */

#include "gps_dummy.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ipc_shared_layout.h"
#include "ipc_protocol.h"

#define DUMMY_PERIOD_MS 500u  /* Alternates GGA / RMC each tick → 1 Hz per type */

/* Taipei fixed coordinates (user-supplied for M3.5 dummy validation). */
#define DUMMY_LAT_DEG    25
#define DUMMY_LAT_MIN_X1E5  312618u   /* (25.052103 - 25) * 60 * 100000 = 312618 */
#define DUMMY_LON_DEG    121
#define DUMMY_LON_MIN_X1E5  3444234u  /* (121.574039 - 121) * 60 * 100000 = 3444234 */
#define DUMMY_ALT_DM     500          /* 50.0 m, expressed in decimetres for printf */
#define DUMMY_FIX_QUALITY 1           /* 1 = GPS fix */
#define DUMMY_NUM_SATS    8
#define DUMMY_HDOP_X10    9           /* 0.9 */

static uint32_t s_seconds;            /* Monotonic seconds since task start */

static uint8_t nmea_checksum(const char *body, size_t len)
{
    uint8_t cs = 0;
    for (size_t i = 0; i < len; ++i) {
        cs ^= (uint8_t)body[i];
    }
    return cs;
}

/* Common helper: wrap body with leading '$' and trailing '*XX\r\n'. */
static int finalize_nmea(char *out, size_t cap, const char *body, size_t body_len)
{
    uint8_t cs = nmea_checksum(body, body_len);
    int total = snprintf(out, cap, "$%s*%02X\r\n", body, cs);
    if (total <= 0 || (size_t)total >= cap) return -1;
    return total;
}

/* GGA — position + fix quality + altitude. Time only (no date). */
static int format_gga(char *out, size_t cap, uint32_t seconds_since_boot)
{
    uint32_t t = seconds_since_boot % 86400u;
    unsigned hh = (unsigned)(t / 3600u);
    unsigned mm = (unsigned)((t / 60u) % 60u);
    unsigned ss = (unsigned)(t % 60u);

    char body[96];
    int body_len = snprintf(body, sizeof(body),
        "GPGGA,%02u%02u%02u.00,"
        "%02u%02u.%05lu,N,"
        "%03u%02u.%05lu,E,"
        "%u,%02u,%u.%u,"
        "%d.%d,M,0.0,M,,",
        hh, mm, ss,
        (unsigned)DUMMY_LAT_DEG,
        (unsigned)(DUMMY_LAT_MIN_X1E5 / 100000u),
        (unsigned long)(DUMMY_LAT_MIN_X1E5 % 100000u),
        (unsigned)DUMMY_LON_DEG,
        (unsigned)(DUMMY_LON_MIN_X1E5 / 100000u),
        (unsigned long)(DUMMY_LON_MIN_X1E5 % 100000u),
        (unsigned)DUMMY_FIX_QUALITY,
        (unsigned)DUMMY_NUM_SATS,
        (unsigned)(DUMMY_HDOP_X10 / 10u),
        (unsigned)(DUMMY_HDOP_X10 % 10u),
        (int)(DUMMY_ALT_DM / 10),
        (int)(DUMMY_ALT_DM % 10));

    if (body_len <= 0 || (size_t)body_len >= sizeof(body)) return -1;
    return finalize_nmea(out, cap, body, (size_t)body_len);
}

/* RMC — recommended-minimum sentence with date. Meshtastic's lookForLocation
 * rejects fixes whose `date` is too old (line 1772 of GPS.cpp), so we have
 * to publish a date even though the dummy clock has no real wall time. Use
 * a fixed 2026-04-26 — the date is just a TinyGPS validity gate, not a
 * value that gets surfaced to the user. */
static int format_rmc(char *out, size_t cap, uint32_t seconds_since_boot)
{
    uint32_t t = seconds_since_boot % 86400u;
    unsigned hh = (unsigned)(t / 3600u);
    unsigned mm = (unsigned)((t / 60u) % 60u);
    unsigned ss = (unsigned)(t % 60u);

    char body[96];
    int body_len = snprintf(body, sizeof(body),
        "GPRMC,%02u%02u%02u.00,A,"
        "%02u%02u.%05lu,N,"
        "%03u%02u.%05lu,E,"
        "0.00,0.00,260426,,,A",
        hh, mm, ss,
        (unsigned)DUMMY_LAT_DEG,
        (unsigned)(DUMMY_LAT_MIN_X1E5 / 100000u),
        (unsigned long)(DUMMY_LAT_MIN_X1E5 % 100000u),
        (unsigned)DUMMY_LON_DEG,
        (unsigned)(DUMMY_LON_MIN_X1E5 / 100000u),
        (unsigned long)(DUMMY_LON_MIN_X1E5 % 100000u));

    if (body_len <= 0 || (size_t)body_len >= sizeof(body)) return -1;
    return finalize_nmea(out, cap, body, (size_t)body_len);
}

static void gps_dummy_task(void *pv)
{
    (void)pv;

    IpcGpsBuf *gps = &g_ipc_shared.gps_buf;

    /* Reset to a known state — boot_magic init zeroed the region, but be
     * explicit so re-spawning the task (future debug) starts clean. */
    gps->write_idx = 0;
    gps->len[0] = 0;
    gps->len[1] = 0;

    TickType_t last = xTaskGetTickCount();
    bool send_rmc = false;
    for (;;) {
        char sentence[128];
        int n;
        if (send_rmc) {
            n = format_rmc(sentence, sizeof(sentence), s_seconds);
        } else {
            n = format_gga(sentence, sizeof(sentence), s_seconds);
            ++s_seconds;  /* Bump the second once per GGA/RMC pair. */
        }
        send_rmc = !send_rmc;

        if (n > 0 && (size_t)n <= sizeof(gps->buf[0])) {
            /* Per ipc_protocol.h IpcGpsBuf contract: write_idx is the slot
             * Core 1 currently owns; reader (Core 0) reads buf[write_idx ^ 1].
             * So we write into buf[write_idx], then flip write_idx — that
             * makes the just-finished slot visible to the reader as
             * buf[(new write_idx) ^ 1]. */
            uint8_t cur = gps->write_idx;
            memcpy(gps->buf[cur], sentence, (size_t)n);
            gps->len[cur] = (uint8_t)n;
            __atomic_store_n(&gps->write_idx, (uint8_t)(cur ^ 1u), __ATOMIC_RELEASE);
        }

        vTaskDelayUntil(&last, pdMS_TO_TICKS(DUMMY_PERIOD_MS));
    }
}

bool gps_dummy_start(UBaseType_t priority)
{
    /* 384 words (1.5 KB). snprintf draws ~400 B of libc frame + a 96 B
     * body buffer + 128 B sentence buffer — comfortably within budget. */
    BaseType_t rc = xTaskCreate(gps_dummy_task, "gps_dummy", 384,
                                NULL, priority, NULL);
    return rc == pdPASS;
}
