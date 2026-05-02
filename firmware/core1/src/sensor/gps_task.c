/* gps_task.c — Drives teseo_liv3fl at a fixed 100 ms drain cadence.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gps_task.h"

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#ifdef MOKYA_GPS_DUMMY_NMEA
#include "gps_dummy.h"
#else
#include "teseo_liv3fl.h"
#include "ipc_shared_layout.h"
#include "ipc_protocol.h"
#include "wall_clock.h"
#endif

#define DRAIN_PERIOD_MS  100u
#define INIT_RETRY_MS    500u

/* TEMP commissioning — flip via `#define MOKYA_COMMISSION_RF_DEBUG` just
 * below and reflash ONCE; Teseo NVM persists the mask across boots. */
/* #define MOKYA_COMMISSION_RF_DEBUG 1 */
#ifdef MOKYA_COMMISSION_RF_DEBUG
volatile uint32_t g_rf_commission_rc;
#endif

#ifndef MOKYA_GPS_DUMMY_NMEA

/* Writes the current Teseo fix into IpcGpsBuf if the parsed state is
 * newer than the last commit (tracked via sentence_count). Called after
 * every teseo_poll() so latency from NMEA → shared SRAM is ≤ 100 ms. */
static uint32_t s_last_sentence_count;

static void ipc_gps_commit_fix(void)
{
    const teseo_state_t *s = teseo_get_state();
    if (!s->fix_valid || s->fix_quality == 0) return;
    if (s->sentence_count == s_last_sentence_count) return;
    if (s->utc_date == 0 || s->utc_time == 0) return;
    s_last_sentence_count = s->sentence_count;

    /* Convert Teseo utc_date (ddmmyy) + utc_time (hhmmss) to Unix epoch. */
    wall_clock_civil_t civil;
    civil.year   = (uint16_t)(2000u + s->utc_date % 100u);
    civil.month  = (uint8_t)((s->utc_date / 100u) % 100u);
    civil.day    = (uint8_t)(s->utc_date / 10000u);
    civil.hour   = (uint8_t)(s->utc_time / 10000u);
    civil.minute = (uint8_t)((s->utc_time / 100u) % 100u);
    civil.second = (uint8_t)(s->utc_time % 100u);
    civil.reserved = 0u;
    uint64_t epoch = wall_clock_civil_to_unix(&civil);

    /* Write structured fix into the currently-owned IpcGpsBuf slot,
     * then flip write_idx to publish the snapshot atomically. */
    IpcGpsBuf *gps = &g_ipc_shared.gps_buf;
    uint8_t cur = gps->write_idx;
    IpcGpsFixSlot *slot = &gps->slot[cur];
    slot->unix_epoch  = (uint32_t)epoch;
    slot->lat_e7      = s->lat_e7;
    slot->lon_e7      = s->lon_e7;
    slot->altitude_mm = (int32_t)s->altitude_m * 1000;
    slot->hdop_x100   = (uint16_t)((uint32_t)s->hdop_x10 * 10u);
    slot->sat_count   = s->num_sats;
    slot->fix_quality = s->fix_quality;
    slot->fix_valid   = s->fix_valid ? 1u : 0u;
    slot->_pad[0] = slot->_pad[1] = slot->_pad[2] = 0u;
    __atomic_store_n(&gps->write_idx, (uint8_t)(cur ^ 1u), __ATOMIC_RELEASE);

    /* Sync Core 1 soft wall clock from GNSS. */
    wall_clock_set_unix_from_gnss(epoch);
}

static void gps_task(void *pv)
{
    (void)pv;

    /* Retry init forever — a late-plug or transient bus stall during
     * boot shouldn't permanently disable the task. */
    while (!teseo_init()) {
        vTaskDelay(pdMS_TO_TICKS(INIT_RETRY_MS));
    }

    /* TEMP one-shot RF-debug commissioning. When enabled at build time,
     * writes CDB 231 to switch on $PSTMRF / $PSTMNOISE / $PSTMNOTCHSTATUS /
     * $PSTMCPU / $GPGST permanently (NVM). Remove after one successful
     * boot so subsequent boots don't re-save NVM. */
#ifdef MOKYA_COMMISSION_RF_DEBUG
    extern volatile uint32_t g_rf_commission_rc;
    vTaskDelay(pdMS_TO_TICKS(2000));
    g_rf_commission_rc = teseo_enable_rf_debug_messages(true) ? 1u : 0u;
#endif

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        (void)teseo_poll();
        ipc_gps_commit_fix();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(DRAIN_PERIOD_MS));
    }
}
#endif /* !MOKYA_GPS_DUMMY_NMEA */

bool gps_task_start(UBaseType_t priority)
{
#ifdef MOKYA_GPS_DUMMY_NMEA
    /* Dev-only dummy NMEA injector — bypasses Teseo entirely. */
    return gps_dummy_start(priority);
#else
    /* 512 words (2 KB). Drain buffer and line accumulator are static
     * (no stack cost). NMEA parsing calls atof/strtoul which draw a
     * ~400-byte libc frame — still comfortably within budget. */
    BaseType_t rc = xTaskCreate(gps_task, "gps", 512, NULL, priority, NULL);
    return rc == pdPASS;
#endif
}
