/* status_bar.c — see status_bar.h.
 *
 * Layout (X positions per docs/ui/10-status-bar.md §字位佈局; 8 px / cell):
 *
 *   x  0..40   time         "HH:MM"
 *   x 48..64   tx + rx      "▲ ▼"
 *   x 64..72   warn         "⚠"  (conditional)
 *   x 80..136  neighbours   "●Mesh:N"
 *   x 144..176 gps          "●GPS"
 *   x 184..208 unread       "✉N" (conditional)
 *   x 216..256 battery      "▣87%"
 *   x 288..304 mode         "Op"/"注"/"EN"/"Ab"
 *
 * The bar is parented to the screen (not the view panel) so it survives
 * view swaps. Alert overlay (single label) sits on top and hides the
 * normal cells when active.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "status_bar.h"

#include <stdio.h>
#include <string.h>

#include "ui_theme.h"

#include "FreeRTOS.h"
#include "task.h"

/* Data sources — best-effort. Only read what is genuinely available. */
#include "bq25622.h"
#include "teseo_liv3fl.h"
#include "phoneapi_cache.h"

/* ── State ───────────────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t *bar;          /* container */
    lv_obj_t *time_lbl;
    lv_obj_t *tx_lbl;
    lv_obj_t *rx_lbl;
    lv_obj_t *warn_lbl;
    lv_obj_t *mesh_lbl;
    lv_obj_t *gps_lbl;
    lv_obj_t *unread_lbl;
    lv_obj_t *batt_lbl;
    lv_obj_t *mode_lbl;
    /* Alert overlay (simple single-label MVP) */
    lv_obj_t *alert_lbl;

    uint32_t  tx_pulse_until_ms;
    uint32_t  rx_pulse_until_ms;
    uint32_t  alert_clear_at_ms;   /* 0 → sticky / not active */
    uint32_t  last_tick_ms;

    status_bar_mode_t mode;
} status_bar_t;

static status_bar_t s;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static lv_obj_t *make_label(lv_obj_t *parent, int x, int w, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, 0);
    lv_obj_set_size(l, w, STATUS_BAR_HEIGHT);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ── Init ────────────────────────────────────────────────────────────── */

void status_bar_init(lv_obj_t *screen)
{
    memset(&s, 0, sizeof(s));

    s.bar = lv_obj_create(screen);
    lv_obj_set_pos(s.bar, 0, 0);
    lv_obj_set_size(s.bar, 320, STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(s.bar, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(s.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s.bar, 0, 0);
    lv_obj_set_style_pad_all(s.bar, 0, 0);
    lv_obj_set_style_radius(s.bar, 0, 0);
    lv_obj_clear_flag(s.bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_color_t white = ui_color(UI_COLOR_TEXT_PRIMARY);
    lv_color_t dim   = ui_color(UI_COLOR_BORDER_NORMAL);

    s.time_lbl   = make_label(s.bar,   0,  40, white);
    s.tx_lbl     = make_label(s.bar,  48,   8, dim);
    s.rx_lbl     = make_label(s.bar,  56,   8, dim);
    s.warn_lbl   = make_label(s.bar,  64,   8, ui_color(UI_COLOR_WARN_YELLOW));
    s.mesh_lbl   = make_label(s.bar,  80,  56, white);
    s.gps_lbl    = make_label(s.bar, 144,  32, ui_color(UI_COLOR_TEXT_SECONDARY));
    s.unread_lbl = make_label(s.bar, 184,  24, ui_color(UI_COLOR_ACCENT_SUCCESS));
    s.batt_lbl   = make_label(s.bar, 216,  40, white);
    s.mode_lbl   = make_label(s.bar, 288,  16, white);

    /* Alert overlay sits on top, full-width, hidden by default. */
    s.alert_lbl = lv_label_create(s.bar);
    lv_obj_set_pos(s.alert_lbl, 0, 0);
    lv_obj_set_size(s.alert_lbl, 320, STATUS_BAR_HEIGHT);
    lv_obj_set_style_text_font(s.alert_lbl, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.alert_lbl, white, 0);
    lv_obj_set_style_bg_color(s.alert_lbl, ui_color(UI_COLOR_ALERT_BG_CRIT), 0);
    lv_obj_set_style_bg_opa(s.alert_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s.alert_lbl, 4, 0);
    lv_label_set_text(s.alert_lbl, "");
    lv_obj_add_flag(s.alert_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Initial paint */
    lv_label_set_text(s.tx_lbl, "^");
    lv_label_set_text(s.rx_lbl, "v");
    lv_label_set_text(s.mode_lbl, "Op");
    lv_label_set_text(s.gps_lbl, "?GPS");
    lv_label_set_text(s.mesh_lbl, "Mesh:0");
    lv_label_set_text(s.batt_lbl, "---%");
    lv_label_set_text(s.warn_lbl, "");
    lv_label_set_text(s.unread_lbl, "");
}

/* ── Per-cell refresh ────────────────────────────────────────────────── */

static void refresh_time(void)
{
    /* No RTC on Rev A — show uptime HH:MM as a sane placeholder until
     * a wall-clock source is wired (e.g. Meshtastic admin set_time).  */
    uint32_t s_total = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    uint32_t hh = (s_total / 3600) % 100;
    uint32_t mm = (s_total / 60) % 60;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             (unsigned long)hh, (unsigned long)mm);
    lv_label_set_text(s.time_lbl, buf);
}

static void refresh_tx_rx(uint32_t now)
{
    bool tx_on = now < s.tx_pulse_until_ms;
    bool rx_on = now < s.rx_pulse_until_ms;
    lv_obj_set_style_text_color(s.tx_lbl,
        tx_on ? ui_color(UI_COLOR_ACCENT_FOCUS) : ui_color(UI_COLOR_BORDER_NORMAL), 0);
    lv_obj_set_style_text_color(s.rx_lbl,
        rx_on ? ui_color(UI_COLOR_ACCENT_SUCCESS) : ui_color(UI_COLOR_BORDER_NORMAL), 0);
}

static void refresh_neighbours(void)
{
    uint32_t n = phoneapi_cache_node_count();
    char buf[12];
    if (n > 99) snprintf(buf, sizeof(buf), "Mesh:99+");
    else        snprintf(buf, sizeof(buf), "Mesh:%lu", (unsigned long)n);
    lv_label_set_text(s.mesh_lbl, buf);
    /* Colour: green if any neighbour, grey if 0. We don't yet track
     * per-neighbour last_heard age in aggregate. */
    lv_obj_set_style_text_color(s.mesh_lbl,
        n > 0 ? ui_color(UI_COLOR_ACCENT_SUCCESS)
              : ui_color(UI_COLOR_TEXT_SECONDARY), 0);
}

static void refresh_gps(void)
{
    const teseo_state_t *t = teseo_get_state();
    if (!t || !t->online) {
        lv_label_set_text(s.gps_lbl, "xGPS");
        lv_obj_set_style_text_color(s.gps_lbl, ui_color(UI_COLOR_WARN_RED), 0);
        return;
    }
    if (t->fix_valid && t->num_sats >= 4) {
        lv_label_set_text(s.gps_lbl, ".GPS");
        lv_obj_set_style_text_color(s.gps_lbl, ui_color(UI_COLOR_ACCENT_SUCCESS), 0);
    } else if (t->num_sats > 0) {
        lv_label_set_text(s.gps_lbl, "oGPS");
        lv_obj_set_style_text_color(s.gps_lbl, ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    } else {
        lv_label_set_text(s.gps_lbl, "xGPS");
        lv_obj_set_style_text_color(s.gps_lbl, ui_color(UI_COLOR_WARN_RED), 0);
    }
}

static void refresh_unread(void)
{
    uint32_t n = phoneapi_msgs_count();
    if (n == 0) {
        lv_label_set_text(s.unread_lbl, "");
        return;
    }
    char buf[8];
    if (n > 9) snprintf(buf, sizeof(buf), "M9+");
    else       snprintf(buf, sizeof(buf), "M%lu", (unsigned long)n);
    lv_label_set_text(s.unread_lbl, buf);
}

static void refresh_battery(void)
{
    const bq25622_state_t *b = bq25622_get_state();
    if (!b || !b->online) {
        lv_label_set_text(s.batt_lbl, "---%");
        lv_obj_set_style_text_color(s.batt_lbl, ui_color(UI_COLOR_TEXT_SECONDARY), 0);
        return;
    }
    /* Crude SoC from VBAT until BQ27441 / soc estimator lands. Linear
     * map 3300..4200 mV → 0..100 %; clamped. This is a placeholder so
     * the cell shows something sane on Rev A — real SoC TBD. */
    int pct = ((int)b->vbat_mv - 3300) * 100 / (4200 - 3300);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    bool charging = (b->vbus_stat != 0) && (b->ibat_ma > 30);
    char buf[12];
    snprintf(buf, sizeof(buf), "%c%d%%",
             charging ? '+' : '#',
             pct);
    lv_label_set_text(s.batt_lbl, buf);

    lv_color_t col = ui_color(UI_COLOR_TEXT_PRIMARY);
    if      (pct < 5)  col = ui_color(UI_COLOR_WARN_RED);
    else if (pct < 15) col = ui_color(UI_COLOR_WARN_RED);
    else if (pct < 30) col = ui_color(UI_COLOR_WARN_YELLOW);
    lv_obj_set_style_text_color(s.batt_lbl, col, 0);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void status_bar_tick(void)
{
    uint32_t now = now_ms();

    /* Throttle expensive refreshes to ~5 Hz */
    if (now - s.last_tick_ms < 200) {
        refresh_tx_rx(now);  /* TX/RX pulse needs to fade quickly */
        return;
    }
    s.last_tick_ms = now;

    /* Alert auto-clear */
    if (s.alert_clear_at_ms != 0 && now >= s.alert_clear_at_ms) {
        status_bar_clear_alert();
    }

    refresh_time();
    refresh_tx_rx(now);
    refresh_neighbours();
    refresh_gps();
    refresh_unread();
    refresh_battery();
}

void status_bar_pulse_tx(void)
{
    s.tx_pulse_until_ms = now_ms() + 250;
}

void status_bar_pulse_rx(void)
{
    s.rx_pulse_until_ms = now_ms() + 250;
}

void status_bar_set_mode(status_bar_mode_t mode)
{
    if (s.mode_lbl == NULL) return;
    s.mode = mode;
    const char *txt = "Op";
    lv_color_t col = ui_color(UI_COLOR_TEXT_PRIMARY);
    switch (mode) {
        case STATUS_BAR_MODE_OP: txt = "Op"; col = ui_color(UI_COLOR_TEXT_PRIMARY);   break;
        case STATUS_BAR_MODE_ZH: txt = "ZH"; col = ui_color(UI_COLOR_ACCENT_FOCUS);   break;
        case STATUS_BAR_MODE_EN: txt = "EN"; col = ui_color(UI_COLOR_ACCENT_FOCUS);   break;
        case STATUS_BAR_MODE_AB: txt = "Ab"; col = ui_color(UI_COLOR_ACCENT_FOCUS);   break;
    }
    lv_label_set_text(s.mode_lbl, txt);
    lv_obj_set_style_text_color(s.mode_lbl, col, 0);
}

void status_bar_show_alert(uint8_t level, const char *text, uint32_t duration_ms)
{
    if (s.alert_lbl == NULL || text == NULL) return;
    lv_color_t bg = ui_color(UI_COLOR_ALERT_BG_CRIT);
    if      (level == 0) bg = ui_color(UI_COLOR_BORDER_NORMAL);
    else if (level == 1) bg = ui_color(UI_COLOR_ALERT_BG_WARN);
    else                 bg = ui_color(UI_COLOR_ALERT_BG_CRIT);
    lv_obj_set_style_bg_color(s.alert_lbl, bg, 0);
    lv_label_set_text(s.alert_lbl, text);
    lv_obj_clear_flag(s.alert_lbl, LV_OBJ_FLAG_HIDDEN);
    s.alert_clear_at_ms = duration_ms ? (now_ms() + duration_ms) : 0;
}

void status_bar_clear_alert(void)
{
    if (s.alert_lbl == NULL) return;
    lv_obj_add_flag(s.alert_lbl, LV_OBJ_FLAG_HIDDEN);
    s.alert_clear_at_ms = 0;
}
