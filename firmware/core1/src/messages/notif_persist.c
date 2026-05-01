/* notif_persist.c — see notif_persist.h. */

#include "notif_persist.h"

#include <string.h>

#include "FreeRTOS.h"
#include "timers.h"

#include "c1_storage.h"
#include "../../lfs/lfs.h"
#include "mokya_trace.h"

#define NOTIF_PERSIST_FLUSH_PERIOD_MS  5000u   /* 5 s debounce */

static TimerHandle_t s_flush_timer __attribute__((section(".psram_bss")));
static volatile bool s_inited      __attribute__((section(".psram_bss")));
static notif_persist_record_t s_record __attribute__((section(".psram_bss")));

/* SWD-coherent diag globals. */
volatile uint32_t g_notif_persist_saves    __attribute__((used)) = 0u;
volatile uint32_t g_notif_persist_loads    __attribute__((used)) = 0u;
volatile uint32_t g_notif_persist_failures __attribute__((used)) = 0u;
volatile int32_t  g_notif_persist_last_err __attribute__((used)) = 0;

/* SWD synchronous-flush trigger for tests. */
volatile uint32_t g_notif_persist_flush_request __attribute__((used)) = 0u;
volatile uint32_t g_notif_persist_flush_done    __attribute__((used)) = 0u;

/* IEEE-CRC32 (poly 0xEDB88320) — small table-less variant. Settings
 * are <100 B so per-byte loop is fine; saves ~1 KB of code that a
 * one-shot persistence path doesn't justify. */
static uint32_t crc32_calc(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int j = 0; j < 8; j++) {
            uint32_t mask = -(c & 1u);
            c = (c >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~c;
}

bool notif_persist_flush_now(void)
{
    if (!c1_storage_is_mounted()) return false;
    if (!notification_settings_consume_dirty()) return true;

    notif_settings_t *live = notification_get_settings();
    s_record.magic    = NOTIF_PERSIST_MAGIC;
    s_record.version  = NOTIF_PERSIST_VERSION;
    s_record.reserved = 0u;
    s_record.body     = *live;
    s_record.crc32    = crc32_calc((const uint8_t *)&s_record.body,
                                    sizeof(s_record.body));

    c1_storage_file_t f;
    if (!c1_storage_open(&f, NOTIF_PERSIST_PATH,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)) {
        g_notif_persist_failures++;
        g_notif_persist_last_err = LFS_ERR_IO;
        notification_settings_dirty();   /* re-queue */
        return false;
    }
    int rc = c1_storage_write(&f, &s_record, sizeof(s_record));
    bool closed = c1_storage_close(&f);
    if (rc != (int)sizeof(s_record) || !closed) {
        g_notif_persist_failures++;
        g_notif_persist_last_err = (rc < 0) ? rc : LFS_ERR_IO;
        TRACE("notif_p", "save_fail", "rc=%d closed=%u",
              rc, (unsigned)closed);
        notification_settings_dirty();
        return false;
    }
    g_notif_persist_saves++;
    TRACE("notif_p", "saved", "ev=0x%04x ch_mask=0x%02x",
          (unsigned)live->event_modes,
          (unsigned)live->channel_valid_mask);
    return true;
}

bool notif_persist_load(void)
{
    if (!c1_storage_is_mounted()) return false;
    if (!c1_storage_exists(NOTIF_PERSIST_PATH)) return false;

    c1_storage_file_t f;
    if (!c1_storage_open(&f, NOTIF_PERSIST_PATH, LFS_O_RDONLY)) {
        g_notif_persist_failures++;
        g_notif_persist_last_err = LFS_ERR_IO;
        return false;
    }
    int rc = c1_storage_read(&f, &s_record, sizeof(s_record));
    (void)c1_storage_close(&f);
    if (rc != (int)sizeof(s_record)) {
        g_notif_persist_failures++;
        g_notif_persist_last_err = (rc < 0) ? rc : LFS_ERR_CORRUPT;
        return false;
    }
    if (s_record.magic   != NOTIF_PERSIST_MAGIC ||
        s_record.version != NOTIF_PERSIST_VERSION) {
        g_notif_persist_failures++;
        g_notif_persist_last_err = LFS_ERR_CORRUPT;
        return false;
    }
    uint32_t want = crc32_calc((const uint8_t *)&s_record.body,
                                sizeof(s_record.body));
    if (want != s_record.crc32) {
        g_notif_persist_failures++;
        g_notif_persist_last_err = LFS_ERR_CORRUPT;
        return false;
    }
    if (s_record.body.version != NOTIF_SETTINGS_VERSION) {
        /* future schema bump → fall back to defaults */
        g_notif_persist_failures++;
        g_notif_persist_last_err = LFS_ERR_CORRUPT;
        return false;
    }
    notif_settings_t *live = notification_get_settings();
    *live = s_record.body;
    g_notif_persist_loads++;
    TRACE("notif_p", "loaded", "ev=0x%04x master=%u",
          (unsigned)live->event_modes,
          (unsigned)live->master_enable);
    return true;
}

static void flush_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (g_notif_persist_flush_request != g_notif_persist_flush_done) {
        if (notif_persist_flush_now()) {
            g_notif_persist_flush_done = g_notif_persist_flush_request;
        }
        return;
    }
    (void)notif_persist_flush_now();
}

void notif_persist_init(void)
{
    if (s_inited) return;
    s_inited = true;
    if (!c1_storage_is_mounted()) return;

    (void)notif_persist_load();

    s_flush_timer = xTimerCreate(
        "notif_p_flush",
        pdMS_TO_TICKS(NOTIF_PERSIST_FLUSH_PERIOD_MS),
        pdTRUE,
        NULL,
        flush_timer_cb);
    if (s_flush_timer != NULL) {
        xTimerStart(s_flush_timer, 0);
    }
}
