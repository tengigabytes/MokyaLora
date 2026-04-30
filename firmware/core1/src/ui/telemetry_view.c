/* telemetry_view.c — see telemetry_view.h.
 *
 * Layout (panel 320 × 224, status bar above, hint bar below):
 *   y   0..15  page title  ("F-1 本機遙測 (1/3)")
 *   y  16..207 8 rows × 24 px (one fact per row, or one neighbour per
 *                             row on F-3)
 *
 * Refresh: 1 Hz tick gate (matches boot_home_view) — sensor state
 * updates ~1 Hz and snr/hops change-seq is checked on F-3.
 *
 * Reuse: format_time_ago() borrowed semantics from common LoRa UIs;
 * SoC linear map + charger online check copied from boot_home_view.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "telemetry_view.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "mokya_trace.h"

#include "bq25622.h"
#include "lps22hh.h"
#include "lis2mdl.h"
#include "lsm6dsv16x.h"
#include "teseo_liv3fl.h"
#include "phoneapi_cache.h"
#include "node_alias.h"
#include "nodes_view.h"
#include "history.h"

/* ── Layout ─────────────────────────────────────────────────────────── */

#define TITLE_H       16
#define ROW_H         24
#define MAX_ROWS       8
#define LIST_TOP      TITLE_H
#define PANEL_W      320

/* ── State ──────────────────────────────────────────────────────────── */

typedef enum {
    TELE_PAGE_F1 = 0,   /* 本機遙測 */
    TELE_PAGE_F2,       /* 環境感測 */
    TELE_PAGE_F3,       /* 鄰居資訊 */
    TELE_PAGE_F4,       /* 歷史曲線 */
    TELE_PAGE_COUNT
} tele_page_t;

/* F-4 layout: three stacked charts inside the 320×224 panel. */
#define F4_CHART_X       4
#define F4_CHART_W       (PANEL_W - 8)
#define F4_CHART_TOP     TITLE_H
#define F4_CHART_H       70
#define F4_CHART_GAP     2
#define F4_POINTS        METRICS_HISTORY_LEN   /* 256 */

typedef struct {
    lv_obj_t   *title;
    lv_obj_t   *rows[MAX_ROWS];
    tele_page_t page;
    uint32_t    last_refresh_ms;

    /* F-3 list state */
    uint32_t    f3_cursor;
    uint32_t    f3_scroll_top;
    uint32_t    f3_last_change_seq;

    /* F-4 chart objects + per-series handles, allocated once at create()
     * and hidden when the page is not visible. */
    lv_obj_t       *f4_chart_soc;
    lv_obj_t       *f4_chart_snr;
    lv_obj_t       *f4_chart_chu;
    lv_chart_series_t *f4_ser_soc;
    lv_chart_series_t *f4_ser_snr;
    lv_chart_series_t *f4_ser_chu;
    lv_obj_t       *f4_label_soc;
    lv_obj_t       *f4_label_snr;
    lv_obj_t       *f4_label_chu;
    uint32_t        f4_last_change_seq;
} telemetry_t;

static telemetry_t s;

/* ── Helpers ────────────────────────────────────────────────────────── */

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, int h,
                            lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

static void clear_rows(void)
{
    for (int i = 0; i < MAX_ROWS; ++i) {
        if (s.rows[i]) {
            lv_label_set_text(s.rows[i], "");
            lv_obj_set_style_text_color(s.rows[i],
                ui_color(UI_COLOR_TEXT_PRIMARY), 0);
        }
    }
}

static void set_row(int i, const char *text)
{
    if (i < 0 || i >= MAX_ROWS || s.rows[i] == NULL) return;
    lv_label_set_text(s.rows[i], text);
}

/* "12s" / "3m" / "1h05" / "2d" — for F-3 last_heard column. */
static void format_time_ago(uint32_t age_secs, char *buf, size_t cap)
{
    if (age_secs < 60u) {
        snprintf(buf, cap, "%us", (unsigned)age_secs);
    } else if (age_secs < 3600u) {
        snprintf(buf, cap, "%um", (unsigned)(age_secs / 60u));
    } else if (age_secs < 86400u) {
        snprintf(buf, cap, "%uh%02u",
                 (unsigned)(age_secs / 3600u),
                 (unsigned)((age_secs / 60u) % 60u));
    } else {
        snprintf(buf, cap, "%ud", (unsigned)(age_secs / 86400u));
    }
}

/* ── Page render: F-1 本機遙測 ──────────────────────────────────────── */

static void render_f1(void)
{
    lv_label_set_text(s.title, "F-1 本機遙測 (1/3)");
    clear_rows();

    char buf[64];

    /* Row 0: battery state-of-charge + charging hint. Linear map matches
     * boot_home_view (3300..4200 mV → 0..100). */
    const bq25622_state_t *b = bq25622_get_state();
    if (b == NULL || !b->online) {
        set_row(0, "Battery   charger offline");
    } else {
        int pct = ((int)b->vbat_mv - 3300) * 100 / (4200 - 3300);
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        const char *st = (b->vbus_stat != 0) ? "chg" : "bat";
        if (b->vbat_mv < 500u) {
            /* Mirrors G-4 VBAT_PRESENT_MIN_MV — no battery installed. */
            snprintf(buf, sizeof(buf), "Battery   no battery (USB only)");
        } else {
            snprintf(buf, sizeof(buf), "Battery   %s %d%%  %u.%02uV",
                     st, pct,
                     (unsigned)(b->vbat_mv / 1000),
                     (unsigned)((b->vbat_mv % 1000) / 10));
        }
        set_row(0, buf);

        /* Row 1: rail snapshot */
        snprintf(buf, sizeof(buf), "Rails     vbus=%u.%02uV  vsys=%u.%02uV",
                 (unsigned)(b->vbus_mv / 1000),
                 (unsigned)((b->vbus_mv % 1000) / 10),
                 (unsigned)(b->vsys_mv / 1000),
                 (unsigned)((b->vsys_mv % 1000) / 10));
        set_row(1, buf);

        /* Row 2: currents (signed) */
        snprintf(buf, sizeof(buf), "Current   ibat=%dmA  ibus=%dmA",
                 (int)b->ibat_ma, (int)b->ibus_ma);
        set_row(2, buf);

        /* Row 3: charger die temp */
        int td_int = b->tdie_cx10 / 10;
        int td_dec = (b->tdie_cx10 < 0 ? -b->tdie_cx10 : b->tdie_cx10) % 10;
        snprintf(buf, sizeof(buf), "Chg temp  %d.%01dC  chg_stat=%u",
                 td_int, td_dec, (unsigned)b->chg_stat);
        set_row(3, buf);
    }

    /* Row 4: uptime since FreeRTOS boot (Core 1 only — Core 0 has its
     * own uptime that this view doesn't surface). */
    uint32_t up_s = now_ms() / 1000u;
    unsigned d = up_s / 86400u;
    unsigned h = (up_s % 86400u) / 3600u;
    unsigned m = (up_s % 3600u) / 60u;
    unsigned ss = up_s % 60u;
    if (d > 0u) {
        snprintf(buf, sizeof(buf), "Uptime    %ud %02u:%02u:%02u", d, h, m, ss);
    } else {
        snprintf(buf, sizeof(buf), "Uptime    %02u:%02u:%02u", h, m, ss);
    }
    set_row(4, buf);

    /* Rows 5/6: device-metrics from self's NodeInfo broadcast. The
     * DeviceMetrics sub-decoder in phoneapi_decode.c already fills
     * `channel_util_pct` / `air_util_tx_pct` for every peer that
     * broadcasts NodeInfo with metrics — and self is upserted by the
     * cascade FR_TAG_NODE_INFO path same as a peer. Sentinel 0xFF =
     * "not yet seen", surfaces as `--`. */
    phoneapi_my_info_t mi;
    phoneapi_node_t    self;
    bool have_self = phoneapi_cache_get_my_info(&mi) &&
                     phoneapi_cache_get_node_by_id(mi.my_node_num, &self);
    if (have_self && self.channel_util_pct != 0xFFu) {
        snprintf(buf, sizeof(buf), "Channel%%  %u%%",
                 (unsigned)self.channel_util_pct);
    } else {
        snprintf(buf, sizeof(buf), "Channel%%  --");
    }
    set_row(5, buf);

    if (have_self && self.air_util_tx_pct != 0xFFu) {
        snprintf(buf, sizeof(buf), "Air tx%%   %u%%",
                 (unsigned)self.air_util_tx_pct);
    } else {
        snprintf(buf, sizeof(buf), "Air tx%%   --");
    }
    set_row(6, buf);

    /* Row 7: hint pointing at the F-4 history chart. Dim so the eye
     * reads the live data above first. */
    set_row(7, "          [F-4 趨勢圖另案]");
    if (s.rows[7]) {
        lv_obj_set_style_text_color(s.rows[7],
            ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    }
}

/* ── Page render: F-2 環境感測 ──────────────────────────────────────── */

static void render_f2(void)
{
    lv_label_set_text(s.title, "F-2 環境感測 (2/3)");
    clear_rows();

    char buf[64];

    /* Row 0: barometric pressure (LPS22HH). */
    const lps22hh_state_t *p = lps22hh_get_state();
    if (p && p->online) {
        unsigned hpa_int = p->pressure_hpa_x100 / 100u;
        unsigned hpa_dec = p->pressure_hpa_x100 % 100u;
        snprintf(buf, sizeof(buf), "Pressure  %u.%02u hPa", hpa_int, hpa_dec);
    } else {
        snprintf(buf, sizeof(buf), "Pressure  baro offline");
    }
    set_row(0, buf);

    /* Row 1: barometric-derived altitude vs GPS altitude (cross-check
     * for users in the field). Standard atmosphere h = 44330 *
     * (1 - (P/1013.25)^(1/5.255)) — but the Pico has no FPU/exp; cheap
     * linear approximation around sea level (8.5 m per hPa drop) is
     * good enough for ±100 m, which is all that matters for sanity. */
    if (p && p->online) {
        int dp = (int)p->pressure_hpa_x100 - 101325;  /* hPa×100 above 1013.25 */
        int alt_baro = -dp * 85 / 1000;                /* dp/100 * 8.5 m       */
        const teseo_state_t *t = teseo_get_state();
        if (t && t->fix_valid) {
            snprintf(buf, sizeof(buf), "Altitude  baro=%dm  gps=%dm",
                     alt_baro, (int)t->altitude_m);
        } else {
            snprintf(buf, sizeof(buf), "Altitude  baro=%dm  (no GPS fix)",
                     alt_baro);
        }
    } else {
        snprintf(buf, sizeof(buf), "Altitude  baro offline");
    }
    set_row(1, buf);

    /* Row 2: temperature triplet — three independent dies cross-check
     * each other; large drift between them flags a sensor fault. */
    int t_baro = (p && p->online) ? p->temperature_cx10 : INT16_MIN;
    const lis2mdl_state_t *m = lis2mdl_get_state();
    int t_mag  = (m && m->online) ? m->temperature_cx10 : INT16_MIN;
    const lsm6dsv16x_state_t *im = lsm6dsv16x_get_state();
    int t_imu  = (im && im->online) ? im->temperature_cx10 : INT16_MIN;
    char baro_s[12], mag_s[12], imu_s[12];
    if (t_baro == INT16_MIN) snprintf(baro_s, sizeof(baro_s), "--");
    else snprintf(baro_s, sizeof(baro_s), "%d.%01dC",
                  t_baro / 10,
                  (t_baro < 0 ? -t_baro : t_baro) % 10);
    if (t_mag  == INT16_MIN) snprintf(mag_s, sizeof(mag_s), "--");
    else snprintf(mag_s, sizeof(mag_s), "%d.%01dC",
                  t_mag / 10,
                  (t_mag < 0 ? -t_mag : t_mag) % 10);
    if (t_imu  == INT16_MIN) snprintf(imu_s, sizeof(imu_s), "--");
    else snprintf(imu_s, sizeof(imu_s), "%d.%01dC",
                  t_imu / 10,
                  (t_imu < 0 ? -t_imu : t_imu) % 10);
    snprintf(buf, sizeof(buf), "Temp      baro=%s mag=%s imu=%s",
             baro_s, mag_s, imu_s);
    set_row(2, buf);

    /* Row 3: humidity — Rev A has no humidity sensor. Marked explicitly
     * so the user knows it's a hardware gap, not a firmware miss. */
    set_row(3, "Humidity  n/a (Rev A 無感測器)");

    /* Row 4: magnetic field vector (X/Y/Z in µT). Useful as a heading
     * sanity-check; full compass is a Map-app feature. */
    if (m && m->online) {
        snprintf(buf, sizeof(buf),
                 "Mag (uT)  X=%+d.%01d Y=%+d.%01d Z=%+d.%01d",
                 m->mag_ut_x10[0] / 10,
                 (m->mag_ut_x10[0] < 0 ? -m->mag_ut_x10[0] : m->mag_ut_x10[0]) % 10,
                 m->mag_ut_x10[1] / 10,
                 (m->mag_ut_x10[1] < 0 ? -m->mag_ut_x10[1] : m->mag_ut_x10[1]) % 10,
                 m->mag_ut_x10[2] / 10,
                 (m->mag_ut_x10[2] < 0 ? -m->mag_ut_x10[2] : m->mag_ut_x10[2]) % 10);
    } else {
        snprintf(buf, sizeof(buf), "Mag       offline");
    }
    set_row(4, buf);

    /* Row 5: IMU acceleration (mg, already scaled by driver).  */
    if (im && im->online) {
        snprintf(buf, sizeof(buf),
                 "Accel(mg) X=%+d Y=%+d Z=%+d",
                 (int)im->accel_mg[0],
                 (int)im->accel_mg[1],
                 (int)im->accel_mg[2]);
    } else {
        snprintf(buf, sizeof(buf), "Accel     offline");
    }
    set_row(5, buf);

    /* Row 6: blank spacer */
    /* Row 7: hint */
    set_row(7, "          [Light/Lux 待感測器]");
    if (s.rows[7]) {
        lv_obj_set_style_text_color(s.rows[7],
            ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    }
}

/* ── Page render: F-3 鄰居資訊 ──────────────────────────────────────── */

static void clamp_f3_scroll(uint32_t total)
{
    if (total == 0u) {
        s.f3_cursor = 0u;
        s.f3_scroll_top = 0u;
        return;
    }
    if (s.f3_cursor >= total) s.f3_cursor = total - 1u;
    if (s.f3_cursor < s.f3_scroll_top) s.f3_scroll_top = s.f3_cursor;
    /* F-3 viewport = MAX_ROWS - 1 (row 0 = column header). */
    uint32_t viewport = MAX_ROWS - 1u;
    if (s.f3_cursor >= s.f3_scroll_top + viewport) {
        s.f3_scroll_top = s.f3_cursor - viewport + 1u;
    }
    if (s.f3_scroll_top + viewport > total) {
        s.f3_scroll_top = (total > viewport) ? (total - viewport) : 0u;
    }
}

static void render_f3(void)
{
    uint32_t total = phoneapi_cache_node_count();
    char buf[96];
    snprintf(buf, sizeof(buf), "F-3 鄰居資訊 (3/3)  共 %lu", (unsigned long)total);
    lv_label_set_text(s.title, buf);

    clear_rows();
    clamp_f3_scroll(total);

    /* Row 0: column header. `Nbrs` is the size of this peer's last
     * NeighborInfo broadcast — `--` when we haven't heard one yet. */
    set_row(0, "Peer      SNR     Hops Heard Nbrs");
    if (s.rows[0]) {
        lv_obj_set_style_text_color(s.rows[0],
            ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    }

    if (total == 0u) {
        set_row(1, "  (尚無鄰居)");
        return;
    }

    /* Capture wall-clock-ish reference for "heard N s ago" — we don't
     * have RTC sync (P2-18 patch); fall back to comparing last_heard
     * (epoch) deltas only when at least one entry's last_heard is
     * non-zero. If the cache has zeros, just skip the age column. */
    uint32_t now_epoch = 0u;
    {
        phoneapi_my_info_t mi;
        if (phoneapi_cache_get_my_info(&mi)) {
            /* mi has no current_time field; fall back to "newest
             * last_heard in cache" as the reference clock. Iterate. */
            phoneapi_node_t e;
            for (uint32_t i = 0; i < total; ++i) {
                if (phoneapi_cache_take_node_at(i, &e)) {
                    if (e.last_heard > now_epoch) now_epoch = e.last_heard;
                }
            }
        }
    }

    uint32_t viewport = MAX_ROWS - 1u;   /* rows 1..7 hold data */
    for (uint32_t i = 0; i < viewport; ++i) {
        uint32_t off = s.f3_scroll_top + i;
        int row = (int)(i + 1u);   /* shift past header row 0 */
        if (off >= total) break;
        phoneapi_node_t e;
        if (!phoneapi_cache_take_node_at(off, &e)) continue;

        char nm[16];
        node_alias_format_display(e.num, e.short_name, nm, sizeof(nm));

        char snr_s[12];
        if (e.snr_x100 == INT32_MIN) {
            snprintf(snr_s, sizeof(snr_s), "--");
        } else {
            int snr_int = e.snr_x100 / 100;
            int snr_dec = (e.snr_x100 < 0 ? -e.snr_x100 : e.snr_x100) % 100 / 10;
            snprintf(snr_s, sizeof(snr_s), "%+d.%01ddB", snr_int, snr_dec);
        }

        char hops_s[8];
        if (e.hops_away == 0xFFu) snprintf(hops_s, sizeof(hops_s), "--");
        else snprintf(hops_s, sizeof(hops_s), "%uh", (unsigned)e.hops_away);

        char age_s[12];
        if (now_epoch != 0u && e.last_heard != 0u && e.last_heard <= now_epoch) {
            format_time_ago(now_epoch - e.last_heard, age_s, sizeof(age_s));
        } else {
            snprintf(age_s, sizeof(age_s), "--");
        }

        char nbrs_s[8];
        if (e.last_neighbors.epoch == 0u) {
            snprintf(nbrs_s, sizeof(nbrs_s), "--");
        } else {
            snprintf(nbrs_s, sizeof(nbrs_s), "%u",
                     (unsigned)e.last_neighbors.count);
        }

        bool focused = (off == s.f3_cursor);
        snprintf(buf, sizeof(buf), "%s%-8s %-7s %-4s %-5s %s",
                 focused ? ">" : " ",
                 nm, snr_s, hops_s, age_s, nbrs_s);
        set_row(row, buf);
        if (s.rows[row]) {
            lv_obj_set_style_text_color(s.rows[row],
                focused ? ui_color(UI_COLOR_ACCENT_FOCUS)
                        : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
        }
    }
}

/* ── Page render: F-4 歷史曲線 ──────────────────────────────────────── */

/* Push 256 ring samples into the three lv_charts. The ring stores
 * newest-first via metrics_history_get(0); LVGL chart x-axis is left-to-
 * right oldest-to-newest, so we walk the ring from oldest to newest and
 * use lv_chart_set_next_value() which auto-advances internal cursor. */
static void render_f4(void)
{
    lv_label_set_text(s.title, "F-4 歷史曲線 (4/4)");
    /* The row labels are hidden on F-4; show only the chart axis labels. */
    clear_rows();

    if (s.f4_chart_soc == NULL) return;   /* charts not yet created */

    /* Reset chart cursors so set_next_value re-fills from x=0. */
    lv_chart_set_all_value(s.f4_chart_soc, s.f4_ser_soc, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s.f4_chart_snr, s.f4_ser_snr, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(s.f4_chart_chu, s.f4_ser_chu, LV_CHART_POINT_NONE);

    uint16_t n = metrics_history_count();
    if (n > F4_POINTS) n = F4_POINTS;

    /* Walk oldest → newest. metrics_history_get(idx_from_newest) with
     * idx going n-1 down to 0 visits oldest first. */
    int latest_soc = INT16_MIN;
    int latest_snr = INT16_MIN;
    for (int idx = (int)n - 1; idx >= 0; --idx) {
        metrics_sample_t m;
        if (!metrics_history_get((uint16_t)idx, &m)) continue;

        if (m.soc_pct == METRICS_HISTORY_NONE) {
            lv_chart_set_next_value(s.f4_chart_soc, s.f4_ser_soc,
                                    LV_CHART_POINT_NONE);
        } else {
            lv_chart_set_next_value(s.f4_chart_soc, s.f4_ser_soc,
                                    m.soc_pct);
            latest_soc = m.soc_pct;
        }

        if (m.last_rx_snr_x10 == METRICS_HISTORY_NONE) {
            lv_chart_set_next_value(s.f4_chart_snr, s.f4_ser_snr,
                                    LV_CHART_POINT_NONE);
        } else {
            lv_chart_set_next_value(s.f4_chart_snr, s.f4_ser_snr,
                                    m.last_rx_snr_x10);
            latest_snr = m.last_rx_snr_x10;
        }

        /* Channel-util series stays NONE until P67 self-decode lands. */
        lv_chart_set_next_value(s.f4_chart_chu, s.f4_ser_chu,
                                LV_CHART_POINT_NONE);
    }

    /* Per-chart caption. Use s.f4_label_* (one-line under each chart). */
    char buf[48];
    if (latest_soc == INT16_MIN) {
        snprintf(buf, sizeof(buf), "電量  --%%   (n=%u)", (unsigned)n);
    } else {
        snprintf(buf, sizeof(buf), "電量  %3d%%  (n=%u)",
                 latest_soc, (unsigned)n);
    }
    lv_label_set_text(s.f4_label_soc, buf);

    if (latest_snr == INT16_MIN) {
        snprintf(buf, sizeof(buf), "訊號  --     dB");
    } else {
        int sign  = latest_snr < 0 ? -1 : 1;
        int abs10 = latest_snr * sign;
        snprintf(buf, sizeof(buf), "訊號  %s%d.%d dB",
                 sign < 0 ? "-" : "+", abs10 / 10, abs10 % 10);
    }
    lv_label_set_text(s.f4_label_snr, buf);

    lv_label_set_text(s.f4_label_chu, "空中時間  待 PortNum 67 解碼");
}

/* Toggle F-4 chart visibility based on current page. The labels follow
 * their parent chart automatically. */
static void f4_set_visible(bool show)
{
    if (s.f4_chart_soc == NULL) return;
    lv_obj_t *objs[] = { s.f4_chart_soc, s.f4_chart_snr, s.f4_chart_chu,
                         s.f4_label_soc, s.f4_label_snr, s.f4_label_chu };
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); ++i) {
        if (objs[i] == NULL) continue;
        if (show) lv_obj_clear_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag (objs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Dispatch ───────────────────────────────────────────────────────── */

static void render(void)
{
    /* Charts are only valid on F-4. Hide them otherwise so the row labels
     * for F-1/F-2/F-3 don't get drawn over by stale chart pixels. */
    f4_set_visible(s.page == TELE_PAGE_F4);

    switch (s.page) {
        case TELE_PAGE_F1: render_f1(); break;
        case TELE_PAGE_F2: render_f2(); break;
        case TELE_PAGE_F3: render_f3(); break;
        case TELE_PAGE_F4: render_f4(); break;
        default: break;
    }
}

/* ── Lifecycle ──────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    /* Preserve cross-create UX state: page + F-3 cursor stay where the
     * user left them when the LRU evicted us and we get re-created. */
    tele_page_t saved_page         = s.page;
    uint32_t    saved_f3_cursor    = s.f3_cursor;
    uint32_t    saved_f3_scroll    = s.f3_scroll_top;
    memset(&s, 0, sizeof(s));
    s.page          = saved_page;
    s.f3_cursor     = saved_f3_cursor;
    s.f3_scroll_top = saved_f3_scroll;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.title = make_label(panel, 4, 0, PANEL_W - 8, TITLE_H,
                         ui_color(UI_COLOR_TEXT_SECONDARY));

    for (int i = 0; i < MAX_ROWS; ++i) {
        s.rows[i] = make_label(panel, 4, LIST_TOP + i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }

    /* F-4 charts: built once, hidden by default. Layout = 3 stacks of
     * 70 px chart + 12 px caption inside (TITLE_H + 3 × (70+12) = 16+246
     * but panel is 224). Trim chart_h by reusing the title row's gap. */
    const int caption_h = 12;
    const int chart_h   = 56;       /* 3 × (56+12) = 204 + TITLE_H 16 = 220 */
    int y = F4_CHART_TOP;
    struct {
        lv_obj_t          **chart_p;
        lv_chart_series_t **ser_p;
        lv_obj_t          **label_p;
        int                 y_min, y_max;
    } cfg[3] = {
        { &s.f4_chart_soc, &s.f4_ser_soc, &s.f4_label_soc, 0,    100  },
        { &s.f4_chart_snr, &s.f4_ser_snr, &s.f4_label_snr, -200, 150  },
        { &s.f4_chart_chu, &s.f4_ser_chu, &s.f4_label_chu, 0,    1000 },
    };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *c = lv_chart_create(panel);
        lv_obj_set_pos(c, F4_CHART_X, y);
        lv_obj_set_size(c, F4_CHART_W, chart_h);
        lv_chart_set_type(c, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(c, F4_POINTS);
        lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
        lv_chart_set_div_line_count(c, 3, 4);
        lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y,
                           cfg[i].y_min, cfg[i].y_max);
        lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR);  /* hide dots */
        lv_obj_set_style_bg_color(c, ui_color(UI_COLOR_BG_PRIMARY), 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, ui_color(UI_COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        *cfg[i].chart_p = c;
        *cfg[i].ser_p   = lv_chart_add_series(c,
                              ui_color(UI_COLOR_ACCENT_FOCUS),
                              LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_all_value(c, *cfg[i].ser_p, LV_CHART_POINT_NONE);

        *cfg[i].label_p = make_label(panel, F4_CHART_X + 2,
                                     y + chart_h, F4_CHART_W - 4, caption_h,
                                     ui_color(UI_COLOR_TEXT_SECONDARY));
        y += chart_h + caption_h;
    }

    render();
}

static void destroy(void)
{
    s.title = NULL;
    for (int i = 0; i < MAX_ROWS; ++i) s.rows[i] = NULL;
    s.f4_chart_soc = s.f4_chart_snr = s.f4_chart_chu = NULL;
    s.f4_ser_soc   = s.f4_ser_snr   = s.f4_ser_chu   = NULL;
    s.f4_label_soc = s.f4_label_snr = s.f4_label_chu = NULL;
    s.f4_last_change_seq = 0u;
}

static void cycle_page(int delta)
{
    int p = (int)s.page + delta;
    while (p < 0) p += TELE_PAGE_COUNT;
    while (p >= TELE_PAGE_COUNT) p -= TELE_PAGE_COUNT;
    s.page = (tele_page_t)p;
    /* Reset the change-seq watch so the new page's first refresh
     * always paints (otherwise a stale match silently skips). */
    s.f3_last_change_seq = 0u;
    s.f4_last_change_seq = 0u;
    s.last_refresh_ms    = 0u;
    render();
    TRACE("tele", "page", "p=%d", (int)s.page);
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;

    switch (ev->keycode) {
        case MOKYA_KEY_LEFT:  cycle_page(-1); return;
        case MOKYA_KEY_RIGHT: cycle_page(+1); return;
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_BOOT_HOME);
            return;
        default: break;
    }

    /* F-3-only keys: list navigation + OK → C-2. */
    if (s.page != TELE_PAGE_F3) return;
    uint32_t total = phoneapi_cache_node_count();
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (total > 0u && s.f3_cursor > 0u) {
                s.f3_cursor--;
                render();
            }
            break;
        case MOKYA_KEY_DOWN:
            if (total > 0u && s.f3_cursor + 1u < total) {
                s.f3_cursor++;
                render();
            }
            break;
        case MOKYA_KEY_OK: {
            if (total == 0u) break;
            phoneapi_node_t e;
            if (phoneapi_cache_take_node_at(s.f3_cursor, &e)) {
                nodes_view_set_active_node(e.num);
                view_router_navigate(VIEW_ID_NODE_DETAIL);
            }
            break;
        }
        default: break;
    }
}

static void refresh(void)
{
    if (s.title == NULL) return;

    if (s.page == TELE_PAGE_F3) {
        /* Re-render only when the cache mutates — matches nodes_view. */
        uint32_t cur = phoneapi_cache_change_seq();
        if (cur == s.f3_last_change_seq) {
            /* Still need to update the relative-time column once a
             * second so the "Heard" column doesn't freeze. */
            uint32_t t = now_ms();
            if (t - s.last_refresh_ms < 1000u) return;
            s.last_refresh_ms = t;
        } else {
            s.f3_last_change_seq = cur;
            s.last_refresh_ms    = now_ms();
        }
        render_f3();
        return;
    }

    if (s.page == TELE_PAGE_F4) {
        /* Sample period is 30 s; gate redraws on the metrics change-seq
         * so the chart re-fill cost only pays when there's new data. */
        uint32_t cur = metrics_history_change_seq();
        if (cur == s.f4_last_change_seq) return;
        s.f4_last_change_seq = cur;
        s.last_refresh_ms    = now_ms();
        render_f4();
        return;
    }

    /* F-1 / F-2: 1 Hz local-sensor refresh. */
    uint32_t t = now_ms();
    if (t - s.last_refresh_ms < 1000u) return;
    s.last_refresh_ms = t;
    render();
}

static const view_descriptor_t TELEMETRY_DESC = {
    .id      = VIEW_ID_TELEMETRY,
    .name    = "telemetry",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "<> 切頁", "OK 詳情(F-3)", "BACK 返回" },
};

const view_descriptor_t *telemetry_view_descriptor(void)
{
    return &TELEMETRY_DESC;
}
