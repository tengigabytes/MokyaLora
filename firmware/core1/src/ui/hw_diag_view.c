/* hw_diag_view.c — see hw_diag_view.h.
 *
 * Phase 2 skeleton: 8 placeholder pages, LEFT/RIGHT cycles. Per-page
 * widgets are built lazily by page_enter() and torn down by
 * page_leave() to keep the LVGL pool small (one page's widgets at a
 * time). The view-level create() builds only the title bar.
 *
 * Pages 3.1-4.3 plug their handlers into the s_pages[] table as they
 * land — until then, each page just renders its name + "TBD" so the
 * navigation works end-to-end from launcher → pages → BACK.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hw_diag_view.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "bq25622.h"
#include "lsm6dsv16x.h"
#include "lis2mdl.h"
#include "lps22hh.h"
#include "keypad_scan.h"
#include "lm27965.h"
#include "teseo_liv3fl.h"

/* ── Page registry ────────────────────────────────────────────────── */

typedef enum {
    DIAG_PAGE_GNSS_NMEA = 0,
    DIAG_PAGE_GNSS_DIAG,
    DIAG_PAGE_GNSS_CFG,        /* Phase 6 — Teseo runtime control */
    DIAG_PAGE_GNSS_CONST,      /* Phase 7.A — constellation 9-preset menu */
    DIAG_PAGE_GNSS_TRACK,      /* Phase 7.B — tracking thresholds + notch filter */
    DIAG_PAGE_GNSS_NMEA_CFG,   /* Phase 7.C — NMEA talker ID + output preset */
    DIAG_PAGE_GNSS_ADV,        /* Phase 7.D — Odometer / Data logger / Geofence read-only */
    DIAG_PAGE_LED,
    DIAG_PAGE_TFT_BL,
    DIAG_PAGE_BUTTONS,
    DIAG_PAGE_SENSORS,
    DIAG_PAGE_CHARGER,
    DIAG_PAGE_CHARGE_CTRL,
    DIAG_PAGE_NOTIF,           /* Phase 8 — notification settings */
    DIAG_PAGE_COUNT
} diag_page_t;

typedef struct {
    const char *name;
    void (*enter)(lv_obj_t *content_root);   /* build widgets, may be NULL */
    void (*leave)(void);                     /* clear widget pointers, may be NULL */
    void (*apply)(const key_event_t *ev);    /* UP/DOWN/OK; may be NULL */
    void (*refresh)(void);                   /* tick refresh; may be NULL */
} diag_page_def_t;

/* Forward declarations of per-page hooks (lower in file). */
static void charger_enter(lv_obj_t *root);
static void charger_leave(void);
static void charger_refresh(void);
static void sensors_enter(lv_obj_t *root);
static void sensors_leave(void);
static void sensors_refresh(void);
static void buttons_enter(lv_obj_t *root);
static void buttons_leave(void);
static void buttons_refresh(void);
static void tft_bl_enter(lv_obj_t *root);
static void tft_bl_leave(void);
static void tft_bl_apply(const key_event_t *ev);
static void tft_bl_refresh(void);
static void led_enter(lv_obj_t *root);
static void led_leave(void);
static void led_apply(const key_event_t *ev);
static void led_refresh(void);
static void gnss_diag_enter(lv_obj_t *root);
static void gnss_diag_leave(void);
static void gnss_diag_refresh(void);
static void gnss_nmea_enter(lv_obj_t *root);
static void gnss_nmea_leave(void);
static void gnss_nmea_apply(const key_event_t *ev);
static void gnss_nmea_refresh(void);
static void gnss_cfg_enter(lv_obj_t *root);
static void gnss_cfg_leave(void);
static void gnss_cfg_apply(const key_event_t *ev);
static void gnss_cfg_refresh(void);
static void gnss_const_enter(lv_obj_t *root);
static void gnss_const_leave(void);
static void gnss_const_apply(const key_event_t *ev);
static void gnss_const_refresh(void);
static void gnss_track_enter(lv_obj_t *root);
static void gnss_track_leave(void);
static void gnss_track_apply(const key_event_t *ev);
static void gnss_track_refresh(void);
static void gnss_nmea_cfg_enter(lv_obj_t *root);
static void gnss_nmea_cfg_leave(void);
static void gnss_nmea_cfg_apply(const key_event_t *ev);
static void gnss_nmea_cfg_refresh(void);
static void gnss_adv_enter(lv_obj_t *root);
static void gnss_adv_leave(void);
static void gnss_adv_apply(const key_event_t *ev);
static void gnss_adv_refresh(void);
static void notif_enter(lv_obj_t *root);
static void notif_leave(void);
static void notif_apply(const key_event_t *ev);
static void notif_refresh(void);

/* Each page's enter() is responsible for clearing/recreating widgets
 * under content_root. The view destroys content_root on view-destroy.
 * Forward declarations — handlers added in Phase 3 / Phase 4. */
static diag_page_def_t s_pages[DIAG_PAGE_COUNT] = {
    [DIAG_PAGE_GNSS_NMEA]   = { "GNSS NMEA",
                                gnss_nmea_enter, gnss_nmea_leave,
                                gnss_nmea_apply, gnss_nmea_refresh },
    [DIAG_PAGE_GNSS_DIAG]   = { "GNSS Diag",
                                gnss_diag_enter, gnss_diag_leave, NULL, gnss_diag_refresh },
    [DIAG_PAGE_GNSS_CFG]    = { "GNSS Cfg",
                                gnss_cfg_enter, gnss_cfg_leave, gnss_cfg_apply, gnss_cfg_refresh },
    [DIAG_PAGE_GNSS_CONST]  = { "GNSS Const",
                                gnss_const_enter, gnss_const_leave,
                                gnss_const_apply, gnss_const_refresh },
    [DIAG_PAGE_GNSS_TRACK]  = { "GNSS Track",
                                gnss_track_enter, gnss_track_leave,
                                gnss_track_apply, gnss_track_refresh },
    [DIAG_PAGE_GNSS_NMEA_CFG] = { "GNSS NMEA Cfg",
                                gnss_nmea_cfg_enter, gnss_nmea_cfg_leave,
                                gnss_nmea_cfg_apply, gnss_nmea_cfg_refresh },
    [DIAG_PAGE_GNSS_ADV]     = { "GNSS Adv",
                                gnss_adv_enter, gnss_adv_leave,
                                gnss_adv_apply, gnss_adv_refresh },
    [DIAG_PAGE_LED]         = { "LED 亮度",
                                led_enter, led_leave, led_apply, led_refresh },
    [DIAG_PAGE_TFT_BL]      = { "TFT 背光",
                                tft_bl_enter, tft_bl_leave, tft_bl_apply, tft_bl_refresh },
    [DIAG_PAGE_BUTTONS]     = { "按鍵診斷",
                                buttons_enter, buttons_leave, NULL, buttons_refresh },
    [DIAG_PAGE_SENSORS]     = { "感測器",
                                sensors_enter, sensors_leave, NULL, sensors_refresh },
    [DIAG_PAGE_CHARGER]     = { "充電器讀值",
                                charger_enter, charger_leave, NULL, charger_refresh },
    [DIAG_PAGE_CHARGE_CTRL] = { "充電控制",     NULL, NULL, NULL, NULL },
    [DIAG_PAGE_NOTIF]       = { "通知設定",
                                notif_enter, notif_leave,
                                notif_apply, notif_refresh },
};

/* ── View state ───────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t   *bg;
    lv_obj_t   *title_lbl;
    lv_obj_t   *content_root;
    lv_obj_t   *placeholder_lbl;   /* shown when active page has no enter() */
    diag_page_t cur_page;
} hw_diag_state_t;

/* PSRAM — same rationale as launcher_view (no SWD inspection, no
 * early-boot touch). Save ~24 B of SRAM .bss. */
static hw_diag_state_t s __attribute__((section(".psram_bss")));

/* SWD-readable mirror of cur_page. PSRAM s.cur_page isn't directly
 * coherent. Updated by switch_page() / create(). 1 byte. */
volatile uint8_t g_hw_diag_cur_page __attribute__((used)) = 0;

/* ── Helpers ──────────────────────────────────────────────────────── */

static void update_title(void)
{
    if (s.title_lbl == NULL) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "[硬體診斷] ◀ %u/%u %s ▶",
             (unsigned)(s.cur_page + 1u),
             (unsigned)DIAG_PAGE_COUNT,
             s_pages[s.cur_page].name);
    lv_label_set_text(s.title_lbl, buf);
}

static void clear_content(void)
{
    if (s.content_root == NULL) return;
    /* Destroy all widget children of content_root. View-owned widget
     * pointers (placeholder_lbl etc.) are nulled by the leave handlers
     * before this runs. */
    lv_obj_clean(s.content_root);
    s.placeholder_lbl = NULL;
}

static void show_placeholder(void)
{
    if (s.content_root == NULL) return;
    s.placeholder_lbl = lv_label_create(s.content_root);
    lv_obj_set_style_text_font(s.placeholder_lbl, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.placeholder_lbl,
                                ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    lv_label_set_text(s.placeholder_lbl, "尚未實作（Phase 3/4 待補）");
    lv_obj_align(s.placeholder_lbl, LV_ALIGN_CENTER, 0, 0);
}

static void switch_page(diag_page_t to)
{
    if (to >= DIAG_PAGE_COUNT) return;
    if (s_pages[s.cur_page].leave) s_pages[s.cur_page].leave();
    clear_content();
    s.cur_page = to;
    g_hw_diag_cur_page = (uint8_t)to;
    update_title();
    if (s_pages[s.cur_page].enter) {
        s_pages[s.cur_page].enter(s.content_root);
    } else {
        show_placeholder();
    }
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    /* Title bar at top — 16 px tall. */
    s.title_lbl = lv_label_create(panel);
    lv_obj_set_pos(s.title_lbl, 4, 2);
    lv_obj_set_size(s.title_lbl, 312, 16);
    lv_obj_set_style_text_font(s.title_lbl, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.title_lbl,
                                ui_color(UI_COLOR_ACCENT_FOCUS), 0);
    lv_obj_set_style_pad_all(s.title_lbl, 0, 0);
    lv_label_set_long_mode(s.title_lbl, LV_LABEL_LONG_CLIP);

    /* Content root: from y=20 to y=224, width 320. Pages render here. */
    s.content_root = lv_obj_create(panel);
    lv_obj_set_pos(s.content_root, 0, 20);
    lv_obj_set_size(s.content_root, 320, 204);
    lv_obj_set_style_bg_opa(s.content_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.content_root, 0, 0);
    lv_obj_set_style_radius(s.content_root, 0, 0);
    lv_obj_set_style_pad_all(s.content_root, 0, 0);
    lv_obj_clear_flag(s.content_root, LV_OBJ_FLAG_SCROLLABLE);

    s.cur_page = DIAG_PAGE_GNSS_NMEA;
    g_hw_diag_cur_page = (uint8_t)s.cur_page;
    update_title();
    if (s_pages[s.cur_page].enter) {
        s_pages[s.cur_page].enter(s.content_root);
    } else {
        show_placeholder();
    }
}

static void destroy(void)
{
    if (s_pages[s.cur_page].leave) s_pages[s.cur_page].leave();
    /* content_root + title_lbl are deleted by router via lv_obj_del(panel). */
    memset(&s, 0, sizeof(s));
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    switch (ev->keycode) {
        case MOKYA_KEY_LEFT:
            switch_page((diag_page_t)((s.cur_page + DIAG_PAGE_COUNT - 1u) % DIAG_PAGE_COUNT));
            break;
        case MOKYA_KEY_RIGHT:
            switch_page((diag_page_t)((s.cur_page + 1u) % DIAG_PAGE_COUNT));
            break;
        default:
            if (s_pages[s.cur_page].apply) {
                s_pages[s.cur_page].apply(ev);
            }
            break;
    }
}

static void refresh(void)
{
    if (s_pages[s.cur_page].refresh) s_pages[s.cur_page].refresh();
}

static const view_descriptor_t HW_DIAG_DESC = {
    .id      = VIEW_ID_HW_DIAG,
    .name    = "hw_diag",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "◀▶ 切換頁", "OK 互動", "BACK 返回" },
};

/* ── Page: charger readout (DIAG_PAGE_CHARGER) ────────────────────── *
 *
 * Reads bq25622_get_state() snapshot once per refresh tick and dumps
 * every measurable field. Display only — no controls; charger control
 * lives on its own page (DIAG_PAGE_CHARGE_CTRL). */

static struct {
    lv_obj_t *vbus_lbl;
    lv_obj_t *vbat_lbl;
    lv_obj_t *vsys_lbl;
    lv_obj_t *ibus_lbl;
    lv_obj_t *ibat_lbl;
    lv_obj_t *ts_tdie_lbl;
    lv_obj_t *stat_lbl;
    lv_obj_t *fault_lbl;
    lv_obj_t *wd_lbl;
    uint32_t  last_refresh_tick;
} s_charger __attribute__((section(".psram_bss")));

static lv_obj_t *mk_diag_label(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_style_text_font(lbl, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(lbl, ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_pad_all(lbl, 0, 0);
    return lbl;
}

static void charger_enter(lv_obj_t *root)
{
    /* Two-column layout, 16 px line height, 9 lines. Content area is
     * 320 × 204; left col x=4, right col x=164. */
    s_charger.vbus_lbl     = mk_diag_label(root,   4,   0);
    s_charger.vbat_lbl     = mk_diag_label(root,   4,  16);
    s_charger.vsys_lbl     = mk_diag_label(root,   4,  32);
    s_charger.ibus_lbl     = mk_diag_label(root,   4,  48);
    s_charger.ibat_lbl     = mk_diag_label(root,   4,  64);
    s_charger.ts_tdie_lbl  = mk_diag_label(root,   4,  80);
    s_charger.stat_lbl     = mk_diag_label(root,   4,  96);
    s_charger.fault_lbl    = mk_diag_label(root,   4, 112);
    s_charger.wd_lbl       = mk_diag_label(root,   4, 128);
    s_charger.last_refresh_tick = 0;
    charger_refresh();
}

static void charger_leave(void)
{
    /* Widgets are children of content_root which clear_content() will
     * lv_obj_clean(). Just null our pointers. */
    memset(&s_charger, 0, sizeof(s_charger));
}

static const char *chg_stat_name(uint8_t s)
{
    switch (s) {
        case 0: return "NoCHG";
        case 1: return "CC";
        case 2: return "Taper";
        case 3: return "TopOff";
        default: return "?";
    }
}

static const char *vbus_stat_name(uint8_t s)
{
    switch (s) {
        case 0: return "None";
        case 4: return "USB-Adap";
        case 7: return "OTG";
        default: return "?";
    }
}

static void charger_refresh(void)
{
    if (s_charger.vbus_lbl == NULL) return;
    /* Throttle to ~2 Hz; bq25622 ADC refresh is 1 Hz so faster is wasted. */
    uint32_t tick = lv_tick_get();
    if ((tick - s_charger.last_refresh_tick) < 500u) return;
    s_charger.last_refresh_tick = tick;

    const bq25622_state_t *b = bq25622_get_state();
    char buf[80];

    if (b == NULL || !b->online) {
        lv_label_set_text(s_charger.vbus_lbl, "充電器: offline");
        lv_label_set_text(s_charger.vbat_lbl, "");
        lv_label_set_text(s_charger.vsys_lbl, "");
        lv_label_set_text(s_charger.ibus_lbl, "");
        lv_label_set_text(s_charger.ibat_lbl, "");
        lv_label_set_text(s_charger.ts_tdie_lbl, "");
        lv_label_set_text(s_charger.stat_lbl, "");
        lv_label_set_text(s_charger.fault_lbl, "");
        lv_label_set_text(s_charger.wd_lbl, "");
        return;
    }

    snprintf(buf, sizeof(buf), "VBUS  %u mV", (unsigned)b->vbus_mv);
    lv_label_set_text(s_charger.vbus_lbl, buf);
    snprintf(buf, sizeof(buf), "VBAT  %u mV", (unsigned)b->vbat_mv);
    lv_label_set_text(s_charger.vbat_lbl, buf);
    snprintf(buf, sizeof(buf), "VSYS  %u mV   PMID %u mV",
             (unsigned)b->vsys_mv, (unsigned)b->vpmid_mv);
    lv_label_set_text(s_charger.vsys_lbl, buf);
    snprintf(buf, sizeof(buf), "IBUS  %d mA", (int)b->ibus_ma);
    lv_label_set_text(s_charger.ibus_lbl, buf);
    snprintf(buf, sizeof(buf), "IBAT  %d mA", (int)b->ibat_ma);
    lv_label_set_text(s_charger.ibat_lbl, buf);
    snprintf(buf, sizeof(buf), "TS %u.%u %%   TDIE %d.%d C",
             (unsigned)(b->ts_pct_x10 / 10u), (unsigned)(b->ts_pct_x10 % 10u),
             (int)(b->tdie_cx10 / 10), (int)((b->tdie_cx10 < 0 ? -b->tdie_cx10 : b->tdie_cx10) % 10));
    lv_label_set_text(s_charger.ts_tdie_lbl, buf);
    snprintf(buf, sizeof(buf), "CHG=%s VBUS=%s TS=%u",
             chg_stat_name(b->chg_stat),
             vbus_stat_name(b->vbus_stat),
             (unsigned)b->ts_stat);
    lv_label_set_text(s_charger.stat_lbl, buf);
    snprintf(buf, sizeof(buf), "Faults  bat=%u sys=%u tshut=%u",
             (unsigned)b->bat_fault,
             (unsigned)b->sys_fault,
             (unsigned)b->tshut);
    lv_label_set_text(s_charger.fault_lbl, buf);
    snprintf(buf, sizeof(buf), "WD %u s   exp=%lu   i2c_fail=%lu",
             (unsigned)(b->wd_window == BQ25622_WD_OFF ? 0 :
                        (b->wd_window == BQ25622_WD_50S ? 50 :
                         b->wd_window == BQ25622_WD_100S ? 100 : 200)),
             (unsigned long)b->wd_expired_count,
             (unsigned long)b->i2c_fail_count);
    lv_label_set_text(s_charger.wd_lbl, buf);
}

/* ── Page: sensors (DIAG_PAGE_SENSORS) ────────────────────────────── *
 *
 * IMU (LSM6DSV16X): 3-axis accel, 3-axis gyro, internal temp.
 * Magnetometer (LIS2MDL): 3-axis mag, internal temp.
 * Barometer (LPS22HH): pressure, temp.
 *
 * Each driver has its own state mirror updated by sensor_task at
 * 10 Hz; we just snapshot and render. */

static struct {
    lv_obj_t *imu_a_lbl;
    lv_obj_t *imu_g_lbl;
    lv_obj_t *imu_t_lbl;
    lv_obj_t *mag_xyz_lbl;
    lv_obj_t *mag_t_lbl;
    lv_obj_t *baro_lbl;
    uint32_t  last_refresh_tick;
} s_sensors __attribute__((section(".psram_bss")));

static void sensors_enter(lv_obj_t *root)
{
    s_sensors.imu_a_lbl   = mk_diag_label(root, 4,   0);
    s_sensors.imu_g_lbl   = mk_diag_label(root, 4,  16);
    s_sensors.imu_t_lbl   = mk_diag_label(root, 4,  32);
    s_sensors.mag_xyz_lbl = mk_diag_label(root, 4,  56);
    s_sensors.mag_t_lbl   = mk_diag_label(root, 4,  72);
    s_sensors.baro_lbl    = mk_diag_label(root, 4,  96);
    s_sensors.last_refresh_tick = 0;
    sensors_refresh();
}

static void sensors_leave(void)
{
    memset(&s_sensors, 0, sizeof(s_sensors));
}

static void sensors_refresh(void)
{
    if (s_sensors.imu_a_lbl == NULL) return;
    /* Sensors run at 10 Hz; refresh UI every 100 ms (one tick per sample). */
    uint32_t tick = lv_tick_get();
    if ((tick - s_sensors.last_refresh_tick) < 100u) return;
    s_sensors.last_refresh_tick = tick;

    char buf[80];

    const lsm6dsv16x_state_t *imu = lsm6dsv16x_get_state();
    if (imu != NULL && imu->online) {
        snprintf(buf, sizeof(buf), "Acc  %+5d %+5d %+5d mg",
                 (int)imu->accel_mg[0], (int)imu->accel_mg[1], (int)imu->accel_mg[2]);
        lv_label_set_text(s_sensors.imu_a_lbl, buf);
        snprintf(buf, sizeof(buf), "Gyr  %+5d %+5d %+5d dps/10",
                 (int)imu->gyro_dps_x10[0], (int)imu->gyro_dps_x10[1], (int)imu->gyro_dps_x10[2]);
        lv_label_set_text(s_sensors.imu_g_lbl, buf);
        snprintf(buf, sizeof(buf), "IMU temp %d.%d C   i2c_fail=%lu",
                 (int)(imu->temperature_cx10 / 10),
                 (int)((imu->temperature_cx10 < 0 ? -imu->temperature_cx10 : imu->temperature_cx10) % 10),
                 (unsigned long)imu->i2c_fail_count);
        lv_label_set_text(s_sensors.imu_t_lbl, buf);
    } else {
        lv_label_set_text(s_sensors.imu_a_lbl, "IMU: offline");
        lv_label_set_text(s_sensors.imu_g_lbl, "");
        lv_label_set_text(s_sensors.imu_t_lbl, "");
    }

    const lis2mdl_state_t *mag = lis2mdl_get_state();
    if (mag != NULL && mag->online) {
        snprintf(buf, sizeof(buf), "Mag  %+5d %+5d %+5d uT/10",
                 (int)mag->mag_ut_x10[0], (int)mag->mag_ut_x10[1], (int)mag->mag_ut_x10[2]);
        lv_label_set_text(s_sensors.mag_xyz_lbl, buf);
        snprintf(buf, sizeof(buf), "Mag temp %d.%d C   i2c_fail=%lu",
                 (int)(mag->temperature_cx10 / 10),
                 (int)((mag->temperature_cx10 < 0 ? -mag->temperature_cx10 : mag->temperature_cx10) % 10),
                 (unsigned long)mag->i2c_fail_count);
        lv_label_set_text(s_sensors.mag_t_lbl, buf);
    } else {
        lv_label_set_text(s_sensors.mag_xyz_lbl, "Mag: offline");
        lv_label_set_text(s_sensors.mag_t_lbl, "");
    }

    const lps22hh_state_t *baro = lps22hh_get_state();
    if (baro != NULL && baro->online) {
        snprintf(buf, sizeof(buf), "Baro  %lu.%02lu hPa   %d.%d C   fail=%lu",
                 (unsigned long)(baro->pressure_hpa_x100 / 100u),
                 (unsigned long)(baro->pressure_hpa_x100 % 100u),
                 (int)(baro->temperature_cx10 / 10),
                 (int)((baro->temperature_cx10 < 0 ? -baro->temperature_cx10 : baro->temperature_cx10) % 10),
                 (unsigned long)baro->i2c_fail_count);
        lv_label_set_text(s_sensors.baro_lbl, buf);
    } else {
        lv_label_set_text(s_sensors.baro_lbl, "Baro: offline");
    }
}

/* ── Page: button matrix (DIAG_PAGE_BUTTONS) ─────────────────────── *
 *
 * Real-time visualisation of the 6×6 keypad scan state. Each cell shows
 * the row/col index and lights up when the corresponding key is pressed.
 * Reads g_kp_stable (post-debounce). Below the matrix: scan tick counter
 * (proves the scan task is alive) and recent keycode log from the
 * key_event ring. */

#define BTN_GRID_SIZE   24      /* per-cell pixel width/height */
#define BTN_GRID_GAP     2
#define BTN_GRID_X0     16
#define BTN_GRID_Y0      0

static struct {
    lv_obj_t *cells[KEY_ROWS][KEY_COLS];
    lv_obj_t *tick_lbl;
    lv_obj_t *log_lbl;
    uint32_t  last_refresh_tick;
    uint8_t   last_state[KEY_ROWS];
    uint32_t  last_log_idx;
} s_buttons __attribute__((section(".psram_bss")));

static void buttons_enter(lv_obj_t *root)
{
    /* 6×6 grid of small filled rectangles. Total grid: 6*24 + 5*2 = 154 px. */
    for (uint8_t r = 0; r < KEY_ROWS; r++) {
        for (uint8_t c = 0; c < KEY_COLS; c++) {
            lv_obj_t *cell = lv_obj_create(root);
            lv_obj_set_pos(cell,
                           BTN_GRID_X0 + c * (BTN_GRID_SIZE + BTN_GRID_GAP),
                           BTN_GRID_Y0 + r * (BTN_GRID_SIZE + BTN_GRID_GAP));
            lv_obj_set_size(cell, BTN_GRID_SIZE, BTN_GRID_SIZE);
            lv_obj_set_style_bg_color(cell, ui_color(UI_COLOR_BG_SECONDARY), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(cell, ui_color(UI_COLOR_BORDER_NORMAL), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_set_style_radius(cell, 2, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            s_buttons.cells[r][c] = cell;
        }
    }
    s_buttons.tick_lbl = mk_diag_label(root, 4, 160);
    s_buttons.log_lbl  = mk_diag_label(root, 4, 180);
    memset(s_buttons.last_state, 0xFF, sizeof(s_buttons.last_state));   /* force first paint */
    s_buttons.last_refresh_tick = 0;
    s_buttons.last_log_idx = 0;
    buttons_refresh();
}

static void buttons_leave(void)
{
    memset(&s_buttons, 0, sizeof(s_buttons));
}

static void buttons_refresh(void)
{
    if (s_buttons.cells[0][0] == NULL) return;
    /* Refresh fast — keys feel sluggish above ~30 ms latency. 50 ms is
     * a good balance for visual smoothness without burning cycles. */
    uint32_t tick = lv_tick_get();
    if ((tick - s_buttons.last_refresh_tick) < 50u) return;
    s_buttons.last_refresh_tick = tick;

    /* Repaint cells whose row mask changed. */
    for (uint8_t r = 0; r < KEY_ROWS; r++) {
        uint8_t cur = g_kp_stable[r];
        uint8_t old = s_buttons.last_state[r];
        if (cur == old) continue;
        s_buttons.last_state[r] = cur;
        for (uint8_t c = 0; c < KEY_COLS; c++) {
            bool pressed = (cur >> c) & 1u;
            lv_obj_set_style_bg_color(s_buttons.cells[r][c],
                pressed ? ui_color(UI_COLOR_ACCENT_FOCUS)
                        : ui_color(UI_COLOR_BG_SECONDARY), 0);
        }
    }

    /* Scan tick counter — proves keypad_scan_task is alive. */
    char buf[80];
    snprintf(buf, sizeof(buf), "scan tick: %lu",
             (unsigned long)g_kp_scan_tick);
    lv_label_set_text(s_buttons.tick_lbl, buf);

    /* Recent keycode log. g_key_event_log[] is a 16-deep ring written
     * by key_event_push_*. Show up to 8 newest entries from current idx. */
    extern volatile uint8_t  g_key_event_log[];
    extern volatile uint32_t g_key_event_log_idx;
    extern volatile uint32_t g_key_event_pushed;
    uint32_t idx = g_key_event_log_idx;
    if (idx != s_buttons.last_log_idx) {
        s_buttons.last_log_idx = idx;
        char *p = buf;
        char *end = buf + sizeof(buf);
        p += snprintf(p, (size_t)(end - p), "log[%lu]:",
                      (unsigned long)g_key_event_pushed);
        const int LOG_DEPTH = 16;
        for (int i = 0; i < 8 && p < end - 6; i++) {
            int slot = ((int)idx - 1 - i) & (LOG_DEPTH - 1);
            p += snprintf(p, (size_t)(end - p), " %02X",
                          (unsigned)g_key_event_log[slot]);
        }
        lv_label_set_text(s_buttons.log_lbl, buf);
    }
}

/* ── Page: TFT backlight (DIAG_PAGE_TFT_BL) ──────────────────────── *
 *
 * Single slider — Bank A duty 0..31. ↑ raises by 1, ↓ lowers by 1, OK
 * wraps to 0 (off / on toggle). Each change calls
 * lm27965_set_tft_backlight() immediately. Renders the bar so the
 * effect is visible even before the panel re-illuminates. */

static struct {
    lv_obj_t *title_lbl;
    lv_obj_t *bar;
    lv_obj_t *value_lbl;
    lv_obj_t *hint_lbl;
    uint8_t   duty;          /* current shadow */
} s_tft_bl __attribute__((section(".psram_bss")));

static void tft_bl_render(void)
{
    if (s_tft_bl.value_lbl == NULL) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "Duty %u / %u",
             (unsigned)s_tft_bl.duty, (unsigned)LM27965_DUTY_AB_MAX);
    lv_label_set_text(s_tft_bl.value_lbl, buf);
    lv_bar_set_value(s_tft_bl.bar, (int32_t)s_tft_bl.duty, LV_ANIM_OFF);
}

static void tft_bl_enter(lv_obj_t *root)
{
    s_tft_bl.title_lbl = mk_diag_label(root, 4, 0);
    lv_label_set_text(s_tft_bl.title_lbl, "TFT 背光 (Bank A)");

    s_tft_bl.bar = lv_bar_create(root);
    lv_obj_set_pos(s_tft_bl.bar, 4, 30);
    lv_obj_set_size(s_tft_bl.bar, 312, 20);
    lv_bar_set_range(s_tft_bl.bar, 0, LM27965_DUTY_AB_MAX);

    s_tft_bl.value_lbl = mk_diag_label(root, 4, 60);
    s_tft_bl.hint_lbl  = mk_diag_label(root, 4, 90);
    lv_label_set_text(s_tft_bl.hint_lbl, "↑/↓ ±1   OK 切換 0/16");

    /* Pull current state from driver cache. */
    const lm27965_state_t *st = lm27965_get_state();
    s_tft_bl.duty = (st != NULL) ? st->bank_a_duty : 0;
    tft_bl_render();
}

static void tft_bl_leave(void)
{
    memset(&s_tft_bl, 0, sizeof(s_tft_bl));
}

static void tft_bl_apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    bool changed = false;
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s_tft_bl.duty < LM27965_DUTY_AB_MAX) { s_tft_bl.duty++; changed = true; }
            break;
        case MOKYA_KEY_DOWN:
            if (s_tft_bl.duty > 0) { s_tft_bl.duty--; changed = true; }
            break;
        case MOKYA_KEY_OK:
            /* Fast toggle off / mid-bright. */
            s_tft_bl.duty = (s_tft_bl.duty == 0) ? 16 : 0;
            changed = true;
            break;
        default: break;
    }
    if (changed) {
        lm27965_set_tft_backlight(s_tft_bl.duty);
        tft_bl_render();
    }
}

static void tft_bl_refresh(void) { /* nothing async */ }

/* ── Page: LED brightness (DIAG_PAGE_LED) ────────────────────────── *
 *
 * 3 widgets: red LED on/duty, green LED on/off (gated on Bank B duty),
 * keypad backlight on/duty. Bank B duty is shared by green + keypad on
 * Rev A — the field shows that explicitly. ↑/↓ moves widget focus,
 * ←/→ adjusts value, OK toggles the on/off gate. */

typedef enum {
    LED_W_RED_ON = 0,
    LED_W_RED_DUTY,
    LED_W_GREEN_ON,
    LED_W_KBD_ON,
    LED_W_BANKB_DUTY,
    LED_W_COUNT
} led_widget_t;

static struct {
    lv_obj_t *lbls[LED_W_COUNT];
    lv_obj_t *hint_lbl;
    led_widget_t cur;
    /* Shadow values (initialised from driver cache on enter). */
    bool     red_on;
    uint8_t  red_duty;        /* 0..3 */
    bool     green_on;
    bool     kbd_on;
    uint8_t  bankb_duty;      /* 0..31 — shared by green + keypad */
} s_led __attribute__((section(".psram_bss")));

static const char *led_widget_label(led_widget_t w)
{
    switch (w) {
        case LED_W_RED_ON:     return "紅燈 開關";
        case LED_W_RED_DUTY:   return "紅燈 亮度 (Bank C, 0..3)";
        case LED_W_GREEN_ON:   return "綠燈 開關 (gated by Bank B)";
        case LED_W_KBD_ON:     return "鍵盤背光 開關";
        case LED_W_BANKB_DUTY: return "Bank B 亮度 (0..31, 綠+鍵盤共用)";
        default:               return "?";
    }
}

static void led_render(void)
{
    char buf[80];
    for (int w = 0; w < LED_W_COUNT; w++) {
        if (s_led.lbls[w] == NULL) continue;
        const char *focus = (w == (int)s_led.cur) ? "▶ " : "  ";
        switch (w) {
            case LED_W_RED_ON:
                snprintf(buf, sizeof(buf), "%s%s : %s",
                         focus, led_widget_label(w), s_led.red_on ? "ON" : "OFF");
                break;
            case LED_W_RED_DUTY:
                snprintf(buf, sizeof(buf), "%s%s : %u",
                         focus, led_widget_label(w), (unsigned)s_led.red_duty);
                break;
            case LED_W_GREEN_ON:
                snprintf(buf, sizeof(buf), "%s%s : %s",
                         focus, led_widget_label(w), s_led.green_on ? "ON" : "OFF");
                break;
            case LED_W_KBD_ON:
                snprintf(buf, sizeof(buf), "%s%s : %s",
                         focus, led_widget_label(w), s_led.kbd_on ? "ON" : "OFF");
                break;
            case LED_W_BANKB_DUTY:
                snprintf(buf, sizeof(buf), "%s%s : %u",
                         focus, led_widget_label(w), (unsigned)s_led.bankb_duty);
                break;
            default: buf[0] = '\0'; break;
        }
        lv_label_set_text(s_led.lbls[w], buf);
    }
}

static void led_apply_to_hw(void)
{
    lm27965_set_led_red(s_led.red_on, s_led.red_duty);
    lm27965_set_keypad_backlight(s_led.kbd_on, s_led.green_on, s_led.bankb_duty);
}

static void led_enter(lv_obj_t *root)
{
    for (int w = 0; w < LED_W_COUNT; w++) {
        s_led.lbls[w] = mk_diag_label(root, 4, w * 18);
    }
    s_led.hint_lbl = mk_diag_label(root, 4, LED_W_COUNT * 18 + 8);
    lv_label_set_text(s_led.hint_lbl, "↑/↓ 移動  OK 切換/+亮度");

    const lm27965_state_t *st = lm27965_get_state();
    if (st != NULL) {
        s_led.red_duty   = st->bank_c_duty;
        s_led.bankb_duty = st->bank_b_duty;
        /* On/off bits live in GP register (kept in cache). Approximate from
         * the cache: any nonzero duty path was on at last write, but the
         * authoritative bits are the ENA/ENB/ENC bits. Use bit positions
         * documented in lm27965.c — simpler to start with the assumption
         * that boot turned on TFT only, so red/green/keypad all start off. */
        s_led.red_on   = (st->gp & (1u << 0)) != 0;   /* ENC */
        s_led.kbd_on   = (st->gp & (1u << 1)) != 0;   /* ENB */
        s_led.green_on = (st->gp & (1u << 3)) != 0;   /* EN3B */
    } else {
        s_led.red_duty = 0;
        s_led.bankb_duty = 0;
        s_led.red_on = s_led.green_on = s_led.kbd_on = false;
    }
    s_led.cur = LED_W_RED_ON;
    led_render();
}

static void led_leave(void)
{
    memset(&s_led, 0, sizeof(s_led));
}

/* hw_diag's parent dispatcher consumes ←/→ for page switching, so the
 * LED page only has ↑/↓/OK available. Design:
 *   ↑/↓  → move widget focus
 *   OK   → bool widgets toggle on/off; duty widgets +1 with wrap to 0
 *           (allows quick rotation without left/right adjustment). */
static void led_apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    bool changed = false;
    bool changed_value = false;
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s_led.cur > 0) { s_led.cur--; changed = true; }
            break;
        case MOKYA_KEY_DOWN:
            if (s_led.cur + 1 < LED_W_COUNT) { s_led.cur++; changed = true; }
            break;
        case MOKYA_KEY_OK:
            switch (s_led.cur) {
                case LED_W_RED_ON:
                    s_led.red_on   = !s_led.red_on;   changed_value = true; break;
                case LED_W_GREEN_ON:
                    s_led.green_on = !s_led.green_on; changed_value = true; break;
                case LED_W_KBD_ON:
                    s_led.kbd_on   = !s_led.kbd_on;   changed_value = true; break;
                case LED_W_RED_DUTY:
                    /* +1 with wrap (0..LM27965_DUTY_C_MAX). */
                    s_led.red_duty = (uint8_t)((s_led.red_duty + 1u)
                                                 % (LM27965_DUTY_C_MAX + 1u));
                    changed_value = true;
                    break;
                case LED_W_BANKB_DUTY:
                    /* +1 with wrap, but coarser steps so 0..31 doesn't
                     * need 32 OK presses. Step 4 → 8 stops over the
                     * range, lands on each "perceptual quartile". */
                    s_led.bankb_duty = (uint8_t)((s_led.bankb_duty + 4u)
                                                  % (LM27965_DUTY_AB_MAX + 1u));
                    changed_value = true;
                    break;
                default: break;
            }
            break;
        default: break;
    }
    if (changed_value) led_apply_to_hw();
    if (changed || changed_value) led_render();
}

static void led_refresh(void) { /* nothing async */ }

/* ── Page: GNSS Diag (DIAG_PAGE_GNSS_DIAG) ───────────────────────── *
 *
 * Slimmed-down sibling of rf_debug_view focused on the high-value
 * fields that fit one screen: fix status, position, time, motion,
 * top sat C/N0 list, and (if RF debug is enabled) noise floor. */

/* Reduced from 6 to 4 sat rows so the GST σ + ANF rows fit on one
 * screen (content area 204 px / 14 px per row ≈ 14 rows max). */
#define GNSS_SAT_ROWS  4

static struct {
    lv_obj_t *fix_lbl;          /* Fix + sats + HDOP + Rate */
    lv_obj_t *pos_lbl;
    lv_obj_t *time_lbl;
    lv_obj_t *motion_lbl;
    lv_obj_t *gst_lbl;          /* GST σ_lat / σ_lon / σ_alt */
    lv_obj_t *noise_lbl;
    lv_obj_t *anf_lbl;          /* ANF GPS / GLN summary */
    lv_obj_t *sat_hdr_lbl;
    lv_obj_t *sat_lbls[GNSS_SAT_ROWS];
    lv_obj_t *footer_lbl;
    uint32_t  last_refresh_tick;
    uint32_t  last_sat_update;
} s_gnss_diag __attribute__((section(".psram_bss")));

static void gnss_diag_enter(lv_obj_t *root)
{
    /* 14 px line height; layout from y=0 down to footer at ~190. */
    int y = 0;
    s_gnss_diag.fix_lbl     = mk_diag_label(root, 4, y); y += 14;
    s_gnss_diag.pos_lbl     = mk_diag_label(root, 4, y); y += 14;
    s_gnss_diag.time_lbl    = mk_diag_label(root, 4, y); y += 14;
    s_gnss_diag.motion_lbl  = mk_diag_label(root, 4, y); y += 14;
    s_gnss_diag.gst_lbl     = mk_diag_label(root, 4, y); y += 14;
    s_gnss_diag.noise_lbl   = mk_diag_label(root, 4, y); y += 14;
    s_gnss_diag.anf_lbl     = mk_diag_label(root, 4, y); y += 14;
    s_gnss_diag.sat_hdr_lbl = mk_diag_label(root, 4, y); y += 14;
    lv_label_set_text(s_gnss_diag.sat_hdr_lbl, " PRN  Elev  Az    C/N0");
    for (int i = 0; i < GNSS_SAT_ROWS; i++) {
        s_gnss_diag.sat_lbls[i] = mk_diag_label(root, 4, y + i * 14);
    }
    y += GNSS_SAT_ROWS * 14;
    s_gnss_diag.footer_lbl  = mk_diag_label(root, 4, y);
    s_gnss_diag.last_refresh_tick = 0;
    s_gnss_diag.last_sat_update   = 0;
    gnss_diag_refresh();
}

static void gnss_diag_leave(void)
{
    memset(&s_gnss_diag, 0, sizeof(s_gnss_diag));
}

static void gnss_diag_refresh(void)
{
    if (s_gnss_diag.fix_lbl == NULL) return;
    uint32_t tick = lv_tick_get();
    if ((tick - s_gnss_diag.last_refresh_tick) < 250u) return;
    s_gnss_diag.last_refresh_tick = tick;

    char buf[80];
    const teseo_state_t *t = teseo_get_state();

    if (t == NULL || !t->online) {
        lv_label_set_text(s_gnss_diag.fix_lbl, "GNSS: offline");
        lv_label_set_text(s_gnss_diag.pos_lbl, "");
        lv_label_set_text(s_gnss_diag.time_lbl, "");
        lv_label_set_text(s_gnss_diag.motion_lbl, "");
        lv_label_set_text(s_gnss_diag.gst_lbl, "");
        lv_label_set_text(s_gnss_diag.noise_lbl, "");
        lv_label_set_text(s_gnss_diag.anf_lbl, "");
        return;
    }

    /* Fix line + current fix rate. */
    gnss_rate_t rate = teseo_get_fix_rate();
    const char *rate_str =
        (rate == GNSS_RATE_OFF)  ? "OFF"  :
        (rate == GNSS_RATE_1HZ)  ? "1Hz"  :
        (rate == GNSS_RATE_2HZ)  ? "2Hz"  :
        (rate == GNSS_RATE_5HZ)  ? "5Hz"  :
        (rate == GNSS_RATE_10HZ) ? "10Hz" : "?";
    snprintf(buf, sizeof(buf), "Fix=%s q=%u sats=%u HDOP=%u.%u Rate=%s",
             t->fix_valid ? "VALID" : "no-fix",
             (unsigned)t->fix_quality,
             (unsigned)t->num_sats,
             (unsigned)(t->hdop_x10 / 10u),
             (unsigned)(t->hdop_x10 % 10u),
             rate_str);
    lv_label_set_text(s_gnss_diag.fix_lbl, buf);

    /* lat / lon: show as deg with sign for compactness. */
    int32_t lat = t->lat_e7;
    int32_t lon = t->lon_e7;
    snprintf(buf, sizeof(buf), "Pos %d.%07d, %d.%07d",
             (int)(lat / 10000000), (int)(lat < 0 ? -lat : lat) % 10000000,
             (int)(lon / 10000000), (int)(lon < 0 ? -lon : lon) % 10000000);
    lv_label_set_text(s_gnss_diag.pos_lbl, buf);

    snprintf(buf, sizeof(buf), "UTC %06lu  date %06lu",
             (unsigned long)t->utc_time, (unsigned long)t->utc_date);
    lv_label_set_text(s_gnss_diag.time_lbl, buf);

    snprintf(buf, sizeof(buf), "alt=%d m  spd=%d.%d km/h  hdg=%d.%d deg",
             (int)t->altitude_m,
             (int)(t->speed_kmh_x10 / 10), (int)(t->speed_kmh_x10 % 10),
             (int)(t->course_deg_x10 / 10), (int)(t->course_deg_x10 % 10));
    lv_label_set_text(s_gnss_diag.motion_lbl, buf);

    /* GST σ — position standard deviation in metres. Only populated if
     * RF debug is enabled (Teseo emits $GPGST then). */
    if (t->gst_count > 0u) {
        snprintf(buf, sizeof(buf), "GST sigma lat=%u.%u lon=%u.%u alt=%u.%u m (n=%lu)",
                 (unsigned)(t->gst_sigma_lat_m_x10 / 10u),
                 (unsigned)(t->gst_sigma_lat_m_x10 % 10u),
                 (unsigned)(t->gst_sigma_lon_m_x10 / 10u),
                 (unsigned)(t->gst_sigma_lon_m_x10 % 10u),
                 (unsigned)(t->gst_sigma_alt_m_x10 / 10u),
                 (unsigned)(t->gst_sigma_alt_m_x10 % 10u),
                 (unsigned long)t->gst_count);
    } else {
        snprintf(buf, sizeof(buf), "GST sigma: (no $GPGST — enable RF debug)");
    }
    lv_label_set_text(s_gnss_diag.gst_lbl, buf);

    /* Noise floor + CPU. Only meaningful if RF debug enabled. */
    const teseo_rf_state_t *rf = teseo_get_rf_state();
    if (rf != NULL && rf->noise_count > 0u) {
        snprintf(buf, sizeof(buf), "Noise GPS=%ld GLN=%ld  CPU=%u.%u%% @%uMHz",
                 (long)rf->noise_gps, (long)rf->noise_gln,
                 (unsigned)(rf->cpu_pct_x10 / 10u),
                 (unsigned)(rf->cpu_pct_x10 % 10u),
                 (unsigned)rf->cpu_mhz);
    } else {
        snprintf(buf, sizeof(buf), "Noise/CPU: RF debug disabled (see Cfg page)");
    }
    lv_label_set_text(s_gnss_diag.noise_lbl, buf);

    /* ANF (adaptive notch filter) — jammer detection. */
    if (rf != NULL && rf->anf_count > 0u) {
        snprintf(buf, sizeof(buf),
                 "ANF G f=%lu lk=%u md=%u  L f=%lu lk=%u md=%u",
                 (unsigned long)rf->anf_gps.freq_hz,
                 (unsigned)rf->anf_gps.lock,
                 (unsigned)rf->anf_gps.mode,
                 (unsigned long)rf->anf_gln.freq_hz,
                 (unsigned)rf->anf_gln.lock,
                 (unsigned)rf->anf_gln.mode);
    } else {
        snprintf(buf, sizeof(buf), "ANF: (RF debug disabled)");
    }
    lv_label_set_text(s_gnss_diag.anf_lbl, buf);

    /* Sat table — top 6 by C/N0. Only rebuild if the talker cycle
     * counter advanced (typical 1 Hz). */
    const teseo_sat_view_t *sv = teseo_get_sat_view();
    if (sv != NULL && sv->update_count != s_gnss_diag.last_sat_update) {
        s_gnss_diag.last_sat_update = sv->update_count;

        /* Sort indices by snr_dbhz desc — N small, insertion sort fine. */
        uint8_t order[32];
        uint8_t n = sv->count > 32 ? 32 : sv->count;
        for (uint8_t i = 0; i < n; i++) order[i] = i;
        for (uint8_t i = 1; i < n; i++) {
            uint8_t k = order[i];
            int j = (int)i - 1;
            while (j >= 0 && sv->sats[order[j]].snr_dbhz < sv->sats[k].snr_dbhz) {
                order[j + 1] = order[j]; j--;
            }
            order[j + 1] = k;
        }
        for (int i = 0; i < GNSS_SAT_ROWS; i++) {
            if (i < n) {
                const teseo_sat_info_t *s_ = &sv->sats[order[i]];
                snprintf(buf, sizeof(buf), " %3u  %3u   %3u   %2u dB-Hz",
                         (unsigned)s_->prn,
                         (unsigned)s_->elevation_deg,
                         (unsigned)s_->azimuth_deg,
                         (unsigned)s_->snr_dbhz);
            } else {
                buf[0] = '\0';
            }
            lv_label_set_text(s_gnss_diag.sat_lbls[i], buf);
        }
    }

    /* Footer: sentence + i2c counters + RF debug message counters
     * (proves which RF debug streams are landing). */
    if (rf != NULL) {
        snprintf(buf, sizeof(buf),
                 "sent=%lu i2c_fail=%lu rf=%lu nz=%lu anf=%lu cpu=%lu",
                 (unsigned long)t->sentence_count,
                 (unsigned long)t->i2c_fail_count,
                 (unsigned long)rf->rf_update_count,
                 (unsigned long)rf->noise_count,
                 (unsigned long)rf->anf_count,
                 (unsigned long)rf->cpu_count);
    } else {
        snprintf(buf, sizeof(buf), "sent=%lu i2c_fail=%lu",
                 (unsigned long)t->sentence_count,
                 (unsigned long)t->i2c_fail_count);
    }
    lv_label_set_text(s_gnss_diag.footer_lbl, buf);
}

/* ── Page: GNSS NMEA stream (DIAG_PAGE_GNSS_NMEA) ────────────────── *
 *
 * Live ring of the most recent NMEA sentences. 12 lines × ~16 px each
 * fits 192 px of the 204 px content area. UP toggles pause/resume so
 * the user can read a frozen snapshot. The reader compares
 * teseo_get_raw_seq() against last-seen to skip redraws when nothing
 * new arrived. */

#define NMEA_DISPLAY_LINES   12
/* Reuse TESEO_RAW_LINE_MAX (80) from the driver header. */

static struct {
    lv_obj_t *line_lbls[NMEA_DISPLAY_LINES];
    lv_obj_t *footer_lbl;
    char      buf[NMEA_DISPLAY_LINES][TESEO_RAW_LINE_MAX];
    uint32_t  last_seq;
    bool      paused;
} s_nmea __attribute__((section(".psram_bss")));

static void nmea_render(void)
{
    uint8_t got = teseo_get_raw_lines(s_nmea.buf, NMEA_DISPLAY_LINES);
    for (int i = 0; i < NMEA_DISPLAY_LINES; i++) {
        if (i < got) {
            lv_label_set_text(s_nmea.line_lbls[i], s_nmea.buf[i]);
        } else {
            lv_label_set_text(s_nmea.line_lbls[i], "");
        }
    }
}

static void gnss_nmea_enter(lv_obj_t *root)
{
    for (int i = 0; i < NMEA_DISPLAY_LINES; i++) {
        s_nmea.line_lbls[i] = lv_label_create(root);
        lv_obj_set_pos(s_nmea.line_lbls[i], 2, i * 14);
        lv_obj_set_style_text_font(s_nmea.line_lbls[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_nmea.line_lbls[i],
                                    ui_color(UI_COLOR_TEXT_PRIMARY), 0);
        lv_obj_set_style_pad_all(s_nmea.line_lbls[i], 0, 0);
        lv_label_set_long_mode(s_nmea.line_lbls[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_size(s_nmea.line_lbls[i], 316, 14);
    }
    s_nmea.footer_lbl = mk_diag_label(root, 4, NMEA_DISPLAY_LINES * 14 + 4);
    lv_label_set_text(s_nmea.footer_lbl, "↑ 暫停/恢復");
    s_nmea.last_seq = 0;
    s_nmea.paused = false;
    nmea_render();
}

static void gnss_nmea_leave(void)
{
    memset(&s_nmea, 0, sizeof(s_nmea));
}

static void gnss_nmea_apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    if (ev->keycode == MOKYA_KEY_UP) {
        s_nmea.paused = !s_nmea.paused;
        char buf[64];
        snprintf(buf, sizeof(buf), "↑ %s",
                 s_nmea.paused ? "已暫停 (再按恢復)" : "暫停/恢復");
        lv_label_set_text(s_nmea.footer_lbl, buf);
    }
}

static void gnss_nmea_refresh(void)
{
    if (s_nmea.line_lbls[0] == NULL) return;
    if (s_nmea.paused) return;
    uint32_t seq = teseo_get_raw_seq();
    if (seq == s_nmea.last_seq) return;
    s_nmea.last_seq = seq;
    nmea_render();
}

/* ── Page: GNSS Cfg (DIAG_PAGE_GNSS_CFG) ─────────────────────────── *
 *
 * Runtime control over the Teseo. ↑/↓ moves between widgets, OK
 * triggers the action.  Destructive actions (Restore defaults) require
 * a second OK to confirm. */

typedef enum {
    GCFG_W_FIX_RATE = 0,    /* cycle OFF / 1 / 2 / 5 / 10 Hz */
    GCFG_W_RF_DEBUG,        /* toggle ON / OFF */
    GCFG_W_COLD_START,
    GCFG_W_WARM_START,
    GCFG_W_HOT_START,
    GCFG_W_SAVE_NVM,
    GCFG_W_REBOOT,
    GCFG_W_RESTORE,
    GCFG_W_COUNT
} gcfg_widget_t;

static const char *k_gcfg_labels[GCFG_W_COUNT] = {
    "Fix rate",
    "RF debug",
    "Cold start",
    "Warm start",
    "Hot start",
    "Save to NVM",
    "Engine reboot (SRR)",
    "Restore defaults",
};

static struct {
    lv_obj_t *lbls[GCFG_W_COUNT];
    lv_obj_t *status_lbl;
    gcfg_widget_t cur;
    bool          rf_debug_on;          /* shadow — driver doesn't expose */
    bool          confirm_pending;      /* armed after first OK on Restore */
    uint32_t      confirm_pending_tick; /* expires after 5 s */
    char          last_status[80];
} s_gcfg __attribute__((section(".psram_bss")));

static void gcfg_render(void)
{
    char buf[80];
    for (int w = 0; w < GCFG_W_COUNT; w++) {
        if (s_gcfg.lbls[w] == NULL) continue;
        const char *prefix = (w == (int)s_gcfg.cur) ? "▶ " : "  ";
        switch (w) {
            case GCFG_W_FIX_RATE: {
                gnss_rate_t r = teseo_get_fix_rate();
                const char *rs = (r == GNSS_RATE_OFF)  ? "OFF"  :
                                 (r == GNSS_RATE_1HZ)  ? "1Hz"  :
                                 (r == GNSS_RATE_2HZ)  ? "2Hz"  :
                                 (r == GNSS_RATE_5HZ)  ? "5Hz"  :
                                 (r == GNSS_RATE_10HZ) ? "10Hz" : "?";
                snprintf(buf, sizeof(buf), "%s%s : %s", prefix, k_gcfg_labels[w], rs);
                break;
            }
            case GCFG_W_RF_DEBUG:
                snprintf(buf, sizeof(buf), "%s%s : %s",
                         prefix, k_gcfg_labels[w],
                         s_gcfg.rf_debug_on ? "ON" : "OFF");
                break;
            case GCFG_W_RESTORE:
                if (s_gcfg.confirm_pending && s_gcfg.cur == GCFG_W_RESTORE) {
                    snprintf(buf, sizeof(buf), "%s%s  [OK 再次確認]",
                             prefix, k_gcfg_labels[w]);
                } else {
                    snprintf(buf, sizeof(buf), "%s%s  (DESTRUCTIVE)",
                             prefix, k_gcfg_labels[w]);
                }
                break;
            default:
                snprintf(buf, sizeof(buf), "%s%s", prefix, k_gcfg_labels[w]);
                break;
        }
        lv_label_set_text(s_gcfg.lbls[w], buf);
    }
    if (s_gcfg.status_lbl != NULL) {
        lv_label_set_text(s_gcfg.status_lbl, s_gcfg.last_status);
    }
}

static void gcfg_set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_gcfg.last_status, sizeof(s_gcfg.last_status), fmt, ap);
    va_end(ap);
}

static void gnss_cfg_enter(lv_obj_t *root)
{
    /* 8 widgets × ~22 px = 176 px; status_lbl below at y=180. */
    for (int w = 0; w < GCFG_W_COUNT; w++) {
        s_gcfg.lbls[w] = mk_diag_label(root, 4, w * 22);
    }
    s_gcfg.status_lbl = mk_diag_label(root, 4, 180);
    s_gcfg.cur = GCFG_W_FIX_RATE;
    s_gcfg.confirm_pending = false;
    /* Note: RF debug shadow defaults to "false" — the actual NVM mask
     * is whatever the user (or factory) commissioned. UI displays this
     * shadow only; toggling actually writes via teseo_enable_rf_debug. */
    snprintf(s_gcfg.last_status, sizeof(s_gcfg.last_status), "ready");
    gcfg_render();
}

static void gnss_cfg_leave(void)
{
    memset(&s_gcfg, 0, sizeof(s_gcfg));
}

static void gnss_cfg_apply(const key_event_t *ev)
{
    if (!ev->pressed) return;

    /* Confirm window expires after 5 s of inactivity. */
    if (s_gcfg.confirm_pending) {
        uint32_t tick = lv_tick_get();
        if ((tick - s_gcfg.confirm_pending_tick) > 5000u) {
            s_gcfg.confirm_pending = false;
        }
    }

    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s_gcfg.cur > 0) s_gcfg.cur--;
            s_gcfg.confirm_pending = false;
            gcfg_render();
            return;
        case MOKYA_KEY_DOWN:
            if (s_gcfg.cur + 1 < GCFG_W_COUNT) s_gcfg.cur++;
            s_gcfg.confirm_pending = false;
            gcfg_render();
            return;
        case MOKYA_KEY_OK:
            break;
        default:
            return;
    }

    /* OK action per widget. */
    switch (s_gcfg.cur) {
        case GCFG_W_FIX_RATE: {
            /* Cycle OFF → 1 → 2 → 5 → 10 → OFF. */
            gnss_rate_t r = teseo_get_fix_rate();
            gnss_rate_t next =
                (r == GNSS_RATE_OFF)  ? GNSS_RATE_1HZ  :
                (r == GNSS_RATE_1HZ)  ? GNSS_RATE_2HZ  :
                (r == GNSS_RATE_2HZ)  ? GNSS_RATE_5HZ  :
                (r == GNSS_RATE_5HZ)  ? GNSS_RATE_10HZ :
                                         GNSS_RATE_OFF;
            bool ok = teseo_set_fix_rate(next);
            gcfg_set_status("set rate: %s", ok ? "OK (NVM+SRR)" : "FAIL");
            break;
        }
        case GCFG_W_RF_DEBUG: {
            bool target = !s_gcfg.rf_debug_on;
            bool ok = teseo_enable_rf_debug_messages(target);
            if (ok) s_gcfg.rf_debug_on = target;
            gcfg_set_status("RF debug %s: %s",
                            target ? "ON" : "OFF",
                            ok ? "OK (NVM+SRR)" : "FAIL");
            break;
        }
        case GCFG_W_COLD_START:
            gcfg_set_status("cold start: %s",
                            teseo_cold_start() ? "sent" : "FAIL");
            break;
        case GCFG_W_WARM_START:
            gcfg_set_status("warm start: %s",
                            teseo_warm_start() ? "sent" : "FAIL");
            break;
        case GCFG_W_HOT_START:
            gcfg_set_status("hot start: %s",
                            teseo_hot_start() ? "sent" : "FAIL");
            break;
        case GCFG_W_SAVE_NVM:
            gcfg_set_status("save NVM: %s",
                            teseo_savepar() ? "OK" : "FAIL");
            break;
        case GCFG_W_REBOOT:
            gcfg_set_status("SRR sent (engine reboot ~1-2s)");
            (void)teseo_srr();
            break;
        case GCFG_W_RESTORE:
            if (!s_gcfg.confirm_pending) {
                s_gcfg.confirm_pending = true;
                s_gcfg.confirm_pending_tick = lv_tick_get();
                gcfg_set_status("Press OK again within 5s to confirm");
            } else {
                bool ok1 = teseo_restore_defaults();
                bool ok2 = teseo_savepar();
                (void)teseo_srr();
                s_gcfg.confirm_pending = false;
                gcfg_set_status("restore: %s, save: %s, SRR sent",
                                ok1 ? "ok" : "fail",
                                ok2 ? "ok" : "fail");
            }
            break;
        default: break;
    }
    gcfg_render();
}

static void gnss_cfg_refresh(void)
{
    /* Live-update fix rate display (other UI widgets static). */
    if (s_gcfg.lbls[GCFG_W_FIX_RATE] == NULL) return;
    /* Cheap re-render once per second. */
    static uint32_t last_tick;
    uint32_t tick = lv_tick_get();
    if ((tick - last_tick) < 1000u) return;
    last_tick = tick;
    gcfg_render();
}

/* ── Page: GNSS Const (DIAG_PAGE_GNSS_CONST) ─────────────────────── *
 *
 * 9 single-OK preset selector. ↑/↓ moves focus, OK applies the
 * selected preset (writes CDB-200 + CDB-227 + SAVEPAR + SRR — blocks
 * ~3 s, NMEA stream drops out for ~1-2 s during SRR). The page also
 * tries to read the current device value at entry to mark which preset
 * is currently active with a "✓". GETPAR can be slow; if it fails the
 * UI shows "?" and lets the user pick anyway. */

static const char *k_const_preset_names[9] = {
    "GPS only",
    "GLONASS only",
    "Galileo only",
    "BeiDou only",
    "QZSS only",
    "GPS + SBAS",
    "GPS+GAL+QZSS+GLN",
    "GPS+GAL+QZSS+BD",
    "GLONASS+BeiDou",
};

static struct {
    lv_obj_t *header_lbl;
    lv_obj_t *preset_lbls[9];
    lv_obj_t *status_lbl;
    uint8_t   cur;             /* focus index (0..8) */
    uint8_t   active_preset;   /* 0xFF = unknown / no match */
    uint32_t  cdb200_seen;
    uint32_t  cdb227_seen;
    char      last_status[80];
} s_gconst __attribute__((section(".psram_bss")));

/* Match (cdb200, cdb227) raw bits to a preset by inspecting the masked
 * constellation bits only (other features ignored). Returns 0xFF on
 * no match. */
static uint8_t match_const_preset(uint32_t cdb200, uint32_t cdb227);

static void gconst_render(void)
{
    char buf[64];
    if (s_gconst.header_lbl != NULL) {
        if (s_gconst.active_preset == 0xFFu) {
            snprintf(buf, sizeof(buf), "Active: ? (CDB200=0x%08lX 227=0x%08lX)",
                     (unsigned long)s_gconst.cdb200_seen,
                     (unsigned long)s_gconst.cdb227_seen);
        } else {
            snprintf(buf, sizeof(buf), "Active: %s",
                     k_const_preset_names[s_gconst.active_preset]);
        }
        lv_label_set_text(s_gconst.header_lbl, buf);
    }
    for (int i = 0; i < 9; i++) {
        if (s_gconst.preset_lbls[i] == NULL) continue;
        const char *focus = (i == (int)s_gconst.cur) ? "▶ " : "  ";
        const char *check = (i == (int)s_gconst.active_preset) ? " ✓" : "";
        snprintf(buf, sizeof(buf), "%s%s%s", focus, k_const_preset_names[i], check);
        lv_label_set_text(s_gconst.preset_lbls[i], buf);
    }
    if (s_gconst.status_lbl != NULL) {
        lv_label_set_text(s_gconst.status_lbl, s_gconst.last_status);
    }
}

static uint8_t match_const_preset(uint32_t cdb200, uint32_t cdb227)
{
    /* Precomputed expected bits per preset — must mirror the table in
     * teseo_liv3fl.c k_const_presets[]. */
    static const struct { uint32_t a; uint32_t b; } expected[9] = {
        { 0x00410000u, 0x00000000u },
        { 0x00220000u, 0x00000000u },
        { 0x00000000u, 0x00000180u },
        { 0x00000000u, 0x00000600u },
        { 0x00840000u, 0x00000000u },
        { 0x0041000Cu, 0x00000000u },
        { 0x00E70000u, 0x00000180u },
        { 0x00C50000u, 0x00000780u },
        { 0x00220000u, 0x00000600u },
    };
    uint32_t a = cdb200 & 0x00E7000Cu;
    uint32_t b = cdb227 & 0x00000780u;
    for (uint8_t i = 0; i < 9; i++) {
        if (a == expected[i].a && b == expected[i].b) return i;
    }
    return 0xFFu;
}

static void gconst_enter(lv_obj_t *root)
{
    /* 1 header + 9 preset rows + 1 status. 14 px each = 11 × 14 = 154
     * px. Add 2 px gap between header and list. */
    s_gconst.header_lbl = mk_diag_label(root, 4, 0);
    for (int i = 0; i < 9; i++) {
        s_gconst.preset_lbls[i] = mk_diag_label(root, 4, 18 + i * 14);
    }
    s_gconst.status_lbl = mk_diag_label(root, 4, 18 + 9 * 14 + 6);
    s_gconst.cur = 0;
    s_gconst.active_preset = 0xFFu;
    s_gconst.cdb200_seen = 0;
    s_gconst.cdb227_seen = 0;
    snprintf(s_gconst.last_status, sizeof(s_gconst.last_status),
             "ready (loading current...)");
    gconst_render();
    /* Lazy-load current values on next refresh so view-create doesn't
     * block the LVGL tick. */
}

static void gconst_leave(void)
{
    memset(&s_gconst, 0, sizeof(s_gconst));
}

static void gnss_const_enter(lv_obj_t *root) { gconst_enter(root); }
static void gnss_const_leave(void)            { gconst_leave(); }

static void gnss_const_apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s_gconst.cur > 0) s_gconst.cur--;
            gconst_render();
            return;
        case MOKYA_KEY_DOWN:
            if (s_gconst.cur + 1 < 9) s_gconst.cur++;
            gconst_render();
            return;
        case MOKYA_KEY_OK:
            break;
        default:
            return;
    }

    /* Apply the focused preset. Blocks ~3 s — UI freezes. */
    uint8_t p = s_gconst.cur;
    snprintf(s_gconst.last_status, sizeof(s_gconst.last_status),
             "applying %s ...", k_const_preset_names[p]);
    gconst_render();
    bool ok = teseo_set_constellation_preset((teseo_const_preset_t)p);
    if (ok) {
        s_gconst.active_preset = p;
        snprintf(s_gconst.last_status, sizeof(s_gconst.last_status),
                 "%s applied (NVM+SRR, ~2s reboot)", k_const_preset_names[p]);
    } else {
        snprintf(s_gconst.last_status, sizeof(s_gconst.last_status),
                 "%s FAILED", k_const_preset_names[p]);
    }
    gconst_render();
}

static void gnss_const_refresh(void)
{
    /* Lazy GETPAR — only on first refresh after entry. Subsequent
     * refreshes are no-ops to avoid hammering the bus. */
    static bool s_loaded;
    if (s_gconst.header_lbl == NULL) { s_loaded = false; return; }
    if (s_loaded) return;
    s_loaded = true;

    uint32_t a = 0, b = 0;
    if (teseo_get_constellation_raw(&a, &b)) {
        s_gconst.cdb200_seen = a;
        s_gconst.cdb227_seen = b;
        s_gconst.active_preset = match_const_preset(a, b);
        snprintf(s_gconst.last_status, sizeof(s_gconst.last_status),
                 "↑/↓ select  OK apply (writes NVM + reboot Teseo)");
    } else {
        snprintf(s_gconst.last_status, sizeof(s_gconst.last_status),
                 "GETPAR failed — current preset unknown");
    }
    gconst_render();
}

/* ── Page: GNSS Track (DIAG_PAGE_GNSS_TRACK) ─────────────────────── *
 *
 * 7 widgets — 4 numeric thresholds + 2 cycle (integrity / notch) + 1
 * Apply button. ↑/↓ moves widget focus, OK on numeric advances by step
 * with wrap, OK on cycle widgets advances state, OK on Apply does
 * SAVEPAR + SRR. Each numeric/cycle OK writes SETPAR (RAM only) — must
 * Apply to make permanent. */

typedef enum {
    GTRK_W_MASK_ANGLE = 0,
    GTRK_W_TRACK_CN0,
    GTRK_W_POS_CN0,
    GTRK_W_POS_MASK_ANGLE,
    GTRK_W_INTEGRITY,
    GTRK_W_NOTCH,
    GTRK_W_APPLY,
    GTRK_W_COUNT
} gtrk_widget_t;

#define GTRK_STEP_ANGLE  5u
#define GTRK_MAX_ANGLE   30u
#define GTRK_STEP_DB     5u
#define GTRK_MAX_DB      40u

static const char *k_integrity_modes[4] = {
    "OFF", "Position", "Time", "Both",
};
static const char *k_notch_modes[5] = {
    "OFF", "GPS-normal", "GLN-normal", "GPS+GLN-normal", "Auto-insertion",
};
static const uint8_t k_notch_bits[5] = {
    0x00,                    /* OFF — all bits cleared */
    0x02,                    /* GPS normal mode (b1) */
    0x04,                    /* GLN normal mode (b2) */
    0x06,                    /* GPS + GLN normal */
    0x18,                    /* GPS + GLN auto-insertion (b3+b4) */
};

static struct {
    lv_obj_t *lbls[GTRK_W_COUNT];
    lv_obj_t *status_lbl;
    gtrk_widget_t cur;
    /* Shadow values — written to RAM via SETPAR on each OK. */
    uint8_t mask_angle;       /* 0..30 */
    uint8_t track_cn0;        /* 0..40 */
    uint8_t pos_cn0;          /* 0..40 */
    uint8_t pos_mask_angle;   /* 0..30 */
    uint8_t integrity_mode;   /* 0..3 */
    uint8_t notch_mode;       /* 0..4 (index into k_notch_modes) */
    bool    dirty;
    char    last_status[80];
} s_gtrk __attribute__((section(".psram_bss")));

static void gtrk_render(void)
{
    char buf[80];
    const char *mark = s_gtrk.dirty ? " *" : "";
    for (int w = 0; w < GTRK_W_COUNT; w++) {
        if (s_gtrk.lbls[w] == NULL) continue;
        const char *focus = (w == (int)s_gtrk.cur) ? "▶ " : "  ";
        switch (w) {
            case GTRK_W_MASK_ANGLE:
                snprintf(buf, sizeof(buf), "%sB1 Mask angle      : %u°",
                         focus, (unsigned)s_gtrk.mask_angle);
                break;
            case GTRK_W_TRACK_CN0:
                snprintf(buf, sizeof(buf), "%sB2 Tracking C/N0   : %u dB",
                         focus, (unsigned)s_gtrk.track_cn0);
                break;
            case GTRK_W_POS_CN0:
                snprintf(buf, sizeof(buf), "%sB3 Positioning C/N0: %u dB",
                         focus, (unsigned)s_gtrk.pos_cn0);
                break;
            case GTRK_W_POS_MASK_ANGLE:
                snprintf(buf, sizeof(buf), "%sB4 Pos mask angle  : %u°",
                         focus, (unsigned)s_gtrk.pos_mask_angle);
                break;
            case GTRK_W_INTEGRITY:
                snprintf(buf, sizeof(buf), "%sB5 Integrity check : %s",
                         focus, k_integrity_modes[s_gtrk.integrity_mode & 0x3u]);
                break;
            case GTRK_W_NOTCH:
                snprintf(buf, sizeof(buf), "%sE1 Notch filter    : %s",
                         focus, k_notch_modes[s_gtrk.notch_mode % 5u]);
                break;
            case GTRK_W_APPLY:
                snprintf(buf, sizeof(buf), "%sSave to NVM + reboot%s",
                         focus, mark);
                break;
            default: buf[0] = '\0'; break;
        }
        lv_label_set_text(s_gtrk.lbls[w], buf);
    }
    if (s_gtrk.status_lbl != NULL) {
        lv_label_set_text(s_gtrk.status_lbl, s_gtrk.last_status);
    }
}

static void gnss_track_enter(lv_obj_t *root)
{
    /* 7 widgets × 22 px = 154 px + 8 px gap + status. */
    for (int w = 0; w < GTRK_W_COUNT; w++) {
        s_gtrk.lbls[w] = mk_diag_label(root, 4, w * 22);
    }
    s_gtrk.status_lbl = mk_diag_label(root, 4, 22 * GTRK_W_COUNT + 6);
    s_gtrk.cur = GTRK_W_MASK_ANGLE;
    s_gtrk.dirty = false;
    /* Best-effort load of current values. Defaults if GETPAR fails. */
    uint32_t v;
    s_gtrk.mask_angle      = teseo_getpar_u32(104, &v) ? (uint8_t)v : 5u;
    s_gtrk.track_cn0       = teseo_getpar_u32(105, &v) ? (uint8_t)v : 25u;
    s_gtrk.pos_cn0         = teseo_getpar_u32(132, &v) ? (uint8_t)v : 30u;
    s_gtrk.pos_mask_angle  = teseo_getpar_u32(198, &v) ? (uint8_t)v : 5u;
    s_gtrk.integrity_mode  = teseo_getpar_u32(272, &v) ? (uint8_t)(v & 0x3u) : 0u;
    /* Notch CDB-125: map raw bitmap back to nearest preset (0..4). */
    if (teseo_getpar_u32(125, &v)) {
        uint8_t bits = (uint8_t)v;
        s_gtrk.notch_mode = 0;
        for (uint8_t i = 0; i < 5; i++) {
            if (k_notch_bits[i] == bits) { s_gtrk.notch_mode = i; break; }
        }
    } else {
        s_gtrk.notch_mode = 0;
    }
    snprintf(s_gtrk.last_status, sizeof(s_gtrk.last_status),
             "OK adjusts. Apply at bottom to save NVM + SRR.");
    gtrk_render();
}

static void gnss_track_leave(void)
{
    memset(&s_gtrk, 0, sizeof(s_gtrk));
}

static void gnss_track_apply(const key_event_t *ev)
{
    if (!ev->pressed) return;

    if (ev->keycode == MOKYA_KEY_UP) {
        if (s_gtrk.cur > 0) s_gtrk.cur--;
        gtrk_render();
        return;
    }
    if (ev->keycode == MOKYA_KEY_DOWN) {
        if (s_gtrk.cur + 1 < GTRK_W_COUNT) s_gtrk.cur++;
        gtrk_render();
        return;
    }
    if (ev->keycode != MOKYA_KEY_OK) return;

    bool ok = true;
    switch (s_gtrk.cur) {
        case GTRK_W_MASK_ANGLE:
            s_gtrk.mask_angle = (uint8_t)((s_gtrk.mask_angle + GTRK_STEP_ANGLE)
                                           % (GTRK_MAX_ANGLE + 1u));
            ok = teseo_set_mask_angle(s_gtrk.mask_angle);
            s_gtrk.dirty = true;
            snprintf(s_gtrk.last_status, sizeof(s_gtrk.last_status),
                     "B1 → %u°: %s (RAM)", s_gtrk.mask_angle,
                     ok ? "OK" : "FAIL");
            break;
        case GTRK_W_TRACK_CN0:
            s_gtrk.track_cn0 = (uint8_t)((s_gtrk.track_cn0 + GTRK_STEP_DB)
                                          % (GTRK_MAX_DB + 1u));
            ok = teseo_set_tracking_cn0(s_gtrk.track_cn0);
            s_gtrk.dirty = true;
            snprintf(s_gtrk.last_status, sizeof(s_gtrk.last_status),
                     "B2 → %u dB: %s (RAM)", s_gtrk.track_cn0,
                     ok ? "OK" : "FAIL");
            break;
        case GTRK_W_POS_CN0:
            s_gtrk.pos_cn0 = (uint8_t)((s_gtrk.pos_cn0 + GTRK_STEP_DB)
                                        % (GTRK_MAX_DB + 1u));
            ok = teseo_set_positioning_cn0(s_gtrk.pos_cn0);
            s_gtrk.dirty = true;
            snprintf(s_gtrk.last_status, sizeof(s_gtrk.last_status),
                     "B3 → %u dB: %s (RAM)", s_gtrk.pos_cn0,
                     ok ? "OK" : "FAIL");
            break;
        case GTRK_W_POS_MASK_ANGLE:
            s_gtrk.pos_mask_angle = (uint8_t)((s_gtrk.pos_mask_angle + GTRK_STEP_ANGLE)
                                              % (GTRK_MAX_ANGLE + 1u));
            ok = teseo_set_position_mask_angle(s_gtrk.pos_mask_angle);
            s_gtrk.dirty = true;
            snprintf(s_gtrk.last_status, sizeof(s_gtrk.last_status),
                     "B4 → %u°: %s (RAM)", s_gtrk.pos_mask_angle,
                     ok ? "OK" : "FAIL");
            break;
        case GTRK_W_INTEGRITY:
            s_gtrk.integrity_mode = (uint8_t)((s_gtrk.integrity_mode + 1u) & 0x3u);
            ok = teseo_set_integrity_check(s_gtrk.integrity_mode);
            s_gtrk.dirty = true;
            snprintf(s_gtrk.last_status, sizeof(s_gtrk.last_status),
                     "B5 → %s: %s (RAM)",
                     k_integrity_modes[s_gtrk.integrity_mode], ok ? "OK" : "FAIL");
            break;
        case GTRK_W_NOTCH:
            s_gtrk.notch_mode = (uint8_t)((s_gtrk.notch_mode + 1u) % 5u);
            ok = teseo_set_notch_filter(k_notch_bits[s_gtrk.notch_mode]);
            s_gtrk.dirty = true;
            snprintf(s_gtrk.last_status, sizeof(s_gtrk.last_status),
                     "E1 → %s: %s (RAM)",
                     k_notch_modes[s_gtrk.notch_mode], ok ? "OK" : "FAIL");
            break;
        case GTRK_W_APPLY: {
            bool save = teseo_savepar();
            (void)teseo_srr();
            s_gtrk.dirty = false;
            snprintf(s_gtrk.last_status, sizeof(s_gtrk.last_status),
                     "Apply: save %s, SRR sent", save ? "OK" : "FAIL");
            break;
        }
        default: break;
    }
    gtrk_render();
}

static void gnss_track_refresh(void) { /* no live refresh */ }

/* ── Page: GNSS NMEA Cfg (DIAG_PAGE_GNSS_NMEA_CFG) ───────────────── *
 *
 * D4 — Talker ID (CDB-131): cycle through {P, N, A, B, L} which gives
 *      GP / GN / GA / GB / GL talker prefix on GGA/RMC/VTG/GLL.
 * D5 — NMEA preset: cycle through Minimal / Normal / Full / Debug.
 *      Implementation note: only Debug differs functionally from the
 *      others (toggles RF debug stream); the 4 names map to mode 2
 *      (clear) for Min/Normal/Full and mode 1 (OR-set) for Debug.
 *      Standard NMEA always inherits from factory NVM.
 * Apply — SAVEPAR + SRR (each setting has already done that internally
 *         per call, so this is a manual re-trigger if needed). */

typedef enum {
    GNMEA_W_TALKER_ID = 0,
    GNMEA_W_PRESET,
    GNMEA_W_APPLY,
    GNMEA_W_COUNT
} gnmea_widget_t;

static const char k_talker_chars[5] = { 'P', 'N', 'A', 'B', 'L' };
static const char *k_preset_names[TESEO_NMEA_PRESET_COUNT] = {
    "Minimal", "Normal", "Full", "Debug (+RF)",
};

static struct {
    lv_obj_t *lbls[GNMEA_W_COUNT];
    lv_obj_t *status_lbl;
    gnmea_widget_t cur;
    uint8_t talker_idx;        /* index into k_talker_chars */
    uint8_t preset;             /* TESEO_NMEA_PRESET_* */
    char    last_status[80];
} s_gnmea __attribute__((section(".psram_bss")));

static void gnmea_render(void)
{
    char buf[80];
    for (int w = 0; w < GNMEA_W_COUNT; w++) {
        if (s_gnmea.lbls[w] == NULL) continue;
        const char *focus = (w == (int)s_gnmea.cur) ? "▶ " : "  ";
        switch (w) {
            case GNMEA_W_TALKER_ID:
                snprintf(buf, sizeof(buf), "%sD4 Talker ID    : G%c",
                         focus, k_talker_chars[s_gnmea.talker_idx % 5]);
                break;
            case GNMEA_W_PRESET:
                snprintf(buf, sizeof(buf), "%sD5 NMEA preset  : %s",
                         focus, k_preset_names[s_gnmea.preset % TESEO_NMEA_PRESET_COUNT]);
                break;
            case GNMEA_W_APPLY:
                snprintf(buf, sizeof(buf), "%sSave NVM + reboot Teseo", focus);
                break;
            default: buf[0] = '\0'; break;
        }
        lv_label_set_text(s_gnmea.lbls[w], buf);
    }
    if (s_gnmea.status_lbl != NULL) {
        lv_label_set_text(s_gnmea.status_lbl, s_gnmea.last_status);
    }
}

static void gnss_nmea_cfg_enter(lv_obj_t *root)
{
    for (int w = 0; w < GNMEA_W_COUNT; w++) {
        s_gnmea.lbls[w] = mk_diag_label(root, 4, w * 22);
    }
    s_gnmea.status_lbl = mk_diag_label(root, 4, 22 * GNMEA_W_COUNT + 6);
    s_gnmea.cur = GNMEA_W_TALKER_ID;
    s_gnmea.talker_idx = 0;        /* P (default) */
    s_gnmea.preset = TESEO_NMEA_PRESET_NORMAL;
    /* Try to restore current talker ID from NVM. */
    uint32_t v;
    if (teseo_getpar_u32(131, &v)) {
        char id = (char)(v & 0xFFu);
        for (uint8_t i = 0; i < 5; i++) {
            if (k_talker_chars[i] == id) { s_gnmea.talker_idx = i; break; }
        }
    }
    snprintf(s_gnmea.last_status, sizeof(s_gnmea.last_status),
             "↑/↓ select  OK adjust");
    gnmea_render();
}

static void gnss_nmea_cfg_leave(void)
{
    memset(&s_gnmea, 0, sizeof(s_gnmea));
}

static void gnss_nmea_cfg_apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    if (ev->keycode == MOKYA_KEY_UP) {
        if (s_gnmea.cur > 0) s_gnmea.cur--;
        gnmea_render();
        return;
    }
    if (ev->keycode == MOKYA_KEY_DOWN) {
        if (s_gnmea.cur + 1 < GNMEA_W_COUNT) s_gnmea.cur++;
        gnmea_render();
        return;
    }
    if (ev->keycode != MOKYA_KEY_OK) return;

    switch (s_gnmea.cur) {
        case GNMEA_W_TALKER_ID: {
            s_gnmea.talker_idx = (uint8_t)((s_gnmea.talker_idx + 1u) % 5u);
            char id = k_talker_chars[s_gnmea.talker_idx];
            bool ok = teseo_set_talker_id(id);
            snprintf(s_gnmea.last_status, sizeof(s_gnmea.last_status),
                     "Talker → G%c: %s (RAM)", id, ok ? "OK" : "FAIL");
            break;
        }
        case GNMEA_W_PRESET: {
            s_gnmea.preset = (uint8_t)((s_gnmea.preset + 1u) % TESEO_NMEA_PRESET_COUNT);
            bool ok = teseo_set_nmea_preset((teseo_nmea_preset_t)s_gnmea.preset);
            snprintf(s_gnmea.last_status, sizeof(s_gnmea.last_status),
                     "Preset → %s: %s (NVM+SRR)",
                     k_preset_names[s_gnmea.preset], ok ? "OK" : "FAIL");
            break;
        }
        case GNMEA_W_APPLY: {
            bool save = teseo_savepar();
            (void)teseo_srr();
            snprintf(s_gnmea.last_status, sizeof(s_gnmea.last_status),
                     "Save: %s, SRR sent", save ? "OK" : "FAIL");
            break;
        }
        default: break;
    }
    gnmea_render();
}

static void gnss_nmea_cfg_refresh(void) { /* no live refresh */ }

/* ── Page: GNSS Adv (DIAG_PAGE_GNSS_ADV) ─────────────────────────── *
 *
 * F7 Odometer  — 3 toggle bits + alarm distance (cycle 0/100/500/1000m)
 * F8 Logger    — enabled + min distance (cycle 1/10/100m)
 * F6 Geofence  — read-only display of current 4 circles (editing
 *                requires digit-input widget; deferred). */

typedef enum {
    GADV_W_ODO_EN = 0,
    GADV_W_ODO_NMEA,
    GADV_W_ODO_AUTO,
    GADV_W_ODO_ALARM,
    GADV_W_LOG_EN,
    GADV_W_LOG_MIN,
    GADV_W_GEOFENCE_HEADER,    /* selectable but no-op (just labels read-only F6) */
    GADV_W_APPLY,
    GADV_W_COUNT
} gadv_widget_t;

static const uint16_t k_alarm_steps[] = { 0, 100, 500, 1000, 5000, 0xFFFF };
#define ALARM_STEP_COUNT (sizeof(k_alarm_steps) / sizeof(k_alarm_steps[0]))

static const uint16_t k_logmin_steps[] = { 1, 5, 10, 50, 100, 500, 1000 };
#define LOGMIN_STEP_COUNT (sizeof(k_logmin_steps) / sizeof(k_logmin_steps[0]))

static struct {
    lv_obj_t *lbls[GADV_W_COUNT];
    lv_obj_t *fence_lbls[4];
    lv_obj_t *status_lbl;
    gadv_widget_t cur;
    teseo_odometer_cfg_t odo;
    teseo_logger_cfg_t   log;
    uint8_t alarm_step_idx;
    uint8_t logmin_step_idx;
    char    last_status[80];
} s_gadv __attribute__((section(".psram_bss")));

static uint8_t step_idx_for_alarm(uint16_t v)
{
    for (uint8_t i = 0; i < ALARM_STEP_COUNT; i++)
        if (k_alarm_steps[i] == v) return i;
    return 0;
}
static uint8_t step_idx_for_logmin(uint16_t v)
{
    for (uint8_t i = 0; i < LOGMIN_STEP_COUNT; i++)
        if (k_logmin_steps[i] >= v) return i;
    return 0;
}

static void gadv_render(void)
{
    char buf[80];
    for (int w = 0; w < GADV_W_COUNT; w++) {
        if (s_gadv.lbls[w] == NULL) continue;
        const char *focus = (w == (int)s_gadv.cur) ? "▶ " : "  ";
        switch (w) {
            case GADV_W_ODO_EN:
                snprintf(buf, sizeof(buf), "%sF7 Odo enable    : %s", focus,
                         s_gadv.odo.enabled_on_boot ? "ON" : "OFF");
                break;
            case GADV_W_ODO_NMEA:
                snprintf(buf, sizeof(buf), "%sF7 Odo NMEA out  : %s", focus,
                         s_gadv.odo.nmea_enabled ? "ON" : "OFF");
                break;
            case GADV_W_ODO_AUTO:
                snprintf(buf, sizeof(buf), "%sF7 Odo autostart : %s", focus,
                         s_gadv.odo.autostart ? "ON" : "OFF");
                break;
            case GADV_W_ODO_ALARM:
                snprintf(buf, sizeof(buf), "%sF7 Odo alarm     : %u m", focus,
                         (unsigned)s_gadv.odo.alarm_m);
                break;
            case GADV_W_LOG_EN:
                snprintf(buf, sizeof(buf), "%sF8 Logger enable : %s", focus,
                         s_gadv.log.enabled_on_boot ? "ON" : "OFF");
                break;
            case GADV_W_LOG_MIN:
                snprintf(buf, sizeof(buf), "%sF8 Log min dist  : %u m", focus,
                         (unsigned)s_gadv.log.min_distance_m);
                break;
            case GADV_W_GEOFENCE_HEADER:
                snprintf(buf, sizeof(buf), "%sF6 Geofence (read-only)", focus);
                break;
            case GADV_W_APPLY:
                snprintf(buf, sizeof(buf), "%sSave NVM + reboot Teseo", focus);
                break;
            default: buf[0] = '\0'; break;
        }
        lv_label_set_text(s_gadv.lbls[w], buf);
    }
    if (s_gadv.status_lbl != NULL) {
        lv_label_set_text(s_gadv.status_lbl, s_gadv.last_status);
    }
}

static void gnss_adv_enter(lv_obj_t *root)
{
    /* 8 main widgets × 18 px = 144 px, 4 fence rows × 14 = 56 px,
     * status 14 — total ~214 px (just over 204 — squeeze fences below
     * status; LVGL will clip). For now, drop fence rows in this
     * minimal v1 — they're shown only via the header line. */
    for (int w = 0; w < GADV_W_COUNT; w++) {
        s_gadv.lbls[w] = mk_diag_label(root, 4, w * 22);
    }
    s_gadv.status_lbl = mk_diag_label(root, 4, 22 * GADV_W_COUNT + 4);
    s_gadv.cur = GADV_W_ODO_EN;
    /* Best-effort load. */
    if (!teseo_get_odometer_cfg(&s_gadv.odo)) {
        memset(&s_gadv.odo, 0, sizeof(s_gadv.odo));
    }
    if (!teseo_get_logger_cfg(&s_gadv.log)) {
        memset(&s_gadv.log, 0, sizeof(s_gadv.log));
        s_gadv.log.min_distance_m = 10;
    }
    s_gadv.alarm_step_idx  = step_idx_for_alarm(s_gadv.odo.alarm_m);
    s_gadv.logmin_step_idx = step_idx_for_logmin(s_gadv.log.min_distance_m);
    snprintf(s_gadv.last_status, sizeof(s_gadv.last_status),
             "↑/↓ select  OK toggle/cycle  (F6 edit deferred)");
    gadv_render();
}

static void gnss_adv_leave(void)
{
    memset(&s_gadv, 0, sizeof(s_gadv));
}

static void gnss_adv_apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    if (ev->keycode == MOKYA_KEY_UP) {
        if (s_gadv.cur > 0) s_gadv.cur--;
        gadv_render();
        return;
    }
    if (ev->keycode == MOKYA_KEY_DOWN) {
        if (s_gadv.cur + 1 < GADV_W_COUNT) s_gadv.cur++;
        gadv_render();
        return;
    }
    if (ev->keycode != MOKYA_KEY_OK) return;

    bool ok = true;
    switch (s_gadv.cur) {
        case GADV_W_ODO_EN:
            s_gadv.odo.enabled_on_boot = !s_gadv.odo.enabled_on_boot;
            ok = teseo_set_odometer_cfg(&s_gadv.odo);
            snprintf(s_gadv.last_status, sizeof(s_gadv.last_status),
                     "Odo enable %s: %s",
                     s_gadv.odo.enabled_on_boot ? "ON" : "OFF",
                     ok ? "OK" : "FAIL");
            break;
        case GADV_W_ODO_NMEA:
            s_gadv.odo.nmea_enabled = !s_gadv.odo.nmea_enabled;
            ok = teseo_set_odometer_cfg(&s_gadv.odo);
            snprintf(s_gadv.last_status, sizeof(s_gadv.last_status),
                     "Odo NMEA %s: %s",
                     s_gadv.odo.nmea_enabled ? "ON" : "OFF",
                     ok ? "OK" : "FAIL");
            break;
        case GADV_W_ODO_AUTO:
            s_gadv.odo.autostart = !s_gadv.odo.autostart;
            ok = teseo_set_odometer_cfg(&s_gadv.odo);
            snprintf(s_gadv.last_status, sizeof(s_gadv.last_status),
                     "Odo auto %s: %s",
                     s_gadv.odo.autostart ? "ON" : "OFF",
                     ok ? "OK" : "FAIL");
            break;
        case GADV_W_ODO_ALARM:
            s_gadv.alarm_step_idx = (uint8_t)((s_gadv.alarm_step_idx + 1u)
                                              % ALARM_STEP_COUNT);
            s_gadv.odo.alarm_m = k_alarm_steps[s_gadv.alarm_step_idx];
            ok = teseo_set_odometer_cfg(&s_gadv.odo);
            snprintf(s_gadv.last_status, sizeof(s_gadv.last_status),
                     "Odo alarm %u m: %s", (unsigned)s_gadv.odo.alarm_m,
                     ok ? "OK" : "FAIL");
            break;
        case GADV_W_LOG_EN:
            s_gadv.log.enabled_on_boot = !s_gadv.log.enabled_on_boot;
            ok = teseo_set_logger_cfg(&s_gadv.log);
            snprintf(s_gadv.last_status, sizeof(s_gadv.last_status),
                     "Logger %s: %s",
                     s_gadv.log.enabled_on_boot ? "ON" : "OFF",
                     ok ? "OK" : "FAIL");
            break;
        case GADV_W_LOG_MIN:
            s_gadv.logmin_step_idx = (uint8_t)((s_gadv.logmin_step_idx + 1u)
                                                % LOGMIN_STEP_COUNT);
            s_gadv.log.min_distance_m = k_logmin_steps[s_gadv.logmin_step_idx];
            ok = teseo_set_logger_cfg(&s_gadv.log);
            snprintf(s_gadv.last_status, sizeof(s_gadv.last_status),
                     "Log min %u m: %s",
                     (unsigned)s_gadv.log.min_distance_m,
                     ok ? "OK" : "FAIL");
            break;
        case GADV_W_GEOFENCE_HEADER:
            snprintf(s_gadv.last_status, sizeof(s_gadv.last_status),
                     "F6 edit deferred — use SWD writes for now");
            break;
        case GADV_W_APPLY:
            (void)teseo_savepar();
            (void)teseo_srr();
            snprintf(s_gadv.last_status, sizeof(s_gadv.last_status),
                     "saved + SRR");
            break;
        default: break;
    }
    gadv_render();
}

static void gnss_adv_refresh(void) { /* no live refresh */ }

/* ── Page: Notification settings (DIAG_PAGE_NOTIF) ─────────────────── *
 *
 * Three sub-pages walked via FUNC (HW Diag's only spare key under the
 * router):
 *   sub 0 — global event defaults (Master / DnD / Vib intensity / 8 events)
 *   sub 1 — channel overrides (8 channels)
 *   sub 2 — per-conversation overrides (8 slots)
 *
 * UP/DOWN moves widget focus, OK cycles modes (NONE → SILENT → VIBRATE
 * → DEFAULT-fallback for layers 2+3, just NONE → SILENT → VIBRATE for
 * layer 1) or toggles bools / bumps numeric. Each mutation flips the
 * notification dirty flag so notif_persist's debounce timer flushes
 * within 5 s. */

#include "notification.h"

typedef enum {
    NOTIF_SUB_GLOBAL = 0,
    NOTIF_SUB_CHANNELS,
    NOTIF_SUB_PER_CONV,
    NOTIF_SUB_COUNT
} notif_sub_t;

/* master / dnd / quiet-hours / vib-int + 8 events = 12 */
#define NOTIF_GLOBAL_ROWS  (4 + NOTIF_EVENT_COUNT)

typedef struct {
    lv_obj_t   *header_lbl;
    lv_obj_t   *row_lbls[NOTIF_GLOBAL_ROWS];   /* reused across sub-pages */
    lv_obj_t   *hint_lbl;
    notif_sub_t sub;
    uint8_t     cur_row;                       /* 0..N-1 within sub-page */
} notif_page_t;

static notif_page_t s_notif __attribute__((section(".psram_bss")));

static const char *notif_event_name(notif_event_t ev)
{
    switch (ev) {
        case NOTIF_EVENT_DM:        return "DM";
        case NOTIF_EVENT_BROADCAST: return "Broadcast";
        case NOTIF_EVENT_ACK:       return "ACK";
        case NOTIF_EVENT_NEW_NODE:  return "NewNode";
        case NOTIF_EVENT_CHARGE:    return "Charge";
        case NOTIF_EVENT_LOW_BATT:  return "LowBatt";
        case NOTIF_EVENT_KEYPRESS:  return "KeyPress";
        case NOTIF_EVENT_SOS:       return "SOS";
        default:                    return "?";
    }
}

static const char *notif_mode_name(notif_mode_t m)
{
    switch (m) {
        case NOTIF_MODE_NONE:    return "OFF";
        case NOTIF_MODE_SILENT:  return "SIL";
        case NOTIF_MODE_VIBRATE: return "VIB";
        case NOTIF_MODE_DEFAULT: return "DEF";
        default:                 return "?";
    }
}

static notif_mode_t notif_mode_cycle3(notif_mode_t m)
{
    /* OFF → SIL → VIB → OFF (3-state, no DEFAULT). */
    switch (m) {
        case NOTIF_MODE_NONE:    return NOTIF_MODE_SILENT;
        case NOTIF_MODE_SILENT:  return NOTIF_MODE_VIBRATE;
        case NOTIF_MODE_VIBRATE: return NOTIF_MODE_NONE;
        default:                 return NOTIF_MODE_NONE;
    }
}

static notif_mode_t notif_mode_cycle4(notif_mode_t m)
{
    /* OFF → SIL → VIB → DEF → OFF (4-state, includes DEFAULT/clear). */
    switch (m) {
        case NOTIF_MODE_NONE:    return NOTIF_MODE_SILENT;
        case NOTIF_MODE_SILENT:  return NOTIF_MODE_VIBRATE;
        case NOTIF_MODE_VIBRATE: return NOTIF_MODE_DEFAULT;
        case NOTIF_MODE_DEFAULT: return NOTIF_MODE_NONE;
        default:                 return NOTIF_MODE_DEFAULT;
    }
}

static uint8_t notif_rows_for_sub(notif_sub_t s)
{
    switch (s) {
        case NOTIF_SUB_GLOBAL:   return NOTIF_GLOBAL_ROWS;
        case NOTIF_SUB_CHANNELS: return NOTIF_CHANNEL_COUNT;
        case NOTIF_SUB_PER_CONV: return NOTIF_PER_CONV_MAX;
        default:                 return 0;
    }
}

static void notif_render_global(notif_settings_t *cfg, char *buf, size_t cap,
                                uint8_t row)
{
    const char *focus = (row == s_notif.cur_row) ? "▶ " : "  ";
    if (row == 0) {
        snprintf(buf, cap, "%sMaster      : %s",
                 focus, cfg->master_enable ? "ON" : "OFF");
        return;
    }
    if (row == 1) {
        /* DnD = manual mute toggle, no time window. */
        snprintf(buf, cap, "%sDnD         : %s",
                 focus, cfg->dnd_enable ? "ON" : "OFF");
        return;
    }
    if (row == 2) {
        /* Quiet Hours = scheduled time window, needs synced clock. */
        if (cfg->quiet_hours_enable) {
            snprintf(buf, cap, "%sQuiet %02u:%02u-%02u:%02u",
                     focus,
                     (unsigned)(cfg->quiet_start_min / 60u),
                     (unsigned)(cfg->quiet_start_min % 60u),
                     (unsigned)(cfg->quiet_end_min / 60u),
                     (unsigned)(cfg->quiet_end_min % 60u));
        } else {
            snprintf(buf, cap, "%sQuiet Hours : OFF", focus);
        }
        return;
    }
    if (row == 3) {
        snprintf(buf, cap, "%sVibInt      : %u%%",
                 focus, (unsigned)cfg->vib_intensity);
        return;
    }
    notif_event_t ev = (notif_event_t)(row - 4);
    notif_mode_t m = notif_event_mode_get(cfg, ev);
    snprintf(buf, cap, "%s%-11s : %s",
             focus, notif_event_name(ev), notif_mode_name(m));
}

static void notif_render_channels(notif_settings_t *cfg, char *buf, size_t cap,
                                  uint8_t row)
{
    const char *focus = (row == s_notif.cur_row) ? "▶ " : "  ";
    notif_mode_t m = notif_channel_mode_get(cfg, row);
    snprintf(buf, cap, "%sCh %u (DM/BC) : %s",
             focus, (unsigned)row, notif_mode_name(m));
}

static void notif_render_per_conv(notif_settings_t *cfg, char *buf, size_t cap,
                                  uint8_t row)
{
    const char *focus = (row == s_notif.cur_row) ? "▶ " : "  ";
    notif_per_conv_t *p = &cfg->per_conv[row];
    if (p->node_num == 0u) {
        snprintf(buf, cap, "%s[empty %u]", focus, (unsigned)row);
    } else {
        snprintf(buf, cap, "%s!%08lx : %s",
                 focus, (unsigned long)p->node_num,
                 notif_mode_name((notif_mode_t)p->mode));
    }
}

static void notif_render(void)
{
    notif_settings_t *cfg = notification_get_settings();
    if (s_notif.header_lbl != NULL) {
        const char *sub_name =
            (s_notif.sub == NOTIF_SUB_GLOBAL)   ? "Global Defaults" :
            (s_notif.sub == NOTIF_SUB_CHANNELS) ? "Channel Override" :
                                                   "Conversation Override";
        char hdr[80];
        snprintf(hdr, sizeof(hdr), "[NOTIF %u/3] %s",
                 (unsigned)(s_notif.sub + 1u), sub_name);
        lv_label_set_text(s_notif.header_lbl, hdr);
    }
    uint8_t total = notif_rows_for_sub(s_notif.sub);
    char buf[80];
    for (int i = 0; i < NOTIF_GLOBAL_ROWS; i++) {
        if (s_notif.row_lbls[i] == NULL) continue;
        if (i >= total) {
            lv_label_set_text(s_notif.row_lbls[i], "");
            continue;
        }
        switch (s_notif.sub) {
            case NOTIF_SUB_GLOBAL:
                notif_render_global(cfg, buf, sizeof(buf), (uint8_t)i);
                break;
            case NOTIF_SUB_CHANNELS:
                notif_render_channels(cfg, buf, sizeof(buf), (uint8_t)i);
                break;
            case NOTIF_SUB_PER_CONV:
                notif_render_per_conv(cfg, buf, sizeof(buf), (uint8_t)i);
                break;
            default: buf[0] = '\0'; break;
        }
        lv_label_set_text(s_notif.row_lbls[i], buf);
    }
    if (s_notif.hint_lbl != NULL) {
        lv_label_set_text(s_notif.hint_lbl,
                          "↑/↓ 移動  OK 切換  FUNC 換頁  ◀▶ 換 HW 頁");
    }
}

static void notif_enter(lv_obj_t *root)
{
    s_notif.header_lbl = mk_diag_label(root, 4, 0);
    /* 11 rows × 16 px starting at y=20. */
    for (int i = 0; i < NOTIF_GLOBAL_ROWS; i++) {
        s_notif.row_lbls[i] = mk_diag_label(root, 4, 20 + i * 16);
    }
    s_notif.hint_lbl = mk_diag_label(root, 4, 20 + NOTIF_GLOBAL_ROWS * 16 + 4);
    s_notif.sub      = NOTIF_SUB_GLOBAL;
    s_notif.cur_row  = 0;
    notif_render();
}

static void notif_leave(void)
{
    memset(&s_notif, 0, sizeof(s_notif));
}

static void notif_apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    notif_settings_t *cfg = notification_get_settings();
    uint8_t total = notif_rows_for_sub(s_notif.sub);
    bool changed = false;
    bool dirty   = false;
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s_notif.cur_row > 0) { s_notif.cur_row--; changed = true; }
            break;
        case MOKYA_KEY_DOWN:
            if (s_notif.cur_row + 1 < total) { s_notif.cur_row++; changed = true; }
            break;
        case MOKYA_KEY_FUNC:
            s_notif.sub = (notif_sub_t)((s_notif.sub + 1u) % NOTIF_SUB_COUNT);
            s_notif.cur_row = 0;
            changed = true;
            break;
        case MOKYA_KEY_OK:
            switch (s_notif.sub) {
                case NOTIF_SUB_GLOBAL:
                    if (s_notif.cur_row == 0) {
                        cfg->master_enable = !cfg->master_enable;
                        dirty = true;
                    } else if (s_notif.cur_row == 1) {
                        cfg->dnd_enable = !cfg->dnd_enable;
                        dirty = true;
                    } else if (s_notif.cur_row == 2) {
                        cfg->quiet_hours_enable = !cfg->quiet_hours_enable;
                        dirty = true;
                    } else if (s_notif.cur_row == 3) {
                        /* +10 % wraps 0..100. */
                        uint16_t v = (uint16_t)cfg->vib_intensity + 10u;
                        if (v > 100u) v = 0u;
                        cfg->vib_intensity = (uint8_t)v;
                        dirty = true;
                    } else {
                        notif_event_t e =
                            (notif_event_t)(s_notif.cur_row - 4);
                        notif_mode_t m = notif_event_mode_get(cfg, e);
                        notif_event_mode_set(cfg, e, notif_mode_cycle3(m));
                        dirty = true;
                    }
                    break;
                case NOTIF_SUB_CHANNELS: {
                    notif_mode_t m = notif_channel_mode_get(cfg, s_notif.cur_row);
                    notif_channel_mode_set(cfg, s_notif.cur_row,
                                            notif_mode_cycle4(m));
                    dirty = true;
                    break;
                }
                case NOTIF_SUB_PER_CONV: {
                    notif_per_conv_t *p = &cfg->per_conv[s_notif.cur_row];
                    if (p->node_num == 0u) {
                        /* Empty slot: nothing to cycle yet — adding a
                         * peer requires a node-picker (deferred to
                         * next iteration). */
                    } else {
                        notif_mode_t m = (notif_mode_t)p->mode;
                        notif_mode_t next = notif_mode_cycle4(m);
                        notif_per_conv_upsert(cfg, p->node_num, next);
                        dirty = true;
                    }
                    break;
                }
                default: break;
            }
            break;
        default: break;
    }
    if (dirty) notification_settings_dirty();
    if (changed || dirty) notif_render();
}

static void notif_refresh(void) { /* no live data */ }

const view_descriptor_t *hw_diag_view_descriptor(void)
{
    return &HW_DIAG_DESC;
}
