/* map_nav_view.h — D-6 navigation view (peer lock-and-track).
 *
 * Renders the bearing + range + ETA from the local GPS fix to the peer
 * node selected by the user via D-1's OK key (or by C-3 OP_NAVIGATE
 * from nodes_view). One screen, 1 Hz refresh, BACK returns to D-1.
 *
 * Target source (priority):
 *   1. map_nav_view_set_target(node_num) called by external entry
 *      path (nodes_view C-3 OP_NAVIGATE).
 *   2. map_view_get_nav_target() — set by D-1 OK on a peer cursor.
 *
 * The view loads the target on create() so a FUNC-out-and-back keeps
 * the same target. Use map_nav_view_set_target() before navigate to
 * change targets.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *map_nav_view_descriptor(void);

/* Override the next D-6 entry's target. Call BEFORE
 * view_router_navigate(VIEW_ID_MAP_NAV). Pass 0 to clear. Used by
 * nodes_view C-3 OP_NAVIGATE. */
void map_nav_view_set_target(uint32_t node_num);

#ifdef __cplusplus
}
#endif
