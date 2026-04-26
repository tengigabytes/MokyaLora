/* gps_dummy.h — Inject a fixed-position NMEA GGA into IpcGpsBuf at 1 Hz.
 *
 * Dev-only stand-in for the real Teseo path. Lets us validate the IpcGpsBuf
 * pipeline (Core 1 writer → shared SRAM → Core 0 reader) without sky view
 * and without the Teseo NMEA parser in the loop. Selected at build time via
 *
 *   cmake -B build/core1_bridge -DMOKYA_GPS_DUMMY_NMEA=ON ...
 *
 * Default OFF — production builds must run the real Teseo driver.
 *
 * Coordinates: 25.052103 N, 121.574039 E (Taipei). One GGA sentence per
 * second; no RMC, no GSV (Phase 1 dummy keeps to a single sentence type).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_GPS_DUMMY_H
#define MOKYA_CORE1_GPS_DUMMY_H

#include <stdbool.h>

#include "FreeRTOS.h"
#include "portmacro.h"

bool gps_dummy_start(UBaseType_t priority);

#endif /* MOKYA_CORE1_GPS_DUMMY_H */
