/* gps_dummy.c — Fixed-position NMEA GGA injector for IpcGpsBuf validation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Builds one $GPGGA sentence per second with a runtime-computed checksum
 * and a slowly-incrementing UTC field, then commits it into the shared-SRAM
 * IpcGpsBuf double-buffer.
 */

#include "gps_dummy.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ipc_shared_layout.h"
#include "ipc_protocol.h"

#define DUMMY_PERIOD_MS 1000u

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

/* Format one GGA sentence into `out` (caller-sized ≥ 96 B).
 * Returns total byte length, including trailing CRLF. */
static int format_gga(char *out, size_t cap, uint32_t seconds_since_boot)
{
    /* Wrap to 24 h so the field never overflows. */
    uint32_t t = seconds_since_boot % 86400u;
    unsigned hh = (unsigned)(t / 3600u);
    unsigned mm = (unsigned)((t / 60u) % 60u);
    unsigned ss = (unsigned)(t % 60u);

    /* GGA body without leading '$' and trailing '*XX\r\n'. */
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

    if (body_len <= 0 || (size_t)body_len >= sizeof(body)) {
        return -1;
    }

    uint8_t cs = nmea_checksum(body, (size_t)body_len);

    int total = snprintf(out, cap, "$%s*%02X\r\n", body, cs);
    if (total <= 0 || (size_t)total >= cap) {
        return -1;
    }
    return total;
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
    for (;;) {
        char sentence[128];
        int n = format_gga(sentence, sizeof(sentence), s_seconds++);

        if (n > 0 && (size_t)n <= sizeof(gps->buf[0])) {
            uint8_t next = (uint8_t)(gps->write_idx ^ 1u);
            memcpy(gps->buf[next], sentence, (size_t)n);
            gps->len[next] = (uint8_t)n;
            __atomic_store_n(&gps->write_idx, next, __ATOMIC_RELEASE);
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
