/* rf_debug_view.c — see rf_debug_view.h. */

#include "rf_debug_view.h"

#include <stdio.h>
#include <string.h>

#include "teseo_liv3fl.h"

/* ── Layout (landscape 320x240) ──────────────────────────────────────── *
 *
 *   y   0 ..  17  Title bar (Montserrat 14)
 *   y  18 ..  31  Fix status
 *   y  32 ..  45  Noise floor
 *   y  46 ..  59  CPU
 *   y  60 ..  73  ANF GPS
 *   y  74 ..  87  ANF GLN
 *   y  88 ..  89  separator (drawn as a thin label underline)
 *   y  90 .. 103  Sat table header
 *   y 104 .. 239  Sat rows (up to 8 rows × 14 px = 112 px used)
 */

#define COL_FG          lv_color_hex(0xE0E0E0)
#define COL_BG          lv_color_hex(0x000000)
#define COL_TITLE       lv_color_hex(0x00E0E0)
#define COL_WARN        lv_color_hex(0xFFB000)
#define COL_OK          lv_color_hex(0x00D060)
#define COL_DIM         lv_color_hex(0x808080)

#define ROW_TITLE        0
#define ROW_FIX         18
#define ROW_NOISE       32
#define ROW_CPU         46
#define ROW_ANF_GPS     60
#define ROW_ANF_GLN     74
#define ROW_SAT_HDR     94
#define ROW_SAT_FIRST  110
#define ROW_SAT_STEP    14
#define SAT_ROWS        9       /* 110 + 9*14 = 236 ≤ 239 */

/* Running counter copies so we can detect "no data flowing" state. */
static uint32_t s_seen_rf_count;
static uint32_t s_seen_anf_count;
static uint32_t s_seen_noise_count;

/* Widget handles. */
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_fix_lbl;
static lv_obj_t *s_noise_lbl;
static lv_obj_t *s_cpu_lbl;
static lv_obj_t *s_anf_gps_lbl;
static lv_obj_t *s_anf_gln_lbl;
static lv_obj_t *s_sat_hdr_lbl;
static lv_obj_t *s_sat_lbl[SAT_ROWS];
static lv_obj_t *s_hint_lbl;

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

    s_title_lbl   = mk_label(parent,   4, ROW_TITLE,   COL_TITLE);
    s_fix_lbl     = mk_label(parent,   4, ROW_FIX,     COL_FG);
    s_noise_lbl   = mk_label(parent,   4, ROW_NOISE,   COL_FG);
    s_cpu_lbl     = mk_label(parent,   4, ROW_CPU,     COL_FG);
    s_anf_gps_lbl = mk_label(parent,   4, ROW_ANF_GPS, COL_FG);
    s_anf_gln_lbl = mk_label(parent,   4, ROW_ANF_GLN, COL_FG);
    s_sat_hdr_lbl = mk_label(parent,   4, ROW_SAT_HDR, COL_DIM);
    for (int i = 0; i < SAT_ROWS; ++i) {
        s_sat_lbl[i] = mk_label(parent, 4, ROW_SAT_FIRST + i * ROW_SAT_STEP,
                                COL_FG);
        lv_label_set_text(s_sat_lbl[i], "");
    }
    /* Bottom-right hint for commissioning — hidden once data flows. */
    s_hint_lbl = mk_label(parent, 4, 222, COL_WARN);

    lv_label_set_text(s_title_lbl, "GNSS RF debug");
    lv_label_set_text(s_sat_hdr_lbl, "PRN  CN0  Freq (Hz)  PhN");
}

/* Format helpers — keep small, avoid float-heavy printf paths. */

static const char *fix_quality_name(uint8_t q)
{
    switch (q) {
        case 0: return "no-fix";
        case 1: return "GPS";
        case 2: return "DGPS";
        case 4: return "RTK-fix";
        case 5: return "RTK-flt";
        case 6: return "DR";
        default: return "?";
    }
}

static void render_fix(const teseo_state_t *st)
{
    char buf[64];
    snprintf(buf, sizeof buf,
             "%s   fix:%s  sats:%u  HDOP:%u.%u",
             st->online ? "ONLINE" : "offline",
             fix_quality_name(st->fix_quality),
             (unsigned)st->num_sats,
             (unsigned)(st->hdop_x10 / 10u),
             (unsigned)(st->hdop_x10 % 10u));
    lv_label_set_text(s_fix_lbl, buf);
}

static void render_noise(const teseo_rf_state_t *r)
{
    char buf[48];
    snprintf(buf, sizeof buf,
             "Noise  GPS:%-5ld  GLN:%-5ld",
             (long)r->noise_gps, (long)r->noise_gln);
    lv_label_set_text(s_noise_lbl, buf);
}

static void render_cpu(const teseo_rf_state_t *r)
{
    char buf[48];
    snprintf(buf, sizeof buf,
             "CPU   %u.%u%%  @ %u MHz",
             (unsigned)(r->cpu_pct_x10 / 10u),
             (unsigned)(r->cpu_pct_x10 % 10u),
             (unsigned)r->cpu_mhz);
    lv_label_set_text(s_cpu_lbl, buf);
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

static void render_anf(lv_obj_t *lbl, const char *tag,
                       const teseo_anf_path_t *a)
{
    /* ovfs bit 12 (value 1000 in decimal per datasheet: ovfs text form
     * "MRRR" where M = 1 means notch is removing a jammer). */
    bool jammer = (a->ovfs >= 1000);
    char buf[64];
    snprintf(buf, sizeof buf,
             "ANF %s  f=%7lu  %s %s %s",
             tag,
             (unsigned long)a->freq_hz,
             a->lock ? "LK" : "--",
             anf_mode_name(a->mode),
             jammer ? "JAM" : "   ");
    lv_label_set_text(lbl, buf);
}

static void render_sats(const teseo_rf_state_t *r)
{
    /* Show top-N by C/N0 — rf_sats already arrives in engine order which
     * varies each cycle. Sort by descending cn0_dbhz in a local copy. */
    teseo_rf_sat_t s[32];
    uint8_t n = r->rf_sat_count;
    if (n > 32) n = 32;
    for (int i = 0; i < n; ++i) s[i] = r->rf_sats[i];

    /* Insertion sort — n ≤ 32. */
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
                     "%3u  %3u   %+8ld  %5d",
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

void rf_debug_view_tick(void)
{
    const teseo_state_t    *st = teseo_get_state();
    const teseo_rf_state_t *rf = teseo_get_rf_state();

    render_fix(st);

    /* Only draw metrics that have been seen at least once — otherwise
     * show "— not received —" rather than a misleading zero reading. */
    if (rf->noise_count > 0) {
        render_noise(rf);
    } else {
        lv_label_set_text(s_noise_lbl, "Noise  — not received —");
    }
    if (rf->cpu_count > 0) {
        render_cpu(rf);
    } else {
        lv_label_set_text(s_cpu_lbl, "CPU    — not received —");
    }
    if (rf->anf_count > 0) {
        render_anf(s_anf_gps_lbl, "GPS", &rf->anf_gps);
        render_anf(s_anf_gln_lbl, "GLN", &rf->anf_gln);
    } else {
        lv_label_set_text(s_anf_gps_lbl, "ANF GPS — not received —");
        lv_label_set_text(s_anf_gln_lbl, "ANF GLN — not received —");
    }
    render_sats(rf);

    /* Commissioning hint: if no $PSTM* ever arrived, tell the user to
     * run teseo_enable_rf_debug_messages(). */
    if (rf->noise_count == 0 && rf->anf_count == 0 &&
        rf->cpu_count == 0 && rf->rf_update_count == 0) {
        lv_label_set_text(s_hint_lbl,
                          "Run teseo_enable_rf_debug_messages(true)");
    } else {
        lv_label_set_text(s_hint_lbl, "");
    }

    s_seen_rf_count    = rf->rf_update_count;
    s_seen_anf_count   = rf->anf_count;
    s_seen_noise_count = rf->noise_count;
}
