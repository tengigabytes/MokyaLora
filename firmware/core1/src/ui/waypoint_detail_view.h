/* waypoint_detail_view.h — D-4 single-waypoint detail.
 *
 * Layout (panel 320 × 224):
 *   y   0..15  header   "<name> / id 0xXXXXXXXX"
 *   y  16..207 body     full info block (lat/lon/expire/locked_to/icon
 *                        / description / source / epoch_seen / is_local)
 *   y 208..223 hint     "BACK list"
 *
 * Active waypoint id is stashed by the caller (D-3 OK or D-2 layer
 * cursor) via waypoint_detail_view_set_target(id). create() reads
 * it. BACK returns to D-3 (waypoints_view).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *waypoint_detail_view_descriptor(void);

/* External entry — callers (D-3, D-2 layer) push the target id before
 * navigating. 0 = unset (renders "no waypoint selected"). */
void     waypoint_detail_view_set_target(uint32_t waypoint_id);
uint32_t waypoint_detail_view_get_target(void);

#ifdef __cplusplus
}
#endif
