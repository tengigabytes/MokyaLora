/* telemetry_view.h — F-0 telemetry app: F-1/F-2/F-3 sub-pages.
 *
 * F-1 本機遙測  — battery / voltage rails / uptime / charger temp.
 * F-2 環境感測  — barometric pressure, IMU/baro/mag temperatures, mag
 *                 vector, optional GPS-altitude cross-check.
 * F-3 鄰居資訊  — phoneapi_cache neighbour list with SNR / hops / age,
 *                 OK on a row hands the focused node to C-2.
 *
 * F-4 (歷史曲線) is deferred — see plan resilient-knitting-shore.md.
 *
 * Single view_id_t with internal sub-page state machine: LEFT/RIGHT
 * cycles F-1 ↔ F-2 ↔ F-3 (wraps both ways).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *telemetry_view_descriptor(void);

#ifdef __cplusplus
}
#endif
