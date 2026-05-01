/* waypoint_persist.c — see waypoint_persist.h. */

#include "waypoint_persist.h"

#include <string.h>

#include "FreeRTOS.h"
#include "timers.h"

#include "c1_storage.h"
#include "../../lfs/lfs.h"
#include "mokya_trace.h"

/* ── State ─────────────────────────────────────────────────────────── */

#define WAYPOINT_PERSIST_FLUSH_PERIOD_MS  30000u   /* 30 s */

static TimerHandle_t s_flush_timer __attribute__((section(".psram_bss")));
static volatile bool s_inited      __attribute__((section(".psram_bss")));

/* Single shared serialisation buffer (PSRAM, ~1.4 KB). Only one save
 * at a time (timer task is single-thread). */
static waypoint_persist_record_t s_record __attribute__((section(".psram_bss")));

/* SWD-coherent diag globals — .bss-resident (PSRAM not SWD-coherent
 * without explicit cache flush). */
volatile uint32_t g_waypoint_persist_saves    __attribute__((used)) = 0u;
volatile uint32_t g_waypoint_persist_loads    __attribute__((used)) = 0u;
volatile uint32_t g_waypoint_persist_failures __attribute__((used)) = 0u;
volatile int32_t  g_waypoint_persist_last_err __attribute__((used)) = 0;
volatile uint32_t g_waypoint_persist_count    __attribute__((used)) = 0u;   /* Last save's count */

/* Test-only SWD-trigger for synchronous flush. */
volatile uint32_t g_waypoint_persist_flush_request __attribute__((used)) = 0u;
volatile uint32_t g_waypoint_persist_flush_done    __attribute__((used)) = 0u;

/* SWD-coherent snapshot of the first waypoint restored by
 * load_all (ring slot 0). Used by tests for byte-perfect verification.
 * .bss because phoneapi cache lives in PSRAM. */
volatile uint32_t g_waypoint_persist_last_loaded_id     __attribute__((used)) = 0u;
volatile int32_t  g_waypoint_persist_last_loaded_lat_e7 __attribute__((used)) = 0;
volatile int32_t  g_waypoint_persist_last_loaded_lon_e7 __attribute__((used)) = 0;
volatile uint8_t  g_waypoint_persist_last_loaded_name[PHONEAPI_WAYPOINT_NAME_MAX]
                                                        __attribute__((used));

/* ── Save / load ───────────────────────────────────────────────────── */

bool waypoint_persist_flush_now(void)
{
    if (!c1_storage_is_mounted()) return false;
    /* Skip if nothing changed since last save. Pop_dirty is a peek+clear. */
    if (!phoneapi_waypoints_pop_dirty()) return true;

    /* Snapshot the table: build s_record from phoneapi_waypoints_take_at. */
    memset(&s_record, 0, sizeof(s_record));
    s_record.magic   = WAYPOINT_PERSIST_MAGIC;
    s_record.version = WAYPOINT_PERSIST_VERSION;
    uint32_t total = phoneapi_waypoints_count();
    if (total > PHONEAPI_WAYPOINTS_CAP) total = PHONEAPI_WAYPOINTS_CAP;
    uint32_t n = 0;
    for (uint32_t i = 0; i < total; i++) {
        phoneapi_waypoint_t e;
        if (!phoneapi_waypoints_take_at(i, &e)) continue;
        if (!e.in_use) continue;
        s_record.table[n++] = e;
    }
    s_record.count = n;

    c1_storage_file_t f;
    if (!c1_storage_open(&f, WAYPOINT_PERSIST_PATH,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)) {
        g_waypoint_persist_failures++;
        g_waypoint_persist_last_err = LFS_ERR_IO;
        return false;
    }
    int rc = c1_storage_write(&f, &s_record, sizeof(s_record));
    bool closed = c1_storage_close(&f);
    if (rc != (int)sizeof(s_record) || !closed) {
        g_waypoint_persist_failures++;
        g_waypoint_persist_last_err = (rc < 0) ? rc : LFS_ERR_IO;
        TRACE("wp_p", "save_fail", "rc=%d closed=%u",
              rc, (unsigned)closed);
        return false;
    }
    g_waypoint_persist_saves++;
    g_waypoint_persist_count = n;
    TRACE("wp_p", "saved", "count=%u", (unsigned)n);
    return true;
}

uint32_t waypoint_persist_load_all(void)
{
    if (!c1_storage_is_mounted()) return 0;
    if (!c1_storage_exists(WAYPOINT_PERSIST_PATH)) return 0;

    c1_storage_file_t f;
    if (!c1_storage_open(&f, WAYPOINT_PERSIST_PATH, LFS_O_RDONLY)) {
        g_waypoint_persist_failures++;
        g_waypoint_persist_last_err = LFS_ERR_IO;
        return 0;
    }
    int rc = c1_storage_read(&f, &s_record, sizeof(s_record));
    (void)c1_storage_close(&f);
    if (rc != (int)sizeof(s_record)) {
        g_waypoint_persist_failures++;
        g_waypoint_persist_last_err = (rc < 0) ? rc : LFS_ERR_CORRUPT;
        TRACE("wp_p", "load_short", "rc=%d", rc);
        return 0;
    }
    if (s_record.magic != WAYPOINT_PERSIST_MAGIC
        || s_record.version != WAYPOINT_PERSIST_VERSION) {
        g_waypoint_persist_failures++;
        g_waypoint_persist_last_err = LFS_ERR_CORRUPT;
        TRACE("wp_p", "magic_bad", "magic=%#lx ver=%lu",
              (unsigned long)s_record.magic,
              (unsigned long)s_record.version);
        return 0;
    }

    uint32_t loaded = 0;
    if (s_record.count > PHONEAPI_WAYPOINTS_CAP) s_record.count = PHONEAPI_WAYPOINTS_CAP;
    for (uint32_t i = 0; i < s_record.count; i++) {
        phoneapi_waypoint_t *e = &s_record.table[i];
        if (!e->in_use || e->id == 0u) continue;
        phoneapi_waypoints_upsert(e);
        if (loaded == 0u) {
            /* Capture first-loaded waypoint into SWD-coherent diag. */
            g_waypoint_persist_last_loaded_id     = e->id;
            g_waypoint_persist_last_loaded_lat_e7 = e->lat_e7;
            g_waypoint_persist_last_loaded_lon_e7 = e->lon_e7;
            for (size_t k = 0; k < PHONEAPI_WAYPOINT_NAME_MAX; k++) {
                g_waypoint_persist_last_loaded_name[k] = (uint8_t)e->name[k];
            }
        }
        loaded++;
    }
    /* upsert sets dirty — for restore we don't want to immediately
     * re-flush, so clear the dirty bit. */
    (void)phoneapi_waypoints_pop_dirty();
    g_waypoint_persist_loads += loaded;
    TRACE("wp_p", "load_all", "loaded=%u", (unsigned)loaded);
    return loaded;
}

/* ── Flush timer ───────────────────────────────────────────────────── */

static void flush_timer_cb(TimerHandle_t t)
{
    (void)t;
    (void)waypoint_persist_flush_now();
}

void waypoint_persist_init(void)
{
    if (s_inited) return;
    s_inited = true;
    if (!c1_storage_is_mounted()) return;

    s_flush_timer = xTimerCreate(
        "wp_p_flush",
        pdMS_TO_TICKS(WAYPOINT_PERSIST_FLUSH_PERIOD_MS),
        pdTRUE,
        NULL,
        flush_timer_cb);
    if (s_flush_timer != NULL) {
        xTimerStart(s_flush_timer, 0);
    }
}
