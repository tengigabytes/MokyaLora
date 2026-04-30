/* map_view.h — D-1 vector PPI map view.
 *
 * Polar PPI radar centred on the local node:
 *   - 3 concentric range rings (1/3, 2/3, full of disc radius)
 *   - North marker outside the outer ring (north-up; v1 uses the GPS
 *     "true north" reference, magnetic north is v2)
 *   - "+" crosshair at the disc centre = local node (ME)
 *   - Peer labels placed at (range, azimuth) computed from the local
 *     fix and the cascade phoneapi_cache last_position (Phase B)
 *
 * Layer mask + zoom + cursor live entirely inside this view (D-2 is a
 * sub-mode, not a separate router id). D-6 navigation is a separate
 * view (`map_nav_view`) that this view hands off to via
 * `map_view_set_nav_target` + `view_router_navigate(VIEW_ID_MAP_NAV)`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *map_view_descriptor(void);

/* D-6 hand-off: returns the node_num the user last "OK locked" on a
 * peer cursor in D-1, or 0 if no peer has been locked yet. Phase C
 * map_nav_view reads this on entry. */
uint32_t map_view_get_nav_target(void);

#ifdef __cplusplus
}
#endif
