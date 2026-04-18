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
#define DRAIN_BUF_SIZE        256u         /* one transaction per tick     */
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
static gnss_rate_t      s_fix_rate = GNSS_RATE_1HZ;   /* CDB 303 default */

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

static void dispatch_line(void)
{
    if (s_line_len < 8) return;
    if (s_line[0] != '$') return;
    if (!nmea_verify(s_line, s_line_len)) return;

    /* Sentence type = 5 chars starting at s_line+1 (e.g. "GNGGA"). */
    if (s_line_len < 7) return;
    const char *type = s_line + 1;
    const char *body = s_line + 1;

    if (memcmp(type + 2, "GGA", 3) == 0)      parse_gga(body);
    else if (memcmp(type + 2, "RMC", 3) == 0) parse_rmc(body);
    else if (memcmp(type + 2, "GSV", 3) == 0) parse_gsv(type, body);
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

bool teseo_set_fix_rate(gnss_rate_t rate)
{
    if (rate == GNSS_RATE_OFF) {
        if (!send_nmea("PSTMGPSSUSPEND")) return false;
        s_fix_rate = GNSS_RATE_OFF;
        return true;
    }

    const char *body = NULL;
    switch (rate) {
        case GNSS_RATE_1HZ:  body = "PSTMSETPAR,1303,1.0"; break;
        case GNSS_RATE_2HZ:  body = "PSTMSETPAR,1303,0.5"; break;
        case GNSS_RATE_5HZ:  body = "PSTMSETPAR,1303,0.2"; break;
        case GNSS_RATE_10HZ: body = "PSTMSETPAR,1303,0.1"; break;
        default: return false;
    }

    /* If the engine was suspended, wake it first. $PSTMGPSRESTART has no
     * reply, so just fire-and-forget. */
    if (s_fix_rate == GNSS_RATE_OFF) {
        (void)send_nmea("PSTMGPSRESTART");
    }

    if (!send_nmea(body)) return false;

    s_fix_rate = rate;
    return true;
}

gnss_rate_t              teseo_get_fix_rate(void)  { return s_fix_rate; }
const teseo_state_t     *teseo_get_state(void)     { return &s_state; }
const teseo_sat_view_t  *teseo_get_sat_view(void)  { return &s_sat_view; }
