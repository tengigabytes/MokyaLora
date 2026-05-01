/* metrics/history.c — see metrics/history.h.
 *
 * Ring layout: head points to the next free slot; count saturates at
 * METRICS_HISTORY_LEN once filled. Index 0 from newest = head-1; index
 * count-1 = oldest valid sample.
 *
 * SoC and SNR are decoupled: SoC comes from BQ25622 polled at sample time;
 * SNR is whatever RX has dropped into s_last_snr_x4 since the previous
 * tick. Ring is owned by the FreeRTOS soft timer service task only after
 * init; readers (LVGL render path) are advised by an int change-seq —
 * single-writer + 32-bit aligned read makes torn reads benign for a
 * cosmetic chart.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "history.h"

#include <string.h>

#include "FreeRTOS.h"
#include "timers.h"

#include "bq25622.h"
#include "phoneapi_cache.h"
#include "mokya_trace.h"

/* ── Storage ────────────────────────────────────────────────────────── */

static metrics_sample_t s_ring[METRICS_HISTORY_LEN];
static uint16_t         s_head;          /* next write slot */
static uint16_t         s_count;         /* 0..LEN          */
static volatile int16_t s_last_snr_x4 = METRICS_HISTORY_NONE;
static volatile uint32_t s_change_seq;
/* Phase 5 — single dirty bit. Set on every take_sample, drained by
 * history_persist's 5-min timer. Bool because the on-disk format is
 * the whole ring; per-slot tracking adds no value. */
static volatile bool    s_dirty;

/* SWD-readable diag — last sample's per-field value. Updated on every
 * timer tick for host tests that want to verify the ring is accumulating
 * without walking the PSRAM cache or the .bss ring directly. */
volatile uint16_t g_history_count           __attribute__((used)) = 0u;
volatile int16_t  g_history_last_soc_pct    __attribute__((used)) = METRICS_HISTORY_NONE;
volatile int16_t  g_history_last_snr_x10    __attribute__((used)) = METRICS_HISTORY_NONE;
volatile int16_t  g_history_last_air_tx_x10 __attribute__((used)) = METRICS_HISTORY_NONE;

static TimerHandle_t s_timer;
static bool          s_init_done;

/* ── Sampling ───────────────────────────────────────────────────────── */

static int16_t soc_from_vbat(uint32_t vbat_mv)
{
    /* Linear 3300..4200 mV → 0..100 % (matches boot_home_view.c:134). */
    int v = ((int)vbat_mv - 3300) * 100 / (4200 - 3300);
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    return (int16_t)v;
}

static int16_t snr_x10_from_x4(int16_t snr_x4)
{
    if (snr_x4 == METRICS_HISTORY_NONE) return METRICS_HISTORY_NONE;
    /* x4 → x10 with rounding. x10 = x4 × 10 / 4 = x4 × 2.5. */
    int32_t v = ((int32_t)snr_x4 * 10) / 4;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static void take_sample(void)
{
    metrics_sample_t s;
    s.soc_pct         = METRICS_HISTORY_NONE;
    s.last_rx_snr_x10 = snr_x10_from_x4(s_last_snr_x4);
    s.air_tx_pct_x10  = METRICS_HISTORY_NONE;

    const bq25622_state_t *b = bq25622_get_state();
    if (b != NULL && b->online && b->vbat_mv > 0u) {
        s.soc_pct = soc_from_vbat(b->vbat_mv);
    }

    /* Self's air_util_tx_pct comes from its own NodeInfo broadcasts as
     * decoded by cascade FR_TAG_NODE_INFO → DeviceMetrics. Sentinel
     * 0xFF = "not yet seen" → leave as METRICS_HISTORY_NONE. */
    phoneapi_my_info_t mi;
    phoneapi_node_t    self;
    if (phoneapi_cache_get_my_info(&mi) &&
        phoneapi_cache_get_node_by_id(mi.my_node_num, &self) &&
        self.air_util_tx_pct != 0xFFu) {
        s.air_tx_pct_x10 = (int16_t)((int32_t)self.air_util_tx_pct * 10);
    }

    s_ring[s_head] = s;
    s_head = (uint16_t)((s_head + 1u) % METRICS_HISTORY_LEN);
    if (s_count < METRICS_HISTORY_LEN) s_count++;
    s_change_seq++;
    s_dirty = true;

    g_history_count           = s_count;
    g_history_last_soc_pct    = s.soc_pct;
    g_history_last_snr_x10    = s.last_rx_snr_x10;
    g_history_last_air_tx_x10 = s.air_tx_pct_x10;

    TRACE("metrics", "sample",
          "soc=%d,snr_x10=%d,air_x10=%d,n=%u",
          (int)s.soc_pct, (int)s.last_rx_snr_x10,
          (int)s.air_tx_pct_x10, (unsigned)s_count);
}

static void timer_cb(TimerHandle_t t)
{
    (void)t;
    take_sample();
}

/* ── Public API ─────────────────────────────────────────────────────── */

void metrics_history_init(void)
{
    if (s_init_done) return;

    memset(s_ring, 0, sizeof(s_ring));
    s_head = 0u;
    s_count = 0u;
    s_change_seq = 0u;

    s_timer = xTimerCreate("metrics_hist",
                           pdMS_TO_TICKS(METRICS_HISTORY_PERIOD_SECS * 1000u),
                           pdTRUE,           /* auto-reload */
                           NULL,
                           timer_cb);
    if (s_timer != NULL) {
        xTimerStart(s_timer, 0);
    }

    /* Drop one sample immediately so the chart's first visible cell
     * matches "now" instead of waiting 30 s for the user to wonder
     * whether the page is broken. */
    take_sample();

    s_init_done = true;
    TRACE_BARE("metrics", "init");
}

uint16_t metrics_history_count(void)
{
    return s_count;
}

bool metrics_history_get(uint16_t idx_from_newest, metrics_sample_t *out)
{
    if (out == NULL) return false;
    if (idx_from_newest >= s_count) return false;

    /* head points to next-write; newest valid is head-1. */
    uint16_t newest = (uint16_t)((s_head + METRICS_HISTORY_LEN - 1u)
                                 % METRICS_HISTORY_LEN);
    uint16_t slot   = (uint16_t)((newest + METRICS_HISTORY_LEN - idx_from_newest)
                                 % METRICS_HISTORY_LEN);
    *out = s_ring[slot];
    return true;
}

void metrics_history_note_rx_snr_x4(int16_t snr_x4)
{
    if (snr_x4 == METRICS_HISTORY_NONE) return;
    s_last_snr_x4 = snr_x4;
}

uint32_t metrics_history_change_seq(void)
{
    return s_change_seq;
}

/* ── Persistence bridge (Phase 5) ────────────────────────────────── */

void metrics_history_snapshot(metrics_sample_t *buf,
                              uint16_t *out_head,
                              uint16_t *out_count)
{
    if (buf != NULL) {
        memcpy(buf, s_ring, sizeof(s_ring));
    }
    if (out_head)  *out_head  = s_head;
    if (out_count) *out_count = s_count;
}

void metrics_history_restore(const metrics_sample_t *buf,
                             uint16_t head,
                             uint16_t count)
{
    if (buf == NULL) return;
    memcpy(s_ring, buf, sizeof(s_ring));
    s_head  = (uint16_t)(head % METRICS_HISTORY_LEN);
    s_count = (count > METRICS_HISTORY_LEN) ? METRICS_HISTORY_LEN : count;
    s_change_seq++;
    /* Restore is NOT dirty — file already matches. */
    s_dirty = false;
    /* Update SWD diag mirrors so post-load tests see consistent values. */
    g_history_count = s_count;
    if (s_count > 0u) {
        uint16_t newest = (uint16_t)((s_head + METRICS_HISTORY_LEN - 1u)
                                     % METRICS_HISTORY_LEN);
        g_history_last_soc_pct    = s_ring[newest].soc_pct;
        g_history_last_snr_x10    = s_ring[newest].last_rx_snr_x10;
        g_history_last_air_tx_x10 = s_ring[newest].air_tx_pct_x10;
    }
}

bool metrics_history_pop_dirty(void)
{
    bool d = s_dirty;
    s_dirty = false;
    return d;
}
