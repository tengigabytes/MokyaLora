/* rf_debug_view.c — see rf_debug_view.h. */

#include "rf_debug_view.h"

#include <stdio.h>
#include <string.h>

#include "teseo_liv3fl.h"

/* ── Layout (landscape 320x240, lv_font_montserrat_14, ~14 px line) ─── *
 *
 *   y   0 ..  13  title + view-of-used sat count
 *   y  14 ..  27  fix status / sats / HDOP / accuracy σ
 *   y  28 ..  41  UTC time + date
 *   y  42 ..  55  Position (lat, lon)
 *   y  56 ..  69  Altitude / speed / heading
 *   y  70 ..  83  Noise floor + CPU
 *   y  84 ..  97  ANF GPS
 *   y  98 .. 111  ANF GLN
 *   y 112 .. 125  NMEA throughput counters + I2C fail
 *   y 126 .. 139  Sat table header
 *   y 140 .. 223  6 sat rows × 14 px
 *   y 224 .. 239  hint / status footer
 */

#define COL_FG          lv_color_hex(0xE0E0E0)
#define COL_BG          lv_color_hex(0x000000)
#define COL_TITLE       lv_color_hex(0x00E0E0)
#define COL_WARN        lv_color_hex(0xFFB000)
#define COL_OK          lv_color_hex(0x00D060)
#define COL_DIM         lv_color_hex(0x808080)

#define ROW_TITLE         0
#define ROW_FIX          14
#define ROW_TIME         28
#define ROW_POS          42
#define ROW_MOTION       56
#define ROW_RF_BASE      70
#define ROW_ANF_GPS      84
#define ROW_ANF_GLN      98
#define ROW_COUNTERS    112
#define ROW_SAT_HDR     126
#define ROW_SAT_FIRST   140
#define ROW_SAT_STEP     14
#define SAT_ROWS          6       /* 140 + 6*14 = 224 */
#define ROW_FOOTER      224

/* Widget handles. */
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_fix_lbl;
static lv_obj_t *s_time_lbl;
static lv_obj_t *s_pos_lbl;
static lv_obj_t *s_motion_lbl;
static lv_obj_t *s_rf_lbl;
static lv_obj_t *s_anf_gps_lbl;
static lv_obj_t *s_anf_gln_lbl;
static lv_obj_t *s_counters_lbl;
static lv_obj_t *s_sat_hdr_lbl;
static lv_obj_t *s_sat_lbl[SAT_ROWS];
static lv_obj_t *s_footer_lbl;

static lv_obj_t *mk_label(lv_obj_t *parent, int x, int y, lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    return lbl;
}

void rf_debug_view_init(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, COL_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    s_title_lbl    = mk_label(parent,   4, ROW_TITLE,    COL_TITLE);
    s_fix_lbl      = mk_label(parent,   4, ROW_FIX,      COL_FG);
    s_time_lbl     = mk_label(parent,   4, ROW_TIME,     COL_FG);
    s_pos_lbl      = mk_label(parent,   4, ROW_POS,      COL_FG);
    s_motion_lbl   = mk_label(parent,   4, ROW_MOTION,   COL_FG);
    s_rf_lbl       = mk_label(parent,   4, ROW_RF_BASE,  COL_FG);
    s_anf_gps_lbl  = mk_label(parent,   4, ROW_ANF_GPS,  COL_FG);
    s_anf_gln_lbl  = mk_label(parent,   4, ROW_ANF_GLN,  COL_FG);
    s_counters_lbl = mk_label(parent,   4, ROW_COUNTERS, COL_DIM);
    s_sat_hdr_lbl  = mk_label(parent,   4, ROW_SAT_HDR,  COL_DIM);
    for (int i = 0; i < SAT_ROWS; ++i) {
        s_sat_lbl[i] = mk_label(parent, 4,
                                ROW_SAT_FIRST + i * ROW_SAT_STEP, COL_FG);
        lv_label_set_text(s_sat_lbl[i], "");
    }
    s_footer_lbl = mk_label(parent, 4, ROW_FOOTER, COL_WARN);

    lv_label_set_text(s_sat_hdr_lbl, "PRN  CN0  Freq(Hz)   PhN");
}

/* ── Formatting helpers ──────────────────────────────────────────────── */

static const char *fix_quality_name(uint8_t q)
{
    switch (q) {
        case 0: return "no-fix";
        case 1: return "GPS";
        case 2: return "DGPS";
        case 4: return "RTK-F";
        case 5: return "RTK-f";
        case 6: return "DR";
        default: return "?";
    }
}

static const char *anf_mode_name(uint8_t m)
{
    switch (m) {
        case 0: return "OFF";
        case 1: return "ON ";
        case 2: return "AUT";
        default: return "?  ";
    }
}

static void render_title(const teseo_sat_view_t *v, const teseo_state_t *st)
{
    char buf[48];
    snprintf(buf, sizeof buf,
             "GNSS debug    view:%u used:%u",
             (unsigned)v->count, (unsigned)st->num_sats);
    lv_label_set_text(s_title_lbl, buf);
}

static void render_fix(const teseo_state_t *st)
{
    /* σ taken from $GPGST if populated; else show HDOP only. */
    char buf[64];
    if (st->gst_count > 0) {
        snprintf(buf, sizeof buf,
                 "%s %s  HDOP:%u.%u  sig:%u/%u/%um",
                 st->online ? "ON" : "off",
                 fix_quality_name(st->fix_quality),
                 (unsigned)(st->hdop_x10 / 10u),
                 (unsigned)(st->hdop_x10 % 10u),
                 (unsigned)(st->gst_sigma_lat_m_x10 / 10u),
                 (unsigned)(st->gst_sigma_lon_m_x10 / 10u),
                 (unsigned)(st->gst_sigma_alt_m_x10 / 10u));
    } else {
        snprintf(buf, sizeof buf,
                 "%s %s  HDOP:%u.%u",
                 st->online ? "ON" : "off",
                 fix_quality_name(st->fix_quality),
                 (unsigned)(st->hdop_x10 / 10u),
                 (unsigned)(st->hdop_x10 % 10u));
    }
    lv_label_set_text(s_fix_lbl, buf);
}

static void render_time(const teseo_state_t *st)
{
    char buf[48];
    uint32_t t = st->utc_time;
    uint32_t d = st->utc_date;
    if (t == 0 && d == 0) {
        lv_label_set_text(s_time_lbl, "UTC:  --:--:--   date: --/--/--");
        return;
    }
    /* utc_time = hhmmss; utc_date = ddmmyy */
    snprintf(buf, sizeof buf,
             "UTC:  %02u:%02u:%02u   date: %02u/%02u/%02u",
             (unsigned)((t / 10000u) % 100u),
             (unsigned)((t / 100u)   % 100u),
             (unsigned)( t           % 100u),
             (unsigned)((d / 10000u) % 100u),
             (unsigned)((d / 100u)   % 100u),
             (unsigned)( d           % 100u));
    lv_label_set_text(s_time_lbl, buf);
}

static void render_pos(const teseo_state_t *st)
{
    char buf[64];
    if (!st->fix_valid || (st->lat_e7 == 0 && st->lon_e7 == 0)) {
        lv_label_set_text(s_pos_lbl, "pos:  -- no fix --");
        return;
    }
    /* integer ddd.ddddddd */
    long lat_int   = st->lat_e7 / 10000000;
    long lat_frac  = st->lat_e7 - lat_int * 10000000;
    if (lat_frac < 0) lat_frac = -lat_frac;
    long lon_int   = st->lon_e7 / 10000000;
    long lon_frac  = st->lon_e7 - lon_int * 10000000;
    if (lon_frac < 0) lon_frac = -lon_frac;
    snprintf(buf, sizeof buf, "pos:  %ld.%07ld, %ld.%07ld",
             lat_int, lat_frac, lon_int, lon_frac);
    lv_label_set_text(s_pos_lbl, buf);
}

static void render_motion(const teseo_state_t *st)
{
    char buf[48];
    snprintf(buf, sizeof buf,
             "alt:%+dm  spd:%u.%ukm/h  hdg:%udeg",
             (int)st->altitude_m,
             (unsigned)(st->speed_kmh_x10 / 10u),
             (unsigned)(st->speed_kmh_x10 % 10u),
             (unsigned)(st->course_deg_x10 / 10u));
    lv_label_set_text(s_motion_lbl, buf);
}

static void render_rf(const teseo_rf_state_t *r)
{
    char buf[64];
    if (r->noise_count == 0 && r->cpu_count == 0) {
        lv_label_set_text(s_rf_lbl, "RF:    -- not received --");
        return;
    }
    snprintf(buf, sizeof buf,
             "N G:%-5ld R:%-5ld  CPU:%u.%u%% @%uMHz",
             (long)r->noise_gps, (long)r->noise_gln,
             (unsigned)(r->cpu_pct_x10 / 10u),
             (unsigned)(r->cpu_pct_x10 % 10u),
             (unsigned)r->cpu_mhz);
    lv_label_set_text(s_rf_lbl, buf);
}

static void render_anf(lv_obj_t *lbl, const char *tag,
                       const teseo_anf_path_t *a, bool received)
{
    if (!received) {
        char buf[32];
        snprintf(buf, sizeof buf, "ANF %s  -- not received --", tag);
        lv_label_set_text(lbl, buf);
        return;
    }
    /* ovfs bit-12 set (value ≥ 1000) = jammer currently removed. */
    bool jammer = (a->ovfs >= 1000);
    char buf[64];
    snprintf(buf, sizeof buf,
             "ANF %s f=%7lu  %s %s %s",
             tag, (unsigned long)a->freq_hz,
             a->lock ? "LK" : "--",
             anf_mode_name(a->mode),
             jammer ? "JAM" : "   ");
    lv_label_set_text(lbl, buf);
}

static void render_counters(const teseo_state_t *st,
                            const teseo_rf_state_t *r)
{
    char buf[64];
    snprintf(buf, sizeof buf,
             "NMEA:%lu RF:%lu ANF:%lu fail:%lu",
             (unsigned long)st->sentence_count,
             (unsigned long)r->rf_update_count,
             (unsigned long)r->anf_count,
             (unsigned long)st->i2c_fail_count);
    lv_label_set_text(s_counters_lbl, buf);
}

static void render_sats(const teseo_rf_state_t *r)
{
    /* Sort a local copy by descending C/N0 — engine order shuffles. */
    teseo_rf_sat_t s[32];
    uint8_t n = r->rf_sat_count;
    if (n > 32) n = 32;
    for (int i = 0; i < n; ++i) s[i] = r->rf_sats[i];
    for (int i = 1; i < n; ++i) {
        teseo_rf_sat_t cur = s[i];
        int j = i - 1;
        while (j >= 0 && s[j].cn0_dbhz < cur.cn0_dbhz) {
            s[j + 1] = s[j];
            j--;
        }
        s[j + 1] = cur;
    }
    for (int i = 0; i < SAT_ROWS; ++i) {
        if (i < n) {
            char buf[48];
            snprintf(buf, sizeof buf,
                     "%3u  %3u  %+8ld  %5d",
                     (unsigned)s[i].prn,
                     (unsigned)s[i].cn0_dbhz,
                     (long)s[i].freq_hz,
                     (int)s[i].phase_noise);
            lv_label_set_text(s_sat_lbl[i], buf);
        } else {
            lv_label_set_text(s_sat_lbl[i], "");
        }
    }
}

void rf_debug_view_apply(const key_event_t *ev)
{
    (void)ev;
    /* No per-key bindings in the RF view for now. FUNC is swallowed by
     * the router before it reaches us. */
}

void rf_debug_view_refresh(void)
{
    const teseo_state_t    *st = teseo_get_state();
    const teseo_sat_view_t *sv = teseo_get_sat_view();
    const teseo_rf_state_t *rf = teseo_get_rf_state();

    render_title(sv, st);
    render_fix(st);
    render_time(st);
    render_pos(st);
    render_motion(st);
    render_rf(rf);
    render_anf(s_anf_gps_lbl, "GPS", &rf->anf_gps, rf->anf_count > 0);
    render_anf(s_anf_gln_lbl, "GLN", &rf->anf_gln, rf->anf_count > 0);
    render_counters(st, rf);
    render_sats(rf);

    /* Footer: commissioning hint when nothing has arrived from the RF
     * sentence set; otherwise show a brief "all streams live" marker. */
    if (rf->noise_count == 0 && rf->anf_count == 0 &&
        rf->cpu_count == 0 && rf->rf_update_count == 0) {
        lv_label_set_text(s_footer_lbl,
                          "Run teseo_enable_rf_debug_messages(true)");
    } else if (!st->online) {
        lv_label_set_text(s_footer_lbl, "Teseo offline (I2C stall)");
    } else {
        lv_label_set_text(s_footer_lbl, "");
    }
}
