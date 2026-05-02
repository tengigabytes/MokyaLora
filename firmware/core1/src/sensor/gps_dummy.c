/* gps_dummy.c — Fixed-position structured fix injector for IpcGpsBuf validation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Writes one IpcGpsFixSlot per second with a slowly-incrementing unix_epoch
 * and fixed Taipei coordinates into the shared-SRAM IpcGpsBuf double-buffer.
 * Replaces the earlier NMEA sentence injector (M3.5) now that IpcGpsBuf holds
 * structured fix data instead of raw NMEA bytes (M5).
 */

#include "gps_dummy.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ipc_shared_layout.h"
#include "ipc_protocol.h"

#define DUMMY_PERIOD_MS   1000u       /* One structured fix per second */
#define DUMMY_UNIX_BASE   1745625600u /* 2026-04-26 00:00:00 UTC */

/* Taipei fixed coordinates (25.052103 N, 121.574039 E). */
#define DUMMY_LAT_E7     250521030
#define DUMMY_LON_E7    1215740390
#define DUMMY_ALT_MM     50000       /* 50 m above MSL */
#define DUMMY_HDOP_X100  90          /* HDOP 0.90 */
#define DUMMY_SAT_COUNT  8
#define DUMMY_FIX_QUALITY 1          /* GPS fix */

static uint32_t s_seconds;

static void gps_dummy_task(void *pv)
{
    (void)pv;

    IpcGpsBuf *gps = &g_ipc_shared.gps_buf;

    /* Reset to a known state — explicit in case task is ever re-spawned. */
    gps->write_idx = 0;
    memset(gps->slot, 0, sizeof(gps->slot));

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        /* Write structured fix into the currently-owned slot, then flip
         * write_idx to publish.  Reader (Core 0) sees slot[write_idx ^ 1]
         * as the most-recently-published snapshot. */
        uint8_t cur = gps->write_idx;
        IpcGpsFixSlot *slot = &gps->slot[cur];
        slot->unix_epoch  = DUMMY_UNIX_BASE + s_seconds++;
        slot->lat_e7      = DUMMY_LAT_E7;
        slot->lon_e7      = DUMMY_LON_E7;
        slot->altitude_mm = DUMMY_ALT_MM;
        slot->hdop_x100   = DUMMY_HDOP_X100;
        slot->sat_count   = DUMMY_SAT_COUNT;
        slot->fix_quality = DUMMY_FIX_QUALITY;
        slot->fix_valid   = 1u;
        memset(slot->_pad, 0, sizeof(slot->_pad));
        __atomic_store_n(&gps->write_idx, (uint8_t)(cur ^ 1u), __ATOMIC_RELEASE);

        vTaskDelayUntil(&last, pdMS_TO_TICKS(DUMMY_PERIOD_MS));
    }
}

bool gps_dummy_start(UBaseType_t priority)
{
    /* 256 words (1 KB). No NMEA formatting, no libc frame needed. */
    BaseType_t rc = xTaskCreate(gps_dummy_task, "gps_dummy", 256,
                                NULL, priority, NULL);
    return rc == pdPASS;
}
