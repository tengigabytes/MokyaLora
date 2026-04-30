/* modules_index_view.h — S-7 模組設定 list (10 sub-pages).
 *
 * Reachable from the settings App by pressing RIGHT at the root list.
 * Renders all 10 spec-defined ModuleConfig sub-pages:
 *
 *   S-7.1 Canned Message     ✓ wired (SG_CANNED_MSG)
 *   S-7.2 External Notif.    TBD (no IPC keys yet)
 *   S-7.3 Range Test         ✓ wired (SG_RANGE_TEST)
 *   S-7.4 Store & Forward    TBD
 *   S-7.5 Telemetry          ✓ wired (SG_TELEMETRY)
 *   S-7.6 Detection Sensor   ✓ wired (SG_DETECT_SENSOR)
 *   S-7.7 Paxcounter         ✓ wired (SG_PAXCOUNTER)
 *   S-7.8 Neighbor Info      ✓ wired (SG_NEIGHBOR)
 *   S-7.9 Serial             TBD
 *   S-7.10 Remote Hardware   TBD
 *
 * OK on a wired entry stashes the target group via
 * settings_app_view_set_initial_group() and navigates to
 * VIEW_ID_SETTINGS — the next create() seeds the cursor at that group
 * directly, so the user lands inside the chosen module's leaf list.
 *
 * OK on a TBD entry surfaces "IPC pending" status (no navigation).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *modules_index_view_descriptor(void);

#ifdef __cplusplus
}
#endif
