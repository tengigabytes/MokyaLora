/* teseo_liv3fl.h — ST Teseo-LIV3FL GNSS driver (Core 1).
 *
 * Datasheet: DS13881 Rev 4. Software manual: UM2229 Rev 5.
 * On MokyaLora Rev A the device is on the sensor bus (I2C, 7-bit addr
 * 0x3A; time-muxed i2c1, GPIO 34/35). The nRST pin is not wired to any
 * MCU GPIO — software reset only (`$PSTMSRR`).
 *
 * The driver pulls NMEA stream bytes from the receiver on a 100 ms tick
 * (see gps_task.c), accumulates complete sentences, verifies checksums
 * and parses:
 *   - GGA → fix quality / num sats / HDOP / altitude / position / UTC time
 *   - RMC → fix status / speed / course / UTC date
 *   - GSV → per-talker satellite-in-view accumulator, pooled into a
 *           single 32-entry snapshot on each cycle completion
 *
 * GSA, VTG and other sentences are ignored. NVM is never written — the
 * bringup session has already committed the NMEA message mask (GGA +
 * RMC + GSV + GLONASS + Galileo) via `$PSTMSAVEPAR`.
 *
 * Rate control. The native ODR is runtime-adjustable via
 * teseo_set_fix_rate(). Under the hood we write CDB 303 (GNSS FIX Rate,
 * period in seconds, double) with `$PSTMSETPAR,1303,<period>,0` which
 * modifies RAM only — no NVM write, no wear. GNSS_RATE_OFF issues
 * `$PSTMGPSSUSPEND`; any non-OFF after OFF issues `$PSTMGPSRESTART`
 * first. LIV3FL supports 1/2/5/10 Hz (DS13881 Table 1 "Max fix rate").
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_TESEO_LIV3FL_H
#define MOKYA_CORE1_TESEO_LIV3FL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    GNSS_RATE_OFF  = 0,           /* $PSTMGPSSUSPEND — engine parked   */
    GNSS_RATE_1HZ  = 1,
    GNSS_RATE_2HZ  = 2,
    GNSS_RATE_5HZ  = 5,
    GNSS_RATE_10HZ = 10,
} gnss_rate_t;

typedef struct {
    bool     online;              /* I2C reachable (not necessarily fixed) */
    bool     fix_valid;           /* RMC status = 'A'                     */
    uint8_t  fix_quality;         /* GGA field 6 (0=none 1=GPS 2=DGPS)    */
    uint8_t  num_sats;            /* GGA field 7                          */
    int32_t  lat_e7;              /* degrees × 1e7 (matches IPC payload)  */
    int32_t  lon_e7;
    int16_t  altitude_m;          /* GGA field 9 (integer meters)         */
    int16_t  speed_kmh_x10;       /* km/h × 10 (from RMC knots × 1.852)   */
    int16_t  course_deg_x10;      /* true heading deg × 10 (RMC field 8)  */
    uint16_t hdop_x10;            /* GGA field 8 × 10                     */
    uint32_t utc_time;            /* hhmmss                               */
    uint32_t utc_date;            /* ddmmyy                               */
    uint32_t i2c_fail_count;      /* running I2C drain error count        */
    uint32_t sentence_count;      /* parsed RMC + GGA running total       */
    /* $GPGST — pseudorange noise stats. Enabled by the RF debug mask;
     * populated only after that commissioning. Stored as meters × 10 to
     * keep things integer; σ range fits in uint16_t (max 6553.5 m). */
    uint16_t gst_sigma_lat_m_x10;
    uint16_t gst_sigma_lon_m_x10;
    uint16_t gst_sigma_alt_m_x10;
    uint32_t gst_count;
} teseo_state_t;

typedef struct {
    uint8_t  prn;                 /* satellite PRN number (1-255)         */
    uint8_t  elevation_deg;       /* 0–90                                 */
    uint16_t azimuth_deg;         /* 0–359                                */
    uint8_t  snr_dbhz;            /* C/No, 0 = not tracking               */
} teseo_sat_info_t;

typedef struct {
    uint8_t          count;       /* valid entries in sats[]              */
    uint32_t         update_count;/* bumped when a talker cycle completes */
    teseo_sat_info_t sats[32];    /* pooled across GP / GL / GA / BD      */
} teseo_sat_view_t;

/* ── RF / signal-quality diagnostics ─────────────────────────────────── *
 *
 * Populated by $PSTMRF, $PSTMNOISE, $PSTMNOTCHSTATUS, $PSTMCPU sentences
 * if the host has enabled them in CDB 231/232 (NMEA-over-I2C message mask).
 * Call teseo_enable_rf_debug_messages(true) once to switch them on (NVM
 * write + SRR — user-initiated, not every boot). Without enabling, every
 * counter below stays at 0 and the driver quietly ignores the lines.   */

typedef struct {
    uint8_t  prn;                 /* satellite PRN                        */
    uint8_t  cn0_dbhz;            /* carrier-to-noise ratio, dB-Hz        */
    int16_t  phase_noise;         /* raw signed (no documented units)     */
    int32_t  freq_hz;             /* tracking frequency (Hz, signed)      */
} teseo_rf_sat_t;

typedef struct {
    uint32_t freq_hz;             /* $PSTMNOTCHSTATUS kfreq_now_Hz_*     */
    uint32_t power;               /* BPF internal power estimate          */
    uint16_t ovfs;                /* bit 12 (0x1000) = jammer removed;
                                   * bits 0-2 = internal overflow flags   */
    uint8_t  lock;                /* 0 = unlocked, 1 = locked             */
    uint8_t  mode;                /* 0=disabled 1=always 2=auto           */
} teseo_anf_path_t;

typedef struct {
    /* $PSTMNOISE */
    int32_t            noise_gps;
    int32_t            noise_gln;
    uint32_t           noise_count;

    /* $PSTMNOTCHSTATUS — GPS and GLONASS ANF path (L1 / L1OF) */
    teseo_anf_path_t   anf_gps;
    teseo_anf_path_t   anf_gln;
    uint32_t           anf_count;

    /* $PSTMCPU — engine CPU load */
    uint16_t           cpu_pct_x10;     /* % × 10                         */
    uint16_t           cpu_mhz;
    uint32_t           cpu_count;

    /* $PSTMRF — per-satellite signal detail, accumulated across
     * MessgIndex 1..MessgAmount (up to 3 sats per sentence). */
    uint8_t            rf_sat_count;
    uint32_t           rf_update_count;
    teseo_rf_sat_t     rf_sats[32];
} teseo_rf_state_t;

const teseo_rf_state_t      *teseo_get_rf_state(void);

/* Enable/disable $PSTMRF + $PSTMNOISE + $PSTMNOTCHSTATUS + $PSTMCPU +
 * $GPGST on the I2C NMEA output by writing CDB 231 (mask = 0x408000A8)
 * with mode=1 (OR) or mode=2 (AND-NOT), followed by $PSTMSAVEPAR +
 * $PSTMSRR so the engine reloads. Blocks ~2 s including the reboot.
 * Must be called from a FreeRTOS task context (uses vTaskDelay). */
bool                         teseo_enable_rf_debug_messages(bool on);

/* Engine restart commands (UM2229 §10.2 — fire-and-forget, no reply).
 * - cold:  clears almanac + ephemeris + UTC, full re-acquire (~30-60 s)
 * - warm:  keeps almanac, re-acquires ephemeris (~10-20 s)
 * - hot:   keeps everything, just refixes (~few s)
 * Each issues PSTMCOLDSTART / PSTMWARMSTART / PSTMHOTSTART.
 * Engine drops out of NMEA stream for ~1-2 s during the restart. */
bool                         teseo_cold_start(void);
bool                         teseo_warm_start(void);
bool                         teseo_hot_start(void);

/* Persist the current Teseo configuration block to NVM. Acks via
 * $PSTMSAVEPAROK / $PSTMSAVEPARERROR. Blocks up to 1.5 s. */
bool                         teseo_savepar(void);

/* Reboot the engine without changing config. Reloads NVM into the
 * current block and re-applies. No reply. ~1-2 s NMEA dropout. */
bool                         teseo_srr(void);

/* Restore Teseo NVM to factory defaults via $PSTMRESTOREPAR. Replies
 * are not parsed by the driver; caller should expect a 1-2 s NMEA
 * dropout and re-init may be needed. DESTRUCTIVE — UI confirms. */
bool                         teseo_restore_defaults(void);

/* ── Constellation presets (UM2229 §12.18 CDB-200 + CDB-227) ─────── *
 *
 * UM2229 limits valid combinations to single-constellation modes plus
 * three multi-coverage sets:
 *   1. GPS only
 *   2. GLONASS only
 *   3. Galileo only
 *   4. BeiDou only
 *   5. QZSS only
 *   6. GPS + SBAS (augmentation overlay)
 *   7. GPS + GAL + QZSS + GLONASS  (max coverage western)
 *   8. GPS + GAL + QZSS + BeiDou   (max coverage Asia)
 *   9. GLONASS + BeiDou
 *
 * Internally: GETPAR CDB-200/227 → mask out constellation+SBAS bits
 * (other features like Walking Mode, Stop Detection, etc. preserved)
 * → OR in preset bits → SETPAR × 2 → SAVEPAR → SRR. ~2-3 s blocking. */
typedef enum {
    TESEO_CONST_GPS              = 0,
    TESEO_CONST_GLONASS          = 1,
    TESEO_CONST_GALILEO          = 2,
    TESEO_CONST_BEIDOU           = 3,
    TESEO_CONST_QZSS             = 4,
    TESEO_CONST_GPS_SBAS         = 5,
    TESEO_CONST_GPS_GAL_QZSS_GLN = 6,
    TESEO_CONST_GPS_GAL_QZSS_BD  = 7,
    TESEO_CONST_GLN_BD           = 8,
    TESEO_CONST_PRESET_COUNT     = 9,
} teseo_const_preset_t;

bool                         teseo_set_constellation_preset(teseo_const_preset_t p);

/* Read currently-configured CDB-200 + CDB-227. Useful for the UI to
 * check "✓" mark on the active preset. Both filled on success.
 * Blocks ~500 ms (two GETPAR round trips). */
bool                         teseo_get_constellation_raw(uint32_t *cdb200,
                                                          uint32_t *cdb227);

/* ── Tracking / positioning thresholds (UM2229 §12.2/3/4/12) ─────── *
 *
 * All require either a SAVEPAR + SRR (for permanent + immediate) or
 * just SETPAR to RAM (lost on next SRR or reboot). The driver writes
 * to RAM only — caller pairs with teseo_savepar() + teseo_srr() to
 * make permanent. Returns true on SETPAR ACK from device.
 *
 * Ranges (UM2229):
 *   mask_angle_deg              0..30 (typical 5)
 *   tracking_cn0_db             0..40 (typical 25)
 *   positioning_cn0_db          0..40 (typical 30)
 *   position_mask_angle_deg     0..30 (typical 5) — separate from mask_angle */
bool                         teseo_set_mask_angle(uint8_t deg);                /* CDB-104 */
bool                         teseo_set_tracking_cn0(uint8_t dB);                /* CDB-105 */
bool                         teseo_set_positioning_cn0(uint8_t dB);             /* CDB-132 */
bool                         teseo_set_position_mask_angle(uint8_t deg);        /* CDB-198 */

/* CDB-272: position/time integrity check. bits b0=position, b1=time. */
bool                         teseo_set_integrity_check(uint8_t bits);

/* CDB-125 notch filter mode bitmap.
 *   bit 0  = notch filter enabled (master)
 *   bit 1  = GPS notch normal mode
 *   bit 2  = GLN notch normal mode
 *   bit 3  = GPS notch auto-insertion (jammer-locked)
 *   bit 4  = GLN notch auto-insertion */
bool                         teseo_set_notch_filter(uint8_t mode_bits);

/* Generic GETPAR — read CDB-1<id> and return as uint32. Useful for
 * UI to display current device-side values. */
bool                         teseo_getpar_u32(uint16_t cdb_id, uint32_t *out);

/* ── NMEA output customisation (UM2229 §12.7/12.30) ──────────────── *
 *
 * D4 — Talker ID (CDB-131): single ASCII char ('P', 'N', 'A', 'B', 'L'
 *      etc) replacing the second char of GGA/RMC/VTG/GLL talker IDs
 *      (e.g. "GP" → "G<char>"). Affects only the listed sentences;
 *      GSV/GSA are governed by CDB-200 b19/b20.
 *
 * D5 — NMEA-on-I2C mask preset (CDB-231/232): writes the 64-bit mask
 *      to one of 4 sensible presets:
 *        MINIMAL  — RMC + GGA only (lowest bandwidth)
 *        NORMAL   — RMC + GGA + GSV (current driver default)
 *        FULL     — all standard NMEA (adds GSA/VTG/GLL/...)
 *        DEBUG    — Full + ST proprietary (PSTMRF/NOISE/NOTCHSTATUS/CPU/GST)
 *
 * Each makes SAVEPAR + SRR — engine reboots ~1-2s. */
bool                         teseo_set_talker_id(char id);

typedef enum {
    TESEO_NMEA_PRESET_MINIMAL = 0,
    TESEO_NMEA_PRESET_NORMAL  = 1,
    TESEO_NMEA_PRESET_FULL    = 2,
    TESEO_NMEA_PRESET_DEBUG   = 3,
    TESEO_NMEA_PRESET_COUNT   = 4,
} teseo_nmea_preset_t;

bool                         teseo_set_nmea_preset(teseo_nmea_preset_t p);

/* ── Odometer (UM2229 §12.39 CDB-270) ────────────────────────────── *
 *
 * Bits 0..2: enable / NMEA enable / autostart (each 0 or 1).
 * Bits 16..31: 16-bit alarm distance in metres (0 = no alarm).
 * Caller bundles SAVEPAR + SRR via teseo_savepar/teseo_srr. */
typedef struct {
    bool     enabled_on_boot;
    bool     nmea_enabled;
    bool     autostart;
    uint16_t alarm_m;
} teseo_odometer_cfg_t;

bool                         teseo_set_odometer_cfg(const teseo_odometer_cfg_t *cfg);
bool                         teseo_get_odometer_cfg(teseo_odometer_cfg_t *cfg);

/* ── Data logger (UM2229 §12.36/37 CDB-266 + CDB-267) ────────────── *
 *
 * CDB-266 fields control logger storage strategy / behaviour, CDB-267
 * controls minimal distance between log records (metres).
 * v1: only expose enabled-on-boot + minimal-distance, sufficient for
 *     basic walk-tracking use cases. */
typedef struct {
    bool     enabled_on_boot;
    uint16_t min_distance_m;
} teseo_logger_cfg_t;

bool                         teseo_set_logger_cfg(const teseo_logger_cfg_t *cfg);
bool                         teseo_get_logger_cfg(teseo_logger_cfg_t *cfg);

/* ── Geofencing (UM2229 §12.38 CDB-268 + 314..325) ───────────────── *
 *
 * v1 NOTE: only read access exposed for now. Editing requires a digit-
 * input widget which the launcher doesn't currently have — defer to a
 * future phase that adds a numeric-entry widget. */
typedef struct {
    bool     enabled;
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint32_t radius_m;
} teseo_geofence_circle_t;

bool                         teseo_get_geofence_master(uint32_t *cdb268_raw);
bool                         teseo_get_geofence_circle(uint8_t idx,
                                                       teseo_geofence_circle_t *out);

/* Initialise driver state + send $PSTMGPSRESTART to guarantee the engine
 * is running (no-op if already running). Caller must hold no mutex. */
bool                         teseo_init(void);

/* Drain a burst of NMEA bytes (single I2C transaction on the sensor bus)
 * and feed them into the line accumulator / parser. Call on a 100 ms
 * cadence. Returns false on I2C failure — state.i2c_fail_count is
 * incremented; after FAIL_DISCONNECT_COUNT failures online := false. */
bool                         teseo_poll(void);

/* Set the device FIX rate. For non-OFF values writes CDB 303 via
 * `$PSTMSETPAR,1303,<period_s>,0`; period = 1/rate_hz. GNSS_RATE_OFF
 * issues `$PSTMGPSSUSPEND`. Auto-restarts the engine when transitioning
 * from OFF to a non-OFF rate. Does not save to NVM. */
bool                         teseo_set_fix_rate(gnss_rate_t rate);

/* Cached last-set value (independent of what Teseo currently has in
 * RAM; we don't read back to avoid a round-trip). */
gnss_rate_t                  teseo_get_fix_rate(void);

/* Diagnostic: send $PSTMGETPAR,11303 and capture the value Teseo reports
 * as its Current CDB 303 (FIX rate period in seconds). Writes a decimal
 * ASCII string to `out` (e.g. "0.1\0" for 10 Hz). Returns false on timeout.
 * Blocks for up to 500 ms. */
bool                         teseo_get_fix_rate_from_device(char *out,
                                                             size_t out_size);

const teseo_state_t         *teseo_get_state(void);
const teseo_sat_view_t      *teseo_get_sat_view(void);

/* Raw NMEA line ring (HW Diag "GNSS NMEA" page).
 *
 * Each entry is a checksum-validated NMEA sentence ($... up to and
 * including the *XX suffix), null-terminated. Writer is gps_task in
 * dispatch_line(); reader is the UI lvgl_task.  No mutex — readers
 * use teseo_get_raw_seq() to detect new lines and receive a stable
 * snapshot via teseo_get_raw_lines() (memcpy under no-lock; field
 * tearing impossible because we only ever write whole lines + bump
 * a 32-bit seq AFTER the bytes are in place).
 *
 * Capacity: 16 lines × 80 chars = 1280 bytes in PSRAM .bss. */
#define TESEO_RAW_LINE_MAX    80u

/* Snapshot up to `max` most-recent lines (newest first) into the
 * caller-supplied 2D buffer. Returns the count actually written
 * (≤ min(max, ring depth)). The buffer dimension MUST be
 * `TESEO_RAW_LINE_MAX`. */
uint8_t                      teseo_get_raw_lines(char (*out)[TESEO_RAW_LINE_MAX],
                                                  uint8_t max);

/* Monotonic counter — bumps once per published NMEA line. UI compares
 * against last-seen value to gate redraws. */
uint32_t                     teseo_get_raw_seq(void);

#endif /* MOKYA_CORE1_TESEO_LIV3FL_H */
