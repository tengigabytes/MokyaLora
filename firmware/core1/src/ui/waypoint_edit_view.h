/* waypoint_edit_view.h — D-5 add waypoint.
 *
 * v1 scope: GNSS-only mode. Two writable rows:
 *   Row 0  Name           OK opens IME for text edit
 *   Row 1  Save (GNSS)    OK reads teseo_state.lat/lon and inserts into
 *                         the waypoint cache as a local-source entry
 *
 * "Manual lat/lon" entry deferred to v2 — UI hint mentions it.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header "D-5 加航點"
 *   y  16..39   row 0  "Name : <text>"
 *   y  40..63   row 1  "Save (use GNSS now)"
 *   y  64..207  GNSS status block (fix / lat / lon / sats)
 *   y 208..223  hint
 *
 * Entry: from D-3 — TBD entry path. Phase 4 wires LEFT key in D-3 to
 * add. BACK returns to D-3.
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

const view_descriptor_t *waypoint_edit_view_descriptor(void);

#ifdef __cplusplus
}
#endif
