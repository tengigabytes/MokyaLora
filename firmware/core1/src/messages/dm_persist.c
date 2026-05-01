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
volatile uint32_t g_dm_persist_orphans_unlinked __attribute__((used)) = 0u;

/* Test-only SWD-trigger for synchronous flush. Bridge_task polls and
 * calls dm_persist_flush_now when request != done. Avoids 30 s timer
 * latency in scripted tests. */
volatile uint32_t g_dm_persist_flush_request __attribute__((used)) = 0u;
volatile uint32_t g_dm_persist_flush_done    __attribute__((used)) = 0u;

/* Shared text-buffer cap for load-time diag mirror, outbound-DM
 * injection, and indexed-reader output (all share g_dm_persist_last_*
 * BSS buffers — see comments on each trigger below). Matches
 * dm_msg_t.text[200]. */
#define DM_PERSIST_DIAG_TEXT_MAX  200u

/* T1.A2 — outbound DM injection trigger.
 * Test workflow: SWD-write text bytes INTO g_dm_persist_last_text
 * (which is .bss-zeroed each cold boot), set the trigger fields below,
 * then bump g_dm_inject_outbound_request. Bridge_task reads the text
 * out of g_dm_persist_last_text and calls dm_store_ingest_outbound.
 * Reusing the diag buffer saves 200 B of .bss; safe because outbound
 * injection happens BEFORE the post-reset load_one_cb diag write
 * (resets clear .bss in between). */
volatile uint32_t g_dm_inject_outbound_peer       __attribute__((used)) = 0u;
volatile uint32_t g_dm_inject_outbound_packet_id  __attribute__((used)) = 0u;
volatile uint8_t  g_dm_inject_outbound_want_ack   __attribute__((used)) = 0u;
volatile uint16_t g_dm_inject_outbound_text_len   __attribute__((used)) = 0u;
volatile uint32_t g_dm_inject_outbound_request    __attribute__((used)) = 0u;
volatile uint32_t g_dm_inject_outbound_done       __attribute__((used)) = 0u;

/* T1.A2 — ack-state update trigger. Drives dm_store_update_ack. */
volatile uint32_t g_dm_inject_ack_packet_id __attribute__((used)) = 0u;
volatile uint8_t  g_dm_inject_ack_state     __attribute__((used)) = 0u;
volatile uint32_t g_dm_inject_ack_request   __attribute__((used)) = 0u;
volatile uint32_t g_dm_inject_ack_done      __attribute__((used)) = 0u;

/* T1.B3 — DM indexed reader. Test scripts SWD-write
 * g_dm_query_peer_id + g_dm_query_idx, then bump g_dm_query_request.
 * Bridge_task polls and writes the queried dm_msg_t fields into the
 * SAME g_dm_persist_last_* globals used by the load-time diag mirror
 * (saves 232 B; safe because the test serializes "load → query → query
 * → query" — only one read is "live" at a time). ok=1 on success. */
volatile uint32_t g_dm_query_peer_id  __attribute__((used)) = 0u;
volatile uint8_t  g_dm_query_idx      __attribute__((used)) = 0u;
volatile uint32_t g_dm_query_request  __attribute__((used)) = 0u;
volatile uint32_t g_dm_query_done     __attribute__((used)) = 0u;
volatile uint8_t  g_dm_query_ok       __attribute__((used)) = 0u;

/* Byte-coherent SWD diag of the last successfully-loaded peer's
 * OLDEST message (i.e. ring index 0).  Lives in regular .bss because
 * peer_slot_t storage is in PSRAM which isn't SWD-coherent without
 * explicit cache flush.  Captured by load_one_cb after each
 * dm_store_restore_peer succeeds — the test script reads these to
 * verify byte-perfect round-trip. T1.A2 extends to all dm_msg_t fields. */
volatile uint32_t g_dm_persist_last_peer       __attribute__((used)) = 0u;
volatile uint8_t  g_dm_persist_last_count      __attribute__((used)) = 0u;
volatile uint8_t  g_dm_persist_last_outbound   __attribute__((used)) = 0u;
volatile uint16_t g_dm_persist_last_text_len   __attribute__((used)) = 0u;
volatile uint8_t  g_dm_persist_last_text[DM_PERSIST_DIAG_TEXT_MAX]
                                                __attribute__((used));
/* T1.A2 — full dm_msg_t fields for the captured oldest message. */
volatile uint32_t g_dm_persist_last_seq        __attribute__((used)) = 0u;
volatile uint32_t g_dm_persist_last_epoch      __attribute__((used)) = 0u;
volatile uint32_t g_dm_persist_last_packet_id  __attribute__((used)) = 0u;
volatile uint8_t  g_dm_persist_last_ack_state  __attribute__((used)) = 0u;
volatile uint32_t g_dm_persist_last_ack_epoch  __attribute__((used)) = 0u;
volatile int16_t  g_dm_persist_last_rx_snr_x4  __attribute__((used)) = 0;
volatile int16_t  g_dm_persist_last_rx_rssi    __attribute__((used)) = 0;
volatile uint8_t  g_dm_persist_last_hop_limit  __attribute__((used)) = 0u;
volatile uint8_t  g_dm_persist_last_hop_start  __attribute__((used)) = 0u;
volatile uint8_t  g_dm_persist_last_want_ack   __attribute__((used)) = 0u;
volatile uint8_t  g_dm_persist_last_unread     __attribute__((used)) = 0u;
volatile uint32_t g_dm_persist_last_activity_ms __attribute__((used)) = 0u;

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
    if (!dm_store_restore_peer(&s_record)) {
        /* Magic/version mismatch or peer_id==0 — record as a corruption
         * failure so tests (and field debugging) can detect that the
         * on-disk file was rejected. The opposite-side rejection path
         * (s_record.magic check below) is dead code now that we surface
         * the same condition here, but kept for paranoia. */
        g_dm_persist_failures++;
        g_dm_persist_last_err = LFS_ERR_CORRUPT;
        TRACE("dm_p", "restore_reject",
              "name=%s magic=%#lx ver=%lu peer=%lu",
              name, (unsigned long)s_record.magic,
              (unsigned long)s_record.version,
              (unsigned long)s_record.peer_node_id);
    } else {
        ctx->loaded++;
        g_dm_persist_loads++;
        /* Capture the oldest message into the SWD-coherent diag.
         * Ring layout: head = next-write, count = entries, oldest is at
         * (head - count) mod 8. */
        int oldest = (int)s_record.head - (int)s_record.count;
        if (oldest < 0) oldest += (int)DM_STORE_MSGS_PER;
        if (s_record.count > 0u && oldest >= 0
            && oldest < (int)DM_STORE_MSGS_PER) {
            const dm_msg_t *m = &s_record.ring[oldest];
            uint16_t L = m->text_len > DM_PERSIST_DIAG_TEXT_MAX
                            ? (uint16_t)DM_PERSIST_DIAG_TEXT_MAX
                            : m->text_len;
            g_dm_persist_last_peer        = s_record.peer_node_id;
            g_dm_persist_last_count       = s_record.count;
            g_dm_persist_last_outbound    = m->outbound ? 1u : 0u;
            g_dm_persist_last_text_len    = L;
            for (uint16_t i = 0; i < L; i++) {
                g_dm_persist_last_text[i] = (uint8_t)m->text[i];
            }
            /* T1.A2: full field set. */
            g_dm_persist_last_seq         = m->seq;
            g_dm_persist_last_epoch       = m->epoch;
            g_dm_persist_last_packet_id   = m->packet_id;
            g_dm_persist_last_ack_state   = m->ack_state;
            g_dm_persist_last_ack_epoch   = m->ack_epoch;
            g_dm_persist_last_rx_snr_x4   = m->rx_snr_x4;
            g_dm_persist_last_rx_rssi     = m->rx_rssi;
            g_dm_persist_last_hop_limit   = m->hop_limit;
            g_dm_persist_last_hop_start   = m->hop_start;
            g_dm_persist_last_want_ack    = m->want_ack;
            g_dm_persist_last_unread      = s_record.unread;
            g_dm_persist_last_activity_ms = s_record.last_activity_ms;
        }
    }
    return true;
}

/* Cleanup pass: walk /.dm_* files, unlink any whose peer_id is no
 * longer in dm_store (evicted during load_all because >8 files
 * existed and the last_activity_ms ordering chose 8 newer ones). */
typedef struct {
    uint32_t unlinked;
} cleanup_ctx_t;

static bool cleanup_one_cb(const char *name, uint32_t size, void *vctx)
{
    cleanup_ctx_t *ctx = (cleanup_ctx_t *)vctx;
    (void)size;
    uint32_t peer_id = 0u;
    if (parse_path(name, &peer_id) < 0) return true;
    /* If dm_store doesn't have this peer (it was evicted during load),
     * the on-disk file is an orphan. Unlink it. */
    dm_peer_summary_t s;
    if (dm_store_get_peer(peer_id, &s)) return true;   /* still resident */
    char path[DM_PERSIST_PATH_MAX];
    format_path(path, sizeof(path), peer_id);
    if (c1_storage_unlink(path)) {
        ctx->unlinked++;
        TRACE("dm_p", "evict_unlink", "peer=%lu",
              (unsigned long)peer_id);
    }
    return true;
}

uint32_t dm_persist_load_all(void)
{
    if (!c1_storage_is_mounted()) return 0;
    load_ctx_t ctx = { .loaded = 0 };
    (void)c1_storage_walk("/", load_one_cb, &ctx);
    /* Cleanup orphans (peers whose files exist but didn't end up in
     * dm_store after eviction).  This keeps the on-disk file count
     * bounded by DM_STORE_PEER_CAP across reboots. */
    cleanup_ctx_t cctx = { .unlinked = 0 };
    (void)c1_storage_walk("/", cleanup_one_cb, &cctx);
    g_dm_persist_orphans_unlinked = cctx.unlinked;
    TRACE("dm_p", "load_all", "loaded=%u orphans_unlinked=%u",
          (unsigned)ctx.loaded, (unsigned)cctx.unlinked);
    return ctx.loaded;
}

/* ── T1 SWD-trigger poll (called from bridge_task) ────────────────── */

void dm_persist_poll_swd_triggers(void)
{
    /* Outbound DM injection — text comes from g_dm_persist_last_text. */
    {
        uint32_t req = g_dm_inject_outbound_request;
        if (req != 0u && req != g_dm_inject_outbound_done) {
            uint16_t len = g_dm_inject_outbound_text_len;
            if (len > DM_PERSIST_DIAG_TEXT_MAX) len = DM_PERSIST_DIAG_TEXT_MAX;
            /* Snapshot the text out of the shared diag buffer before
             * dm_store_ingest_outbound runs (which holds the dm_store
             * mutex; cheap to copy locally first). */
            uint8_t scratch[DM_PERSIST_DIAG_TEXT_MAX];
            for (uint16_t i = 0; i < len; i++) {
                scratch[i] = g_dm_persist_last_text[i];
            }
            dm_store_ingest_outbound(g_dm_inject_outbound_peer,
                                     g_dm_inject_outbound_packet_id,
                                     g_dm_inject_outbound_want_ack ? true : false,
                                     scratch, len);
            g_dm_inject_outbound_done = req;
        }
    }
    /* Ack-state update. */
    {
        uint32_t req = g_dm_inject_ack_request;
        if (req != 0u && req != g_dm_inject_ack_done) {
            dm_store_update_ack(g_dm_inject_ack_packet_id,
                                (dm_ack_state_t)g_dm_inject_ack_state);
            g_dm_inject_ack_done = req;
        }
    }
    /* DM indexed reader — fills the same diag globals the load path
     * uses, so the test reads via the existing read_diag() helper. */
    {
        uint32_t req = g_dm_query_request;
        if (req != 0u && req != g_dm_query_done) {
            dm_msg_t m;
            bool ok = dm_store_get_msg(g_dm_query_peer_id,
                                       g_dm_query_idx, &m);
            if (ok) {
                uint16_t L = m.text_len > DM_PERSIST_DIAG_TEXT_MAX
                                 ? (uint16_t)DM_PERSIST_DIAG_TEXT_MAX
                                 : m.text_len;
                g_dm_persist_last_peer        = g_dm_query_peer_id;
                g_dm_persist_last_count       = g_dm_query_idx;   /* idx echo */
                g_dm_persist_last_outbound    = m.outbound ? 1u : 0u;
                g_dm_persist_last_text_len    = L;
                for (uint16_t i = 0; i < L; i++) {
                    g_dm_persist_last_text[i] = (uint8_t)m.text[i];
                }
                g_dm_persist_last_seq         = m.seq;
                g_dm_persist_last_epoch       = m.epoch;
                g_dm_persist_last_packet_id   = m.packet_id;
                g_dm_persist_last_ack_state   = m.ack_state;
                g_dm_persist_last_ack_epoch   = m.ack_epoch;
                g_dm_persist_last_rx_snr_x4   = m.rx_snr_x4;
                g_dm_persist_last_rx_rssi     = m.rx_rssi;
                g_dm_persist_last_hop_limit   = m.hop_limit;
                g_dm_persist_last_hop_start   = m.hop_start;
                g_dm_persist_last_want_ack    = m.want_ack;
            }
            g_dm_query_ok   = ok ? 1u : 0u;
            g_dm_query_done = req;
        }
    }
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
