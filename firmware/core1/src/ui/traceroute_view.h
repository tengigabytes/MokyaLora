/* traceroute_view.h — T-1 Traceroute.
 *
 * Pick-a-peer-and-fire panel.  Top half = scrollable peer list (8
 * visible) sourced from phoneapi_cache; OK invokes
 * phoneapi_encode_traceroute() against the cursor's node.  Bottom half
 * shows the most-recent route reply for the focused node — auto-
 * refreshes when the cascade lands a RouteDiscovery (poll change_seq).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *traceroute_view_descriptor(void);

#ifdef __cplusplus
}
#endif
