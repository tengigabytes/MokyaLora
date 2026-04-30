/* remote_admin_view.h — C-3 OP_REMOTE_ADMIN sub-menu (T2.5).
 *
 * Shows the destructive admin actions that can be sent to a remote
 * peer via Meshtastic AdminMessage (portnum 6 ADMIN_APP):
 *
 *   Reboot 5s
 *   Shutdown 5s
 *   Factory reset config (preserve BLE)
 *   Factory reset device (full)
 *   NodeDB reset (favorites preserved)
 *
 * UX is two-step "arm then confirm" to avoid accidental fires — first
 * OK on a row arms it (highlights yellow + status "OK again to fire,
 * BACK cancel"), second OK actually pushes the AdminMessage onto the
 * cascade TX ring.  Navigation away or BACK disarms.
 *
 * Authentication: target's AdminModule honours the request only when
 *   (a) target's config.security.admin_channel_enabled = true AND we
 *       sent on the admin channel index, OR
 *   (b) target's admin_key list contains our public key (Meshtastic
 *       2.5+ signed admin path).
 * Neither is checked client-side — UI just sends and the user verifies
 * via subsequent --info or the target dropping off the mesh on reboot.
 *
 * Reachable from C-3 node_ops_view via OP_REMOTE_ADMIN.  The target
 * node id is read from nodes_view_get_active_node() at create() time.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *remote_admin_view_descriptor(void);

#ifdef __cplusplus
}
#endif
