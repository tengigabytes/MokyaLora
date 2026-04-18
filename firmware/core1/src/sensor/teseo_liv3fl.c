/* teseo_liv3fl.c — Teseo-LIV3FL driver. See teseo_liv3fl.h for summary.
 *
 * NMEA references: UM2229 Rev 5 §2.2.2 (CDB 303 = GNSS FIX Rate, double,
 * period in seconds) and §10.2.16 / §10.2.17 ($PSTMGPSSUSPEND /
 * $PSTMGPSRESTART).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "teseo_liv3fl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include "i2c_bus.h"

/* ── Constants ──────────────────────────────────────────────────────────── */

#define TESEO_ADDR            0x3Au

/* Teseo I2C "no-data" fill byte. Per UM2229 §2.3: when there is nothing
 * queued the receiver streams 0xFF as pad. */
#define NMEA_FILL_BYTE        0xFFu

#define I2C_TIMEOUT_US        200000u
#define DRAIN_BUF_SIZE        1024u        /* one transaction per 100 ms tick.
                                            * Sized for 10 Hz × GGA+RMC+GSV+GSA
                                            * (~6 KB/s) with slack. Teseo pads
                                            * unused bytes with 0xFF fill, so
                                            * over-reading is free. 400 kHz I2C
                                            * reads 1024 B in ~25 ms. */
#define LINE_BUF_SIZE         96u          /* NMEA max 82 + CR/LF + slack  */
#define FIELD_BUF_SIZE        16u
#define CMD_BUF_SIZE          64u

/* After this many consecutive drain failures, declare offline. ~5 s at
 * 100 ms tick — long enough to ride out a transient bus stall while we
 * time-mux with LPS/LIS/IMU. */
#define FAIL_DISCONNECT_COUNT 50u

/* Conversion knots → km/h × 10. 1 knot = 1.852 km/h → × 10 = 18.52. */
#define KNOTS_TO_KMH_X10      18.52

/* ── Driver state ──────────────────────────────────────────────────────── */

static teseo_state_t    s_state;
static teseo_sat_view_t s_sat_view;
static teseo_rf_state_t s_rf_state;
static gnss_rate_t      s_fix_rate = GNSS_RATE_1HZ;   /* CDB 303 default */

/* $PSTMRF accumulator — refreshed on MessgIndex=1, published on
 * MessgIndex == MessgAmount. */
static uint8_t         s_rf_expected_total;
static uint8_t         s_rf_next_idx;
static uint8_t         s_rf_tmp_count;
static teseo_rf_sat_t  s_rf_tmp[32];

/* NMEA line accumulator */
static char     s_line[LINE_BUF_SIZE];
static uint16_t s_line_len;

/* Per-talker GSV accumulator — 4 slots cover GP / GL / GA / BD. */
typedef struct {
    uint16_t talker;              /* packed 2-char (0 = slot unused)     */
    uint8_t  expected_total;
    uint8_t  next_idx;
    uint8_t  sat_count;
    teseo_sat_info_t sats[16];
} gsv_accum_t;
static gsv_accum_t s_gsv[4];

/* Drain buffer — static to keep task stack small. */
static uint8_t s_drain_buf[DRAIN_BUF_SIZE];

/* ── NMEA checksum ──────────────────────────────────────────────────────── */

static uint8_t nmea_xor(const char *body, size_t len)
{
    uint8_t c = 0;
    for (size_t i = 0; i < len; ++i) c ^= (uint8_t)body[i];
    return c;
}

/* Verify a complete line starting with '$' and ending before CR/LF.
 * s_line_len points past the last body byte (no CR/LF present here). */
static bool nmea_verify(const char *line, size_t len)
{
    if (len < 4 || line[0] != '$') return false;
    const char *star = (const char *)memchr(line, '*', len);
    if (!star) return false;
    size_t tail = (size_t)(line + len - star);   /* '*' + 2 hex chars */
    if (tail < 3) return false;
    size_t body_len = (size_t)(star - line) - 1; /* skip leading '$' */
    uint8_t computed = nmea_xor(line + 1, body_len);
    char cs_str[3] = { star[1], star[2], 0 };
    uint8_t expect = (uint8_t)strtoul(cs_str, NULL, 16);
    return computed == expect;
}

/* Copy the n-th comma-delimited field (0-indexed from just-after the
 * leading '$') of a verified NMEA body into `out`. Returns false if the
 * field index is past end-of-line. `out` is always null-terminated. */
static bool nmea_field(const char *body, int n, char *out, size_t out_sz)
{
    if (out_sz == 0) return false;
    out[0] = 0;
    int curr = 0;
    const char *p = body;
    while (*p && *p != '*' && curr < n) {
        if (*p == ',') curr++;
        p++;
    }
    if (curr < n) return false;
    size_t i = 0;
    while (*p && *p != ',' && *p != '*' && i + 1 < out_sz) {
        out[i++] = *p++;
    }
    out[i] = 0;
    return true;
}

/* Convert "ddmm.mmmm" / "dddmm.mmmm" + hemisphere to degrees × 1e7. */
static int32_t nmea_coord_e7(const char *f, char hem)
{
    if (!f || !f[0]) return 0;
    const char *dot = strchr(f, '.');
    if (!dot) return 0;
    int deg_digits = (int)(dot - f) - 2;          /* minutes = 2 digits */
    if (deg_digits < 1 || deg_digits > 3) return 0;

    char deg_str[4] = { 0 };
    memcpy(deg_str, f, (size_t)deg_digits);
    int32_t deg = atoi(deg_str);

    double min = atof(f + deg_digits);
    double total = (double)deg + min / 60.0;
    if (hem == 'S' || hem == 'W') total = -total;
    return (int32_t)(total * 10000000.0);
}

/* ── Sentence parsers ──────────────────────────────────────────────────── */

static void parse_gga(const char *body)
{
    char f[FIELD_BUF_SIZE];
    char latf[FIELD_BUF_SIZE], lonf[FIELD_BUF_SIZE];
    char ns = 'N', ew = 'E';

    if (nmea_field(body, 1, f, sizeof f) && f[0]) {
        s_state.utc_time = (uint32_t)atoi(f);
    }
    bool have_lat = nmea_field(body, 2, latf, sizeof latf) && latf[0];
    if (nmea_field(body, 3, f, sizeof f) && f[0]) ns = f[0];
    bool have_lon = nmea_field(body, 4, lonf, sizeof lonf) && lonf[0];
    if (nmea_field(body, 5, f, sizeof f) && f[0]) ew = f[0];

    if (nmea_field(body, 6, f, sizeof f))
        s_state.fix_quality = (uint8_t)atoi(f);
    if (nmea_field(body, 7, f, sizeof f))
        s_state.num_sats = (uint8_t)atoi(f);
    if (nmea_field(body, 8, f, sizeof f))
        s_state.hdop_x10 = (uint16_t)(atof(f) * 10.0);
    if (nmea_field(body, 9, f, sizeof f))
        s_state.altitude_m = (int16_t)atoi(f);

    /* Only publish coordinates when the fix is usable, to avoid zeroing
     * the last-known position on every dropped sentence. */
    if (s_state.fix_quality > 0 && have_lat && have_lon) {
        s_state.lat_e7 = nmea_coord_e7(latf, ns);
        s_state.lon_e7 = nmea_coord_e7(lonf, ew);
    }
    s_state.sentence_count++;
}

/* $--GST,<utc>,<rms>,<smajor>,<sminor>,<orient>,<sigma_lat>,<sigma_lon>,<sigma_alt>
 * Standard NMEA 0183 — std-dev of pseudorange residuals. We only capture
 * the three orthogonal sigmas. Enabled by the RF-debug CDB 231 mask
 * (bit 3 = $GPGST). */
static void parse_gst(const char *body)
{
    char f[FIELD_BUF_SIZE];
    if (nmea_field(body, 6, f, sizeof f) && f[0])
        s_state.gst_sigma_lat_m_x10 = (uint16_t)(atof(f) * 10.0);
    if (nmea_field(body, 7, f, sizeof f) && f[0])
        s_state.gst_sigma_lon_m_x10 = (uint16_t)(atof(f) * 10.0);
    if (nmea_field(body, 8, f, sizeof f) && f[0])
        s_state.gst_sigma_alt_m_x10 = (uint16_t)(atof(f) * 10.0);
    s_state.gst_count++;
}

static void parse_rmc(const char *body)
{
    char f[FIELD_BUF_SIZE];
    if (nmea_field(body, 2, f, sizeof f))
        s_state.fix_valid = (f[0] == 'A');
    if (nmea_field(body, 7, f, sizeof f) && f[0])
        s_state.speed_kmh_x10 = (int16_t)(atof(f) * KNOTS_TO_KMH_X10);
    if (nmea_field(body, 8, f, sizeof f) && f[0])
        s_state.course_deg_x10 = (int16_t)(atof(f) * 10.0);
    if (nmea_field(body, 9, f, sizeof f) && f[0])
        s_state.utc_date = (uint32_t)atoi(f);
    s_state.sentence_count++;
}

static gsv_accum_t *gsv_accum_for(uint16_t talker)
{
    for (int i = 0; i < 4; ++i)
        if (s_gsv[i].talker == talker) return &s_gsv[i];
    for (int i = 0; i < 4; ++i)
        if (s_gsv[i].talker == 0) {
            s_gsv[i].talker = talker;
            return &s_gsv[i];
        }
    return NULL;   /* more than 4 talkers — shouldn't happen */
}

static void publish_sat_view(void)
{
    uint8_t count = 0;
    for (int i = 0; i < 4; ++i) {
        gsv_accum_t *acc = &s_gsv[i];
        for (int j = 0; j < acc->sat_count && count < 32; ++j)
            s_sat_view.sats[count++] = acc->sats[j];
    }
    s_sat_view.count = count;
    s_sat_view.update_count++;
}

static void parse_gsv(const char *type, const char *body)
{
    /* Talker ID = first two characters of the 5-char sentence type. */
    uint16_t talker = ((uint16_t)(uint8_t)type[0] << 8) | (uint8_t)type[1];
    gsv_accum_t *acc = gsv_accum_for(talker);
    if (!acc) return;

    char f[FIELD_BUF_SIZE];
    if (!nmea_field(body, 1, f, sizeof f)) return;
    uint8_t total = (uint8_t)atoi(f);
    if (!nmea_field(body, 2, f, sizeof f)) return;
    uint8_t idx = (uint8_t)atoi(f);
    /* Field 3 (satellites in view) intentionally ignored — we count. */

    if (idx == 1) {
        acc->expected_total = total;
        acc->next_idx       = 1;
        acc->sat_count      = 0;
    }
    if (idx != acc->next_idx) {
        /* Out-of-order sentence — drop the whole cycle for this talker. */
        acc->next_idx = 0;
        return;
    }

    for (int s = 0; s < 4 && acc->sat_count < 16; ++s) {
        int base = 4 + s * 4;
        char prn_s[8], el_s[8], az_s[8], snr_s[8];
        if (!nmea_field(body, base, prn_s, sizeof prn_s) || !prn_s[0]) break;
        nmea_field(body, base + 1, el_s,  sizeof el_s);
        nmea_field(body, base + 2, az_s,  sizeof az_s);
        nmea_field(body, base + 3, snr_s, sizeof snr_s);
        teseo_sat_info_t *si = &acc->sats[acc->sat_count++];
        si->prn           = (uint8_t)atoi(prn_s);
        si->elevation_deg = (uint8_t)atoi(el_s);
        si->azimuth_deg   = (uint16_t)atoi(az_s);
        si->snr_dbhz      = snr_s[0] ? (uint8_t)atoi(snr_s) : 0;
    }
    acc->next_idx = idx + 1;

    if (idx == total) {
        publish_sat_view();
        acc->next_idx = 0;
    }
}

/* ── ST proprietary command-reply tracking ───────────────────────────── *
 *
 * Teseo command dialogue is synchronous: host sends a $PSTM... line,
 * Teseo replies with $PSTMxxxOK / $PSTMxxxERROR (or a $PSTMSETPAR
 * value-reply in the case of $PSTMGETPAR). We dispatch these through
 * the normal NMEA accumulator and publish the outcome via volatiles
 * so that send_await() below can block on them from the caller's
 * context without a custom parser state machine.                     */

typedef enum {
    TESEO_RESP_NONE,
    TESEO_RESP_SETPAR_OK,
    TESEO_RESP_SETPAR_ERR,
    TESEO_RESP_SAVEPAR_OK,
    TESEO_RESP_SAVEPAR_ERR,
} teseo_resp_t;

static volatile teseo_resp_t s_last_resp;
static volatile uint32_t     s_last_resp_count;

/* GETPAR reply capture. Teseo replies to $PSTMGETPAR with a synthetic
 * $PSTMSETPAR line carrying the current value, distinguishable from the
 * SETPAR command echo by the absence of a trailing "mode" field. */
static char              s_last_getpar_value[32];
static volatile bool     s_last_getpar_valid;

/* ── RF / diagnostic sentence parsers ─────────────────────────────────── */

/* $PSTMNOISE,<GPS_raw_NF>,<GLONASS_raw_NF>*<cs>     (§11.5.45) */
static void parse_pstm_noise(const char *body)
{
    char f[FIELD_BUF_SIZE];
    if (nmea_field(body, 1, f, sizeof f)) s_rf_state.noise_gps = atoi(f);
    if (nmea_field(body, 2, f, sizeof f)) s_rf_state.noise_gln = atoi(f);
    s_rf_state.noise_count++;
}

/* $PSTMCPU,<usage>,-1,<speed>*<cs>                  (§11.5.46)
 * usage is "ddd.dd" (percent), speed is MHz as an integer. */
static void parse_pstm_cpu(const char *body)
{
    char f[FIELD_BUF_SIZE];
    if (nmea_field(body, 1, f, sizeof f))
        s_rf_state.cpu_pct_x10 = (uint16_t)(atof(f) * 10.0);
    if (nmea_field(body, 3, f, sizeof f))
        s_rf_state.cpu_mhz = (uint16_t)atoi(f);
    s_rf_state.cpu_count++;
}

/* $PSTMNOTCHSTATUS,<freq_gps>,<lock_gps>,<pwr_gps>,<ovfs_gps>,<mode_gps>,
 *                  <freq_gln>,<lock_gln>,<pwr_gln>,<ovfs_gln>,<mode_gln>
 * (§11.5.58) */
static void parse_pstm_notchstatus(const char *body)
{
    char f[FIELD_BUF_SIZE];
    if (nmea_field(body, 1, f, sizeof f)) s_rf_state.anf_gps.freq_hz = (uint32_t)atoi(f);
    if (nmea_field(body, 2, f, sizeof f)) s_rf_state.anf_gps.lock    = (uint8_t)atoi(f);
    if (nmea_field(body, 3, f, sizeof f)) s_rf_state.anf_gps.power   = (uint32_t)atoi(f);
    if (nmea_field(body, 4, f, sizeof f)) s_rf_state.anf_gps.ovfs    = (uint16_t)atoi(f);
    if (nmea_field(body, 5, f, sizeof f)) s_rf_state.anf_gps.mode    = (uint8_t)atoi(f);
    if (nmea_field(body, 6, f, sizeof f)) s_rf_state.anf_gln.freq_hz = (uint32_t)atoi(f);
    if (nmea_field(body, 7, f, sizeof f)) s_rf_state.anf_gln.lock    = (uint8_t)atoi(f);
    if (nmea_field(body, 8, f, sizeof f)) s_rf_state.anf_gln.power   = (uint32_t)atoi(f);
    if (nmea_field(body, 9, f, sizeof f)) s_rf_state.anf_gln.ovfs    = (uint16_t)atoi(f);
    if (nmea_field(body, 10, f, sizeof f)) s_rf_state.anf_gln.mode   = (uint8_t)atoi(f);
    s_rf_state.anf_count++;
}

/* $PSTMRF,<MessgAmount>,<MessgIndex>,<used_sats>,[<SatID>,<PhN>,<Freq>,<CN0>]× ≤ 3
 * (§11.5.35). Published into rf_sats[] on the final sentence of the group. */
static void parse_pstm_rf(const char *body)
{
    char f[FIELD_BUF_SIZE];
    if (!nmea_field(body, 1, f, sizeof f)) return;
    uint8_t total = (uint8_t)atoi(f);
    if (!nmea_field(body, 2, f, sizeof f)) return;
    uint8_t idx = (uint8_t)atoi(f);

    if (idx == 1) {
        s_rf_expected_total = total;
        s_rf_next_idx       = 1;
        s_rf_tmp_count      = 0;
    }
    if (idx != s_rf_next_idx) {
        s_rf_next_idx = 0;
        return;
    }

    /* Fields 4..15 repeat 4 per satellite (ID, PhN, Freq, CN0). */
    for (int sat = 0; sat < 3 && s_rf_tmp_count < 32; ++sat) {
        int base = 4 + sat * 4;
        char id_s[8], phn_s[8], frq_s[12], cn_s[8];
        if (!nmea_field(body, base, id_s, sizeof id_s) || !id_s[0]) break;
        nmea_field(body, base + 1, phn_s, sizeof phn_s);
        nmea_field(body, base + 2, frq_s, sizeof frq_s);
        nmea_field(body, base + 3, cn_s,  sizeof cn_s);
        teseo_rf_sat_t *s = &s_rf_tmp[s_rf_tmp_count++];
        s->prn         = (uint8_t)atoi(id_s);
        s->phase_noise = (int16_t)atoi(phn_s);
        s->freq_hz     = (int32_t)atoi(frq_s);
        s->cn0_dbhz    = cn_s[0] ? (uint8_t)atoi(cn_s) : 0;
    }
    s_rf_next_idx = idx + 1;

    if (idx == total) {
        s_rf_state.rf_sat_count = s_rf_tmp_count;
        for (int i = 0; i < s_rf_tmp_count; ++i)
            s_rf_state.rf_sats[i] = s_rf_tmp[i];
        s_rf_state.rf_update_count++;
        s_rf_next_idx = 0;
    }
}

static void dispatch_pstm(const char *body)
{
    /* Matches run in decreasing length order so shorter prefixes don't
     * shadow longer ones (e.g. "PSTMSETPARERROR" before "PSTMSETPAR"). */
    if (memcmp(body, "PSTMSAVEPARERROR", 16) == 0) {
        s_last_resp = TESEO_RESP_SAVEPAR_ERR;
        s_last_resp_count++;
    } else if (memcmp(body, "PSTMSETPARERROR", 15) == 0) {
        s_last_resp = TESEO_RESP_SETPAR_ERR;
        s_last_resp_count++;
    } else if (memcmp(body, "PSTMSAVEPAROK", 13) == 0) {
        s_last_resp = TESEO_RESP_SAVEPAR_OK;
        s_last_resp_count++;
    } else if (memcmp(body, "PSTMSETPAROK", 12) == 0) {
        s_last_resp = TESEO_RESP_SETPAR_OK;
        s_last_resp_count++;
    } else if (memcmp(body, "PSTMSETPAR,", 11) == 0) {
        /* GETPAR value-reply vs our own SETPAR command echo:
         *   GETPAR reply: $PSTMSETPAR,<CB><ID>,<value>*<cs>   (2 fields)
         *   Our echo    : $PSTMSETPAR,<CB><ID>,<value>[,<mode>]*<cs>
         *                 — we always send without mode now, but Teseo
         *                 still echoes what arrived. Distinguish by
         *                 field count after the command name. */
        char f_val[32], f_mode[4];
        bool have_val  = nmea_field(body, 2, f_val,  sizeof f_val)  && f_val[0];
        bool have_mode = nmea_field(body, 3, f_mode, sizeof f_mode) && f_mode[0];
        if (have_val && !have_mode) {
            size_t n = strlen(f_val);
            if (n >= sizeof s_last_getpar_value) n = sizeof s_last_getpar_value - 1;
            for (size_t i = 0; i < n; ++i) s_last_getpar_value[i] = f_val[i];
            s_last_getpar_value[n] = 0;
            s_last_getpar_valid = true;
        }
    }
    else if (memcmp(body, "PSTMNOTCHSTATUS", 15) == 0) {
        parse_pstm_notchstatus(body);
    } else if (memcmp(body, "PSTMNOISE", 9) == 0) {
        parse_pstm_noise(body);
    } else if (memcmp(body, "PSTMCPU", 7) == 0) {
        parse_pstm_cpu(body);
    } else if (memcmp(body, "PSTMRF,", 7) == 0) {
        parse_pstm_rf(body);
    }
    /* Other $PSTM... (PSTMTG, PSTMPA, GPSSUSPENDED, etc.) are silently
     * dropped — no consumers yet. */
}

static void dispatch_line(void)
{
    if (s_line_len < 8) return;
    if (s_line[0] != '$') return;
    if (!nmea_verify(s_line, s_line_len)) return;

    const char *body = s_line + 1;   /* skip leading '$' */

    /* ST proprietary first (no talker prefix). */
    if (memcmp(body, "PSTM", 4) == 0) {
        dispatch_pstm(body);
        return;
    }

    /* Standard NMEA: body layout is [talker:2][type:3],fields...
     * (e.g. "GNGGA,..."). Need at least 5 chars of header. */
    if (s_line_len < 7) return;
    const char *type = body;

    if (memcmp(type + 2, "GGA", 3) == 0)      parse_gga(body);
    else if (memcmp(type + 2, "RMC", 3) == 0) parse_rmc(body);
    else if (memcmp(type + 2, "GSV", 3) == 0) parse_gsv(type, body);
    else if (memcmp(type + 2, "GST", 3) == 0) parse_gst(body);
    /* GSA / VTG / others: ignored by design — see §108 of software
     * requirements. */
}

static void accumulate_byte(char c)
{
    if (c == '$') {
        s_line_len = 0;
        s_line[s_line_len++] = '$';
    } else if (c == '\n') {
        dispatch_line();
        s_line_len = 0;
    } else if (c == '\r') {
        /* strip */
    } else if (s_line_len > 0 && s_line_len < LINE_BUF_SIZE - 1) {
        s_line[s_line_len++] = c;
    } else if (s_line_len >= LINE_BUF_SIZE - 1) {
        /* overflow — discard line */
        s_line_len = 0;
    }
}

/* ── I2C primitives ─────────────────────────────────────────────────────── */

/* Acquire, write a raw NMEA command (caller-supplied bytes including
 * leading '$', trailing '*CC\r\n'), release. */
static bool tx_command(const char *cmd)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_SENSOR, portMAX_DELAY);
    if (bus == NULL) return false;
    int r = i2c_write_timeout_us(bus, TESEO_ADDR, (const uint8_t *)cmd,
                                 strlen(cmd), false, I2C_TIMEOUT_US);
    i2c_bus_release(MOKYA_I2C_SENSOR);
    return r >= 0;
}

/* Build "$<body>*CC\r\n" and transmit. `body` is the payload between '$'
 * and '*' (no leading '$', no checksum). */
static bool send_nmea(const char *body)
{
    char out[CMD_BUF_SIZE];
    uint8_t cs = nmea_xor(body, strlen(body));
    int n = snprintf(out, sizeof out, "$%s*%02X\r\n", body, cs);
    if (n <= 0 || n >= (int)sizeof out) return false;
    return tx_command(out);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

bool teseo_init(void)
{
    /* Best-effort: ensure the engine is running in case a previous session
     * called $PSTMGPSSUSPEND before Core 1 rebooted. A write NACK here
     * doesn't latch offline — teseo_poll() confirms connectivity on the
     * 100 ms drain cadence via bytes actually read. A stuck engine will
     * be revealed by sentence_count staying flat across many polls. */
    (void)send_nmea("PSTMGPSRESTART");
    s_state.fix_valid = false;
    s_fix_rate        = GNSS_RATE_1HZ;
    return true;
}

bool teseo_poll(void)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_SENSOR, portMAX_DELAY);
    if (bus == NULL) return false;
    int r = i2c_read_timeout_us(bus, TESEO_ADDR, s_drain_buf,
                                sizeof s_drain_buf, false, I2C_TIMEOUT_US);
    i2c_bus_release(MOKYA_I2C_SENSOR);

    if (r < 0) {
        if (s_state.i2c_fail_count < UINT32_MAX) s_state.i2c_fail_count++;
        if (s_state.i2c_fail_count >= FAIL_DISCONNECT_COUNT)
            s_state.online = false;
        return false;
    }
    for (int i = 0; i < r; ++i) {
        uint8_t c = s_drain_buf[i];
        if (c == NMEA_FILL_BYTE) continue;
        if (c < 0x20 && c != '\r' && c != '\n') continue;
        accumulate_byte((char)c);
    }
    s_state.i2c_fail_count = 0;
    s_state.online = true;
    return true;
}

/* Send `body`, drain I2C, block until the expected OK/ERR reply arrives
 * or timeout elapses. Returns true only on OK match. */
static bool send_await(const char *body,
                       teseo_resp_t ok, teseo_resp_t err,
                       uint32_t timeout_ms)
{
    s_last_resp = TESEO_RESP_NONE;
    if (!send_nmea(body)) return false;

    TickType_t start   = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        (void)teseo_poll();
        teseo_resp_t r = s_last_resp;
        if (r == ok)  return true;
        if (r == err) return false;
        if ((xTaskGetTickCount() - start) >= timeout) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool teseo_set_fix_rate(gnss_rate_t rate)
{
    if (rate == GNSS_RATE_OFF) {
        if (!send_nmea("PSTMGPSSUSPEND")) return false;
        s_fix_rate = GNSS_RATE_OFF;
        return true;
    }

    /* Wake the engine first if it was suspended. $PSTMGPSRESTART has no
     * reply; it's a no-op if the engine is already running. */
    if (s_fix_rate == GNSS_RATE_OFF) {
        (void)send_nmea("PSTMGPSRESTART");
    }

    const char *body = NULL;
    switch (rate) {
        case GNSS_RATE_1HZ:  body = "PSTMSETPAR,1303,1.0"; break;
        case GNSS_RATE_2HZ:  body = "PSTMSETPAR,1303,0.5"; break;
        case GNSS_RATE_5HZ:  body = "PSTMSETPAR,1303,0.2"; break;
        case GNSS_RATE_10HZ: body = "PSTMSETPAR,1303,0.1"; break;
        default: return false;
    }

    /* 1. Write new period to the Current configuration block (RAM).
     *    Empirically CDB 303 changes in Current alone do NOT propagate
     *    to the running engine — it caches the period at init and does
     *    not re-read. Steps 2/3 are required to effect the change. */
    if (!send_await(body, TESEO_RESP_SETPAR_OK, TESEO_RESP_SETPAR_ERR, 500))
        return false;

    /* 2. Persist Current to NVM so the post-SRR reload keeps our value.
     *    SAVEPAR writes the entire config block, preserving bringup's
     *    NMEA mask bits. NVM endurance is 10k cycles; runtime rate
     *    changes happen at user cadence (≤ 100 over device lifetime),
     *    so this is not a wear concern. */
    if (!send_await("PSTMSAVEPAR",
                    TESEO_RESP_SAVEPAR_OK, TESEO_RESP_SAVEPAR_ERR, 1500))
        return false;

    /* 3. Reboot Teseo so the engine reloads config and picks up the new
     *    period. $PSTMSRR has no reply. NMEA stream drops out for ~1-2 s
     *    during reboot; teseo_poll()'s fail_count threshold (50) spans
     *    ~5 s so online=false will not latch spuriously. */
    (void)send_nmea("PSTMSRR");

    s_fix_rate = rate;
    return true;
}

bool teseo_get_fix_rate_from_device(char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    s_last_getpar_valid = false;
    if (!send_nmea("PSTMGETPAR,11303")) return false;

    TickType_t start   = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(500);
    while ((xTaskGetTickCount() - start) < timeout) {
        (void)teseo_poll();
        if (s_last_getpar_valid) {
            size_t n = strlen(s_last_getpar_value);
            if (n >= out_size) n = out_size - 1;
            for (size_t i = 0; i < n; ++i) out[i] = s_last_getpar_value[i];
            out[n] = 0;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    out[0] = 0;
    return false;
}

gnss_rate_t              teseo_get_fix_rate(void)  { return s_fix_rate; }
const teseo_state_t     *teseo_get_state(void)     { return &s_state; }
const teseo_sat_view_t  *teseo_get_sat_view(void)  { return &s_sat_view; }
const teseo_rf_state_t  *teseo_get_rf_state(void)  { return &s_rf_state; }

/* CDB 231 bit mask for the RF debug sentence set (all low-32 bits):
 *   bit  3 (0x00000008) $GPGST
 *   bit  5 (0x00000020) $PSTMNOISE
 *   bit  7 (0x00000080) $PSTMRF
 *   bit 23 (0x00800000) $PSTMCPU
 *   bit 30 (0x40000000) $PSTMNOTCHSTATUS
 * UM2229 §12.14 defines the mask positions. */
#define RF_DEBUG_MSG_MASK_LOW  0x408000A8u

bool teseo_enable_rf_debug_messages(bool on)
{
    /* Build $PSTMSETPAR,1231,<mask>,<mode> where mode=1 OR-sets the bits,
     * mode=2 AND-NOTs (clears) them. Current config → Teseo mutates in
     * RAM; SAVEPAR commits, SRR reboots engine so the NMEA output stream
     * reflects the new mask. */
    char body[48];
    int n = snprintf(body, sizeof body, "PSTMSETPAR,1231,%X,%u",
                     (unsigned)RF_DEBUG_MSG_MASK_LOW, on ? 1u : 2u);
    if (n <= 0 || n >= (int)sizeof body) return false;

    if (!send_await(body, TESEO_RESP_SETPAR_OK, TESEO_RESP_SETPAR_ERR, 500))
        return false;
    if (!send_await("PSTMSAVEPAR",
                    TESEO_RESP_SAVEPAR_OK, TESEO_RESP_SAVEPAR_ERR, 1500))
        return false;
    (void)send_nmea("PSTMSRR");
    return true;
}
