/* history_persist.c — see history_persist.h. */

#include "history_persist.h"

#include <string.h>

#include "FreeRTOS.h"
#include "timers.h"

#include "c1_storage.h"
#include "../../lfs/lfs.h"
#include "mokya_trace.h"

/* ── State ─────────────────────────────────────────────────────────── */

#define HISTORY_PERSIST_FLUSH_PERIOD_MS  (5u * 60u * 1000u)   /* 5 min */

static TimerHandle_t s_flush_timer __attribute__((section(".psram_bss")));
static volatile bool s_inited      __attribute__((section(".psram_bss")));

/* PSRAM serialisation buffer — ~1.55 KB. Single timer task is the
 * only writer so a static buffer is safe. */
static history_persist_record_t s_record __attribute__((section(".psram_bss")));

/* SWD-coherent diag globals (.bss). */
volatile uint32_t g_history_persist_saves    __attribute__((used)) = 0u;
volatile uint32_t g_history_persist_loads    __attribute__((used)) = 0u;
volatile uint32_t g_history_persist_failures __attribute__((used)) = 0u;
volatile int32_t  g_history_persist_last_err __attribute__((used)) = 0;
volatile uint16_t g_history_persist_last_count   __attribute__((used)) = 0u;
volatile uint16_t g_history_persist_loaded_count __attribute__((used)) = 0u;

/* SWD-trigger for synchronous flush (test harness). */
volatile uint32_t g_history_persist_flush_request __attribute__((used)) = 0u;
volatile uint32_t g_history_persist_flush_done    __attribute__((used)) = 0u;

/* ── Save / load ───────────────────────────────────────────────────── */

bool history_persist_flush_now(void)
{
    if (!c1_storage_is_mounted()) return false;
    if (!metrics_history_pop_dirty()) return true;   /* nothing new */

    memset(&s_record, 0, sizeof(s_record));
    s_record.magic   = HISTORY_PERSIST_MAGIC;
    s_record.version = HISTORY_PERSIST_VERSION;
    metrics_history_snapshot(s_record.ring, &s_record.head, &s_record.count);

    c1_storage_file_t f;
    if (!c1_storage_open(&f, HISTORY_PERSIST_PATH,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)) {
        g_history_persist_failures++;
        g_history_persist_last_err = LFS_ERR_IO;
        return false;
    }
    int rc = c1_storage_write(&f, &s_record, sizeof(s_record));
    bool closed = c1_storage_close(&f);
    if (rc != (int)sizeof(s_record) || !closed) {
        g_history_persist_failures++;
        g_history_persist_last_err = (rc < 0) ? rc : LFS_ERR_IO;
        TRACE("hist_p", "save_fail", "rc=%d closed=%u",
              rc, (unsigned)closed);
        return false;
    }
    g_history_persist_saves++;
    g_history_persist_last_count = s_record.count;
    TRACE("hist_p", "saved", "count=%u head=%u",
          (unsigned)s_record.count, (unsigned)s_record.head);
    return true;
}

uint16_t history_persist_load(void)
{
    if (!c1_storage_is_mounted()) return 0;
    if (!c1_storage_exists(HISTORY_PERSIST_PATH)) return 0;

    c1_storage_file_t f;
    if (!c1_storage_open(&f, HISTORY_PERSIST_PATH, LFS_O_RDONLY)) {
        g_history_persist_failures++;
        g_history_persist_last_err = LFS_ERR_IO;
        return 0;
    }
    int rc = c1_storage_read(&f, &s_record, sizeof(s_record));
    (void)c1_storage_close(&f);
    if (rc != (int)sizeof(s_record)) {
        g_history_persist_failures++;
        g_history_persist_last_err = (rc < 0) ? rc : LFS_ERR_CORRUPT;
        return 0;
    }
    if (s_record.magic != HISTORY_PERSIST_MAGIC
        || s_record.version != HISTORY_PERSIST_VERSION) {
        g_history_persist_failures++;
        g_history_persist_last_err = LFS_ERR_CORRUPT;
        return 0;
    }
    metrics_history_restore(s_record.ring, s_record.head, s_record.count);
    g_history_persist_loads++;
    g_history_persist_loaded_count = s_record.count;
    TRACE("hist_p", "loaded", "count=%u head=%u",
          (unsigned)s_record.count, (unsigned)s_record.head);
    return s_record.count;
}

/* ── Flush timer ───────────────────────────────────────────────────── */

static void flush_timer_cb(TimerHandle_t t)
{
    (void)t;
    (void)history_persist_flush_now();
}

void history_persist_init(void)
{
    if (s_inited) return;
    s_inited = true;
    if (!c1_storage_is_mounted()) return;

    s_flush_timer = xTimerCreate(
        "hist_p_flush",
        pdMS_TO_TICKS(HISTORY_PERSIST_FLUSH_PERIOD_MS),
        pdTRUE,
        NULL,
        flush_timer_cb);
    if (s_flush_timer != NULL) {
        xTimerStart(s_flush_timer, 0);
    }
}
