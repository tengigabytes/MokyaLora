// bringup_gnss_tft.c
// Step 14 — Teseo-LIV3FL outdoor GNSS test with live TFT display
//
// Streams NMEA from Teseo-LIV3FL (I2C Bus A, 0x3A) and renders live data
// on the ST7789VI 2.4" IPS display using a built-in 5x8 bitmap font.
//
// Screen layout (240x320, scale-2: 20 cols x 20 rows, 12x16 px/cell):
//   Row 0  (Y=0):   Header bar
//   Row 1  (Y=16):  Fix status + satellite count (used/visible)
//   Row 2  (Y=32):  UTC time
//   Row 3  (Y=48):  Latitude
//   Row 4  (Y=64):  Longitude
//   Row 5  (Y=80):  Altitude + HDOP
//   Row 6  (Y=96):  Speed + TTFF
//   Row 7  (Y=112): Satellite table header
//   Rows 8-19 (Y=128..304): Up to 12 satellite rows (PRN, EL, AZ, SNR, bar)
//
// At startup sends $PSTMSETPAR to enable GSV + GLONASS + Galileo usage and
// saves with $PSTMSAVEPAR (persists across power cycles).
//
// Usage: run "gnss_tft" from the bringup REPL, or flash gnss_tft_standalone.elf.
// Press BACK key (or Enter when USB is connected) to stop.

#include "bringup.h"
#include "bringup_menu.h"
#include "tft_8080.pio.h"
#include <stdlib.h>   // atof, atoi

// ---------------------------------------------------------------------------
// TFT PIO helpers — use shared menu_tft_* functions from bringup_menu.c.
// Local g_* wrappers kept for minimal diff in GNSS rendering code.
// ---------------------------------------------------------------------------

// Thin wrappers — delegate to shared menu_tft_* functions from bringup_menu.c
// so the GNSS rendering code below needs minimal changes.
static inline bool g_tft_start(void)  { return menu_tft_start(); }
static inline void g_tft_stop(void)   { menu_tft_stop(); }
static inline void g_tft_init(void)   { menu_tft_hw_init(); }
static inline void g_flush(void)      { menu_tft_flush(); }
static inline void g_w8(uint8_t b)    { menu_tft_w8(b); }
static inline void g_cmd(uint8_t c)   { menu_tft_cmd(c); }
static inline void g_dat(uint8_t d)   { menu_tft_dat(d); }

static inline void g_win(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    menu_tft_win(x0, y0, x1, y1);
}

static inline void g_rect(int x, int y, int w, int h, uint16_t c) {
    menu_rect(x, y, w, h, c);
}

// ---------------------------------------------------------------------------
// Text rendering — thin wrappers to shared menu_char / menu_str
// ---------------------------------------------------------------------------

static inline void g_char(int x, int y, char c, uint16_t fg, uint16_t bg, int sc) {
    menu_char(x, y, c, fg, bg, sc);
}

static inline void g_str(int x, int y, const char *s, int width,
                         uint16_t fg, uint16_t bg, int sc) {
    menu_str(x, y, s, width, fg, bg, sc);
}

// ---------------------------------------------------------------------------
// NMEA accumulation buffer and sentence extraction
// ---------------------------------------------------------------------------

#define NBUF 512
static char g_nbuf[NBUF];
static int  g_nlen;

static void gnss_buf_append(const uint8_t *raw, int len) {
    for (int i = 0; i < len; i++) {
        if (raw[i] == 0xFF) continue;   // Teseo-LIV3FL idle filler
        if (g_nlen >= NBUF - 1) {
            // Overflow: discard content up to the next '$'
            char *next = strchr(g_nbuf + 1, '$');
            if (next) {
                g_nlen -= (int)(next - g_nbuf);
                memmove(g_nbuf, next, (size_t)(g_nlen + 1));
            } else {
                g_nlen = 0; g_nbuf[0] = '\0';
            }
        }
        g_nbuf[g_nlen++] = (char)raw[i];
        g_nbuf[g_nlen]   = '\0';
    }
}

// Extract the next complete NMEA sentence ($...\r\n) from g_nbuf.
// Writes content after '$' (without the '$') to out[maxlen].
// Returns true if a sentence was found and consumed.
static bool gnss_pop(char *out, int maxlen) {
    char *start = strchr(g_nbuf, '$');
    if (!start) { g_nlen = 0; g_nbuf[0] = '\0'; return false; }
    char *end = strchr(start, '\n');
    if (!end) {
        // No complete sentence yet; discard garbage before the '$'
        if (start != g_nbuf) {
            g_nlen -= (int)(start - g_nbuf);
            memmove(g_nbuf, start, (size_t)(g_nlen + 1));
        }
        return false;
    }
    *end = '\0';
    char *cr = strchr(start + 1, '\r');
    if (cr) *cr = '\0';
    snprintf(out, maxlen, "%s", start + 1);  // copy without leading '$'
    int consumed = (int)(end - g_nbuf) + 1;
    g_nlen -= consumed;
    if (g_nlen < 0) g_nlen = 0;
    memmove(g_nbuf, end + 1, (size_t)(g_nlen + 1));
    return true;
}

// ---------------------------------------------------------------------------
// NMEA field extraction and coordinate conversion
// ---------------------------------------------------------------------------

// Copy the nth comma-delimited field (0-indexed) from an NMEA sentence body.
// Stops at comma, '*', or end-of-string.
static bool nmea_fld(const char *s, int n, char *out, int max) {
    const char *p = s;
    for (int i = 0; i < n; i++) {
        p = strchr(p, ',');
        if (!p) { out[0] = '\0'; return false; }
        p++;
    }
    int i = 0;
    while (*p && *p != ',' && *p != '*' && i < max - 1) out[i++] = *p++;
    out[i] = '\0';
    return true;
}

// Convert NMEA "ddmm.mmmm" (or "dddmm.mmmm") to decimal degrees.
// Minutes always occupy the last 2 digits before the decimal point.
static double nmea_dec(const char *f) {
    if (!f || !*f) return 0.0;
    const char *dot = strchr(f, '.');
    if (!dot || (dot - f) < 2) return 0.0;
    int dlen = (int)(dot - f) - 2;  // number of degree digits
    char dbuf[8];
    if (dlen >= (int)sizeof(dbuf)) return 0.0;
    memcpy(dbuf, f, (size_t)dlen);
    dbuf[dlen] = '\0';
    return atof(dbuf) + atof(f + dlen) / 60.0;
}

// ---------------------------------------------------------------------------
// GNSS data structures
// ---------------------------------------------------------------------------

typedef struct {
    bool     gga_valid;
    bool     rmc_valid;
    int      fix_quality;  // 0=no fix, 1=GPS SPS, 2=DGPS, 4=RTK fixed, 5=RTK float
    int      num_sats;
    float    hdop;
    double   lat;          // decimal degrees, positive = N, negative = S
    char     lat_ns;       // 'N' or 'S'
    double   lon;          // decimal degrees, positive = E, negative = W
    char     lon_ew;       // 'E' or 'W'
    float    altitude;     // meters MSL
    char     utc[12];      // "HH:MM:SS\0"
    char     date[12];     // "DD/MM/YY\0"
    float    speed_kmh;
    uint32_t boot_ms;
    uint32_t fix_ms;
    bool     ttff_done;
} GnssData;

// Satellite info updated from $xGSV sentences
#define MAX_SATS 24
typedef struct {
    char     prn[4];   // e.g. "G01", "R24", "E03", "B15"
    int8_t   elev;     // elevation, degrees (0-90)
    uint16_t azim;     // azimuth, degrees (0-359)
    uint8_t  snr;      // C/N0, dBHz; 0 = no signal tracked
} SatInfo;

static GnssData g_gd;
static SatInfo  g_sats[MAX_SATS];
static int      g_sat_count;

// Parse GGA sentence body (s points to the field after "GNGGA," or "GPGGA,")
static void parse_gga(const char *s) {
    char f[24];
    // Field 0: UTC time (hhmmss.ss)
    if (nmea_fld(s, 0, f, sizeof(f)) && strlen(f) >= 6)
        snprintf(g_gd.utc, sizeof(g_gd.utc), "%.2s:%.2s:%.2s", f, f + 2, f + 4);
    // Field 1/2: latitude + N/S
    if (nmea_fld(s, 1, f, sizeof(f))) g_gd.lat = nmea_dec(f);
    if (nmea_fld(s, 2, f, sizeof(f))) {
        g_gd.lat_ns = f[0] ? f[0] : 'N';
        if (g_gd.lat_ns == 'S') g_gd.lat = -g_gd.lat;
    }
    // Field 3/4: longitude + E/W
    if (nmea_fld(s, 3, f, sizeof(f))) g_gd.lon = nmea_dec(f);
    if (nmea_fld(s, 4, f, sizeof(f))) {
        g_gd.lon_ew = f[0] ? f[0] : 'E';
        if (g_gd.lon_ew == 'W') g_gd.lon = -g_gd.lon;
    }
    // Fields 5/6/7/8: fix quality, num sats, HDOP, altitude
    if (nmea_fld(s, 5, f, sizeof(f))) g_gd.fix_quality = atoi(f);
    if (nmea_fld(s, 6, f, sizeof(f))) g_gd.num_sats    = atoi(f);
    if (nmea_fld(s, 7, f, sizeof(f))) g_gd.hdop        = (float)atof(f);
    if (nmea_fld(s, 8, f, sizeof(f))) g_gd.altitude    = (float)atof(f);
    g_gd.gga_valid = true;
    // Record TTFF on first fix
    if (g_gd.fix_quality > 0 && !g_gd.ttff_done) {
        g_gd.fix_ms   = to_ms_since_boot(get_absolute_time());
        g_gd.ttff_done = true;
    }
}

// Parse RMC sentence body (s points to the field after "GNRMC," or "GPRMC,")
static void parse_rmc(const char *s) {
    char f[24];
    // Field 1: status (A = valid, V = void)
    if (nmea_fld(s, 1, f, sizeof(f)) && f[0] == 'V') return;
    // Field 6: speed over ground (knots) → km/h
    if (nmea_fld(s, 6, f, sizeof(f))) g_gd.speed_kmh = (float)(atof(f) * 1.852);
    // Field 8: date (ddmmyy) → "DD/MM/YY"
    if (nmea_fld(s, 8, f, sizeof(f)) && strlen(f) >= 6)
        snprintf(g_gd.date, sizeof(g_gd.date), "%.2s/%.2s/%.2s", f, f + 2, f + 4);
    g_gd.rmc_valid = true;
}

// Parse GSV sentence body.
// s     = fields after the talker+sentence prefix (e.g. after "GPGSV,")
// sys   = constellation letter: 'G'=GPS, 'R'=GLONASS, 'E'=Galileo, 'B'=BeiDou
// On the first message of each constellation sweep, old entries are replaced.
static void parse_gsv(const char *s, char sys) {
    char f[8];
    if (!nmea_fld(s, 1, f, sizeof(f))) return;
    int msg_num = atoi(f);

    // First message of this constellation's cycle: evict stale entries
    if (msg_num == 1) {
        int w = 0;
        for (int i = 0; i < g_sat_count; i++) {
            if (g_sats[i].prn[0] != sys)
                g_sats[w++] = g_sats[i];
        }
        g_sat_count = w;
    }

    // Each GSV message carries up to 4 satellite entries starting at field 3
    for (int k = 0; k < 4; k++) {
        int  base = 3 + k * 4;
        char prn_s[8], el_s[8], az_s[8], snr_s[8];
        if (!nmea_fld(s, base, prn_s, sizeof(prn_s)) || !prn_s[0]) break;
        nmea_fld(s, base + 1, el_s,  sizeof(el_s));
        nmea_fld(s, base + 2, az_s,  sizeof(az_s));
        nmea_fld(s, base + 3, snr_s, sizeof(snr_s));
        if (g_sat_count >= MAX_SATS) break;
        SatInfo *sv = &g_sats[g_sat_count++];
        snprintf(sv->prn, sizeof(sv->prn), "%c%02d", sys, atoi(prn_s));
        sv->elev = (int8_t)atoi(el_s);
        sv->azim = (uint16_t)atoi(az_s);
        sv->snr  = (uint8_t)atoi(snr_s);
    }
}

// ---------------------------------------------------------------------------
// Screen layout (240x320, scale-2: 20 cols x 20 rows, 12x16 px/cell)
// ---------------------------------------------------------------------------

// RGB565 palette
#define C_BG    0x0000u   // black
#define C_WHITE 0xFFFFu   // white
#define C_YELL  0xFFE0u   // yellow
#define C_GREEN 0x07E0u   // green
#define C_RED   0xF800u   // red
#define C_CYAN  0x07FFu   // cyan
#define C_GRAY  0x8410u   // gray
#define C_DKBLU 0x000Fu   // dark blue (header background)
#define C_DKGRY 0x4208u   // dark gray

// Row Y-coordinates (16 px per row at scale 2)
#define Y_HDR      0    // Row 0: header bar
#define Y_FIX     16    // Row 1: fix status + sat count
#define Y_UTC_R   32    // Row 2: UTC time
#define Y_LAT     48    // Row 3: latitude
#define Y_LON     64    // Row 4: longitude
#define Y_ALT     80    // Row 5: altitude + HDOP
#define Y_SPD     96    // Row 6: speed + TTFF
#define Y_SAT_HDR 112   // Row 7: satellite table header
#define Y_SAT_DATA 128  // Row 8+: satellite data rows
#define MAX_SAT_ROWS 12 // Rows 8-19 (Y=128..304)

// ---------------------------------------------------------------------------
// GPS status rows rendering
// ---------------------------------------------------------------------------

static void draw_status_rows(void) {
    const GnssData *d = &g_gd;
    char buf[21];
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Row 1 (Y=16): fix status + satellites used / visible
    // Format: "%-8s SAT:xx/xx  " = 8+1+4+2+1+2+2 = 20
    const char *fstr; uint16_t fcol;
    switch (d->fix_quality) {
        case 1: fstr = "GPS SPS "; fcol = C_GREEN; break;
        case 2: fstr = "DGPS    "; fcol = C_GREEN; break;
        case 4: fstr = "RTK FIX "; fcol = C_CYAN;  break;
        case 5: fstr = "RTK FLT "; fcol = C_YELL;  break;
        default:fstr = "NO FIX  "; fcol = C_RED;   break;
    }
    snprintf(buf, sizeof(buf), "%-8s SAT:%02d/%02d  ", fstr, d->num_sats, g_sat_count);
    g_str(0, Y_FIX, buf, 20, fcol, C_BG, 2);

    // Row 2 (Y=32): UTC time — "UTC: HH:MM:SS       " = 5+15 = 20
    snprintf(buf, sizeof(buf), "UTC: %-15s", d->utc[0] ? d->utc : "--:--:--");
    g_str(0, Y_UTC_R, buf, 20, C_WHITE, C_BG, 2);

    // Rows 3-4 (Y=48,64): latitude + longitude (show dashes before first fix)
    if (d->gga_valid && d->fix_quality > 0) {
        double la = d->lat < 0.0 ? -d->lat : d->lat;
        double lo = d->lon < 0.0 ? -d->lon : d->lon;
        // "LAT:%10.6fX     " = 4+10+1+5 = 20
        snprintf(buf, sizeof(buf), "LAT:%10.6f%c     ", la, d->lat_ns);
        g_str(0, Y_LAT, buf, 20, C_WHITE, C_BG, 2);
        snprintf(buf, sizeof(buf), "LON:%10.6f%c     ", lo, d->lon_ew);
        g_str(0, Y_LON, buf, 20, C_WHITE, C_BG, 2);
    } else {
        g_str(0, Y_LAT, "LAT: ----.------    ", 20, C_GRAY, C_BG, 2);
        g_str(0, Y_LON, "LON: -----.------   ", 20, C_GRAY, C_BG, 2);
    }

    // Row 5 (Y=80): altitude + HDOP — "ALT:%7.1fm H:%4.1f " = 4+7+4+4+1 = 20
    if (d->gga_valid && d->fix_quality > 0) {
        snprintf(buf, sizeof(buf), "ALT:%7.1fm H:%4.1f ",
                 (double)d->altitude, (double)d->hdop);
        g_str(0, Y_ALT, buf, 20, C_WHITE, C_BG, 2);
    } else {
        g_str(0, Y_ALT, "ALT: -----.-m H:--- ", 20, C_GRAY, C_BG, 2);
    }

    // Row 6 (Y=96): speed + TTFF/elapsed — "SPD:%5.1f TTFF:%-5s" = 4+5+6+5 = 20
    char ttff_s[8];
    if (d->ttff_done) {
        unsigned s = (unsigned)((d->fix_ms - d->boot_ms) / 1000u);
        snprintf(ttff_s, sizeof(ttff_s), "%4us", s);
    } else {
        unsigned elapsed = (unsigned)((now - d->boot_ms) / 1000u);
        snprintf(ttff_s, sizeof(ttff_s), ">%3us", elapsed);
    }
    if (d->rmc_valid)
        snprintf(buf, sizeof(buf), "SPD:%5.1f TTFF:%-5s", (double)d->speed_kmh, ttff_s);
    else
        snprintf(buf, sizeof(buf), "SPD:---.- TTFF:%-5s", ttff_s);
    g_str(0, Y_SPD, buf, 20, d->ttff_done ? C_CYAN : C_GRAY, C_BG, 2);
}

// ---------------------------------------------------------------------------
// Satellite table rendering
// ---------------------------------------------------------------------------

// Row format: "%-4s%3d %3d %2d %-5s" = 4+3+1+3+1+2+1+5 = 20
// Header:     "SYS  EL  AZ SN SIGB "  (aligned to same column positions)
// SNR bar:    one '#' per 8 dBHz, max 5 chars (=40 dBHz full scale)
// Colour:     gray=no signal, red<25, yellow<35, green>=35

static void draw_sat_table(void) {
    // Row 7: table header, cyan on dark blue
    g_str(0, Y_SAT_HDR, "SYS  EL  AZ SN SIGB ", 20, C_CYAN, C_DKBLU, 2);

    // Sort a local copy by SNR descending (best signal at top)
    SatInfo sorted[MAX_SATS];
    int cnt = g_sat_count;
    if (cnt > MAX_SATS) cnt = MAX_SATS;
    memcpy(sorted, g_sats, (size_t)cnt * sizeof(SatInfo));
    for (int i = 0; i < cnt - 1; i++) {
        for (int j = i + 1; j < cnt; j++) {
            if (sorted[j].snr > sorted[i].snr) {
                SatInfo tmp = sorted[i];
                sorted[i]   = sorted[j];
                sorted[j]   = tmp;
            }
        }
    }

    char buf[21];
    for (int i = 0; i < MAX_SAT_ROWS; i++) {
        int y = Y_SAT_DATA + i * 16;
        if (i >= cnt) {
            g_rect(0, y, 240, 16, C_BG);
            continue;
        }
        const SatInfo *sv = &sorted[i];
        char bar[6];
        int bars = sv->snr / 8;
        if (bars > 5) bars = 5;
        for (int k = 0; k < 5; k++) bar[k] = (k < bars) ? '#' : ' ';
        bar[5] = '\0';
        snprintf(buf, sizeof(buf), "%-4s%3d %3d %2d %-5s",
                 sv->prn, (int)sv->elev, (int)sv->azim, (int)sv->snr, bar);
        uint16_t col = (sv->snr == 0) ? C_GRAY :
                       (sv->snr < 25) ? C_RED  :
                       (sv->snr < 35) ? C_YELL : C_GREEN;
        g_str(0, y, buf, 20, col, C_BG, 2);
    }
}

// ---------------------------------------------------------------------------
// BACK key (R0C1): GPIO42 = Row 0 output, GPIO37 = Col 1 input pull-up.
// Works standalone (no USB) and when USB is connected.
// Call back_key_init() once before the loop; back_key_pressed() in the loop.
// ---------------------------------------------------------------------------

#define BACK_ROW_PIN  (KEY_ROW_BASE + 0)   // GPIO42
#define BACK_COL_PIN  (KEY_COL_BASE + 1)   // GPIO37

void back_key_init(void) {
    gpio_init(BACK_ROW_PIN);
    gpio_set_dir(BACK_ROW_PIN, GPIO_OUT);
    gpio_put(BACK_ROW_PIN, 1);          // idle HIGH
    gpio_init(BACK_COL_PIN);
    gpio_set_dir(BACK_COL_PIN, GPIO_IN);
    gpio_pull_up(BACK_COL_PIN);
}

void back_key_deinit(void) {
    gpio_put(BACK_ROW_PIN, 1);
    gpio_set_dir(BACK_ROW_PIN, GPIO_IN);
    gpio_disable_pulls(BACK_ROW_PIN);
    gpio_disable_pulls(BACK_COL_PIN);
}

// Returns true if BACK key is held down.
// Brief: drive row LOW, sample col, restore row HIGH.
bool back_key_pressed(void) {
    gpio_put(BACK_ROW_PIN, 0);
    sleep_us(10);
    bool pressed = !gpio_get(BACK_COL_PIN);
    gpio_put(BACK_ROW_PIN, 1);
    return pressed;
}

// ---------------------------------------------------------------------------
// Teseo-LIV3FL configuration helper
// Sends a complete $PSTM... sentence over I2C and prints the response.
// Call after Bus A (i2c1, 0x3A) has been initialised.
// ---------------------------------------------------------------------------

static void gnss_send_pstm(const char *cmd) {
    printf("GNSS cfg: %s", cmd);  // cmd already includes \r\n
    int r = i2c_write_timeout_us(i2c1, 0x3A,
                (const uint8_t *)cmd, strlen(cmd), false, 200000);
    if (r < 0) { printf("  (i2c write err %d)\n", r); return; }
    sleep_ms(200);
    uint8_t resp[128];
    r = i2c_read_timeout_us(i2c1, 0x3A, resp, sizeof(resp) - 1, false, 200000);
    if (r > 0) {
        // Strip 0xFF idle bytes and print any real response
        int w = 0;
        for (int i = 0; i < r; i++)
            if (resp[i] != 0xFF) resp[w++] = resp[i];
        if (w > 0) { resp[w] = '\0'; printf("  -> %s\n", (char *)resp); }
    }
}

// ---------------------------------------------------------------------------
// gnss_tft_test — public entry point (Step 14)
// ---------------------------------------------------------------------------

void gnss_tft_test(void) {
    printf("\n--- GNSS TFT Live Display (Step 14) ---\n");
    printf("Streaming NMEA from Teseo-LIV3FL; rendering on ST7789VI.\n");
    printf("Press BACK key (or Enter) to stop.\n");

    // --- TFT GPIO init ---
    gpio_init(TFT_nCS_PIN);  gpio_set_dir(TFT_nCS_PIN,  GPIO_OUT); gpio_put(TFT_nCS_PIN,  1);
    gpio_init(TFT_DCX_PIN);  gpio_set_dir(TFT_DCX_PIN,  GPIO_OUT); gpio_put(TFT_DCX_PIN,  1);
    gpio_init(TFT_nRST_PIN); gpio_set_dir(TFT_nRST_PIN, GPIO_OUT); gpio_put(TFT_nRST_PIN, 1);

    if (!g_tft_start()) return;

    gpio_put(TFT_nRST_PIN, 0); sleep_ms(10);
    gpio_put(TFT_nRST_PIN, 1); sleep_ms(120);
    gpio_put(TFT_nCS_PIN, 0);
    g_tft_init();

    // Backlight on via Bus B — must happen before Bus A is claimed for GNSS
    bus_b_init();
    lm_write(LM27965_BANKA, 0x16);  // TFT backlight 40%
    lm_write(LM27965_GP,    0x21);  // ENA=1
    bus_b_deinit();

    // BACK key — exit trigger when running standalone (no USB)
    back_key_init();

    // --- Draw initial screen ---
    g_rect(0, 0, 240, 320, C_BG);
    g_str(0, Y_HDR, "  MOKYA  GPS  LIVE  ", 20, C_YELL, C_DKBLU, 2);

    // --- Init GNSS data state ---
    memset(&g_gd, 0, sizeof(g_gd));
    g_gd.lat_ns  = 'N';
    g_gd.lon_ew  = 'E';
    g_gd.boot_ms = to_ms_since_boot(get_absolute_time());
    g_sat_count  = 0;
    g_nlen = 0; g_nbuf[0] = '\0';

    draw_status_rows();
    draw_sat_table();

    // --- Init Bus A for GNSS (i2c1, GPIO 34/35) ---
    i2c_init(i2c1, 100 * 1000);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_A_SDA);
    gpio_pull_up(BUS_A_SCL);

    // Configure Teseo CDB via $PSTMSETPAR (mode=1: OR, non-destructive):
    //   ID 200 |= 0x2A0000: NMEA_GNGSV_ENABLE | GLONASS_ENABLE | GLONASS_USE_ENABLE
    //   ID 227 |= 0x80:     GALILEO_USAGE_ENABLE
    // Then save to NVM with $PSTMSAVEPAR.
    gnss_send_pstm("$PSTMSETPAR,1200,2A0000,1*76\r\n");
    gnss_send_pstm("$PSTMSETPAR,1227,80,1*08\r\n");
    gnss_send_pstm("$PSTMSAVEPAR*58\r\n");
    sleep_ms(500);  // wait for NVM write to complete

    // Drain any stale bytes from the serial input buffer
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}

    uint8_t  raw[128];
    char     sent[128];
    uint32_t last_draw = 0;
    bool     dirty     = false;

    // --- Main loop ---
    while (true) {
        // Exit: Enter via USB serial or BACK key on keypad
        int ch = getchar_timeout_us(0);
        if (ch == '\r' || ch == '\n') break;
        if (back_key_pressed()) { sleep_ms(50); break; }

        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Poll GNSS over I2C; 0xFF bytes = idle filler from Teseo-LIV3FL
        int r = i2c_read_timeout_us(i2c1, 0x3A, raw, sizeof(raw), false, 200000);
        if (r > 0) {
            gnss_buf_append(raw, r);
            while (gnss_pop(sent, sizeof(sent))) {
                printf("$%s\n", sent);
                if      (strncmp(sent, "GNGGA,", 6) == 0 ||
                         strncmp(sent, "GPGGA,", 6) == 0) { parse_gga(sent + 6); dirty = true; }
                else if (strncmp(sent, "GNRMC,", 6) == 0 ||
                         strncmp(sent, "GPRMC,", 6) == 0) { parse_rmc(sent + 6); dirty = true; }
                else if (strncmp(sent, "GPGSV,", 6) == 0) { parse_gsv(sent + 6, 'G'); dirty = true; }
                else if (strncmp(sent, "GLGSV,", 6) == 0) { parse_gsv(sent + 6, 'R'); dirty = true; }
                else if (strncmp(sent, "GAGSV,", 6) == 0) { parse_gsv(sent + 6, 'E'); dirty = true; }
                else if (strncmp(sent, "GBGSV,", 6) == 0) { parse_gsv(sent + 6, 'B'); dirty = true; }
            }
        }

        // Redraw on data change (250 ms throttle) or every second for TTFF counter
        bool ttff_tick = !g_gd.ttff_done && (now - last_draw) >= 1000;
        if ((dirty && (now - last_draw) >= 250) || ttff_tick) {
            draw_status_rows();
            draw_sat_table();
            last_draw = now;
            dirty = false;
        }

        sleep_ms(20);  // ~50 Hz poll rate
    }

    // --- Cleanup ---
    back_key_deinit();

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);

    bus_b_init();
    lm_write(LM27965_GP, 0x20);  // backlight off
    bus_b_deinit();

    gpio_put(TFT_nCS_PIN, 1);
    g_tft_stop();
    gpio_set_function(TFT_nCS_PIN,  GPIO_FUNC_NULL);
    gpio_set_function(TFT_DCX_PIN,  GPIO_FUNC_NULL);
    gpio_set_function(TFT_nRST_PIN, GPIO_FUNC_NULL);

    printf("Done\n");
}
