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

const teseo_state_t         *teseo_get_state(void);
const teseo_sat_view_t      *teseo_get_sat_view(void);

#endif /* MOKYA_CORE1_TESEO_LIV3FL_H */
