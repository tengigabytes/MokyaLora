/* waypoints_view.h — D-3 waypoint list (multi-row).
 *
 * Lists up to PHONEAPI_WAYPOINTS_CAP (8) waypoints sourced from
 * phoneapi_cache (cascade WAYPOINT_APP decoder for received broadcasts +
 * D-5 self-created). Newest first by epoch_seen.
 *
 * Each row format: "[focus] icon name  lat,lon  expire/sender"
 *
 * UP/DOWN walks cursor; OK navigates to D-4 (waypoint_detail_view) with
 * the focused waypoint id stashed via waypoints_view_set_active_id().
 * BACK returns to D-1 (map_view).
 *
 * v1: RAM-only — reboot clears all waypoints. Hint bar carries
 * "v1 重啟後重置" reminder.
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

const view_descriptor_t *waypoints_view_descriptor(void);

/* Cross-view stash: D-3 → D-4 hand-off. Set on OK from list, read
 * by waypoint_detail_view on create(). 0 = unset. */
void     waypoints_view_set_active_id(uint32_t waypoint_id);
uint32_t waypoints_view_get_active_id(void);

#ifdef __cplusplus
}
#endif
