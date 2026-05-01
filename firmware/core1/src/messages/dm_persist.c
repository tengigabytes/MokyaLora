/* dm_persist.c — see dm_persist.h. */

#include "dm_persist.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "timers.h"

#include "c1_storage.h"
#include "../../lfs/lfs.h"
#include "mokya_trace.h"

/* ── State ─────────────────────────────────────────────────────────── */

#define DM_PERSIST_FLUSH_PERIOD_MS  30000u   /* 30 s */
#define DM_PERSIST_PATH_PREFIX      "/.dm_"

static TimerHandle_t s_flush_timer  __attribute__((section(".psram_bss")));
static volatile bool s_inited       __attribute__((section(".psram_bss")));

/* SWD-readable diag globals — .bss-resident for SWD coherency. */
volatile uint32_t g_dm_persist_saves    __attribute__((used)) = 0u;
volatile uint32_t g_dm_persist_loads    __attribute__((used)) = 0u;
volatile uint32_t g_dm_persist_failures __attribute__((used)) = 0u;
volatile int32_t  g_dm_persist_last_err __attribute__((used)) = 0;
volatile uint32_t g_dm_persist_flushes  __attribute__((used)) = 0u;

/* Single shared serialisation buffer (PSRAM) — size matches the on-disk
 * record. Only one save_peer runs at a time (timer task is single
 * thread), so a static buffer is safe and avoids 1.8 KB of malloc churn
 * per flush tick. Total ~1.9 KB in PSRAM .bss. */
static dm_persist_record_t s_record __attribute__((section(".psram_bss")));

/* ── Helpers ───────────────────────────────────────────────────────── */

static void format_path(char *buf, size_t cap, uint32_t peer_node_id)
{
    snprintf(buf, cap, DM_PERSIST_PATH_PREFIX "%08lx",
             (unsigned long)peer_node_id);
}

static int parse_path(const char *name, uint32_t *out_peer_id)
{
    /* Match exact prefix "/.dm_" then 8 hex digits. lfs_dir_read
     * returns the basename only (without leading slash), so we look
     * for ".dm_XXXXXXXX". */
    const char *p = name;
    if (*p == '/') p++;
    if (strncmp(p, ".dm_", 4) != 0) return -1;
    p += 4;
    /* Exactly 8 hex digits + NUL. */
    if (strlen(p) != 8) return -1;
    uint32_t v = 0u;
    for (int i = 0; i < 8; i++) {
        char c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return -1;
        v = (v << 4) | d;
    }
    if (v == 0u) return -1;
    *out_peer_id = v;
    return 0;
}

/* ── Save / load ───────────────────────────────────────────────────── */

bool dm_persist_save_peer(uint32_t peer_node_id)
{
    if (!c1_storage_is_mounted() || peer_node_id == 0u) return false;

    /* Snapshot under the dm_store lock. */
    if (!dm_store_snapshot_peer(peer_node_id, &s_record)) {
        /* Peer disappeared (evicted between dirty-set and now); not an
         * error — just leave the on-disk file stale and return. */
        return false;
    }

    char path[DM_PERSIST_PATH_MAX];
    format_path(path, sizeof(path), peer_node_id);

    c1_storage_file_t f;
    if (!c1_storage_open(&f, path,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)) {
        g_dm_persist_failures++;
        g_dm_persist_last_err = LFS_ERR_IO;
        return false;
    }
    int rc = c1_storage_write(&f, &s_record, sizeof(s_record));
    bool closed = c1_storage_close(&f);
    if (rc != (int)sizeof(s_record) || !closed) {
        g_dm_persist_failures++;
        g_dm_persist_last_err = (rc < 0) ? rc : LFS_ERR_IO;
        TRACE("dm_p", "save_fail",
              "peer=%lu rc=%d closed=%u",
              (unsigned long)peer_node_id, rc, (unsigned)closed);
        return false;
    }
    g_dm_persist_saves++;
    TRACE("dm_p", "saved", "peer=%lu count=%u",
          (unsigned long)peer_node_id, (unsigned)s_record.count);
    return true;
}

uint32_t dm_persist_flush_now(void)
{
    if (!c1_storage_is_mounted()) return 0;
    uint32_t ids[DM_STORE_PEER_CAP];
    uint8_t n = dm_store_pop_dirty(ids, DM_STORE_PEER_CAP);
    uint32_t saved = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (dm_persist_save_peer(ids[i])) saved++;
    }
    if (n > 0) g_dm_persist_flushes++;
    return saved;
}

/* Load context — passed through c1_storage_walk callback. */
typedef struct {
    uint32_t loaded;
} load_ctx_t;

static bool load_one_cb(const char *name, uint32_t size, void *vctx)
{
    load_ctx_t *ctx = (load_ctx_t *)vctx;
    uint32_t peer_id = 0u;
    if (parse_path(name, &peer_id) < 0) return true;   /* not a /.dm_ file, continue */
    if (size != sizeof(dm_persist_record_t)) {
        TRACE("dm_p", "size_mismatch",
              "name=%s sz=%u expect=%u",
              name, (unsigned)size,
              (unsigned)sizeof(dm_persist_record_t));
        return true;
    }
    /* lfs_info.name is just the basename — rebuild the full path. */
    char path[DM_PERSIST_PATH_MAX];
    format_path(path, sizeof(path), peer_id);
    c1_storage_file_t f;
    if (!c1_storage_open(&f, path, LFS_O_RDONLY)) return true;
    int rc = c1_storage_read(&f, &s_record, sizeof(s_record));
    (void)c1_storage_close(&f);
    if (rc != (int)sizeof(s_record)) {
        g_dm_persist_failures++;
        g_dm_persist_last_err = (rc < 0) ? rc : LFS_ERR_CORRUPT;
        return true;
    }
    if (dm_store_restore_peer(&s_record)) {
        ctx->loaded++;
        g_dm_persist_loads++;
    }
    return true;
}

uint32_t dm_persist_load_all(void)
{
    if (!c1_storage_is_mounted()) return 0;
    load_ctx_t ctx = { .loaded = 0 };
    (void)c1_storage_walk("/", load_one_cb, &ctx);
    TRACE("dm_p", "load_all", "loaded=%u", (unsigned)ctx.loaded);
    return ctx.loaded;
}

/* ── Flush timer ───────────────────────────────────────────────────── */

static void flush_timer_cb(TimerHandle_t t)
{
    (void)t;
    (void)dm_persist_flush_now();
}

void dm_persist_init(void)
{
    if (s_inited) return;
    s_inited = true;
    if (!c1_storage_is_mounted()) return;

    s_flush_timer = xTimerCreate(
        "dm_p_flush",
        pdMS_TO_TICKS(DM_PERSIST_FLUSH_PERIOD_MS),
        pdTRUE,    /* auto-reload */
        NULL,
        flush_timer_cb);
    if (s_flush_timer != NULL) {
        xTimerStart(s_flush_timer, 0);
    }
}
