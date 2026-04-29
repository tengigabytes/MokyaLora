/* node_ops_view.h — C-3 per-node operations menu.
 *
 * Reads the active node id from nodes_view_get_active_node(). Lists
 * per-node actions per spec: DM, alias, favorite, ignore, traceroute,
 * request position, remote admin. Most are placeholders that flash
 * a hint until the underlying mechanism lands.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *node_ops_view_descriptor(void);

#ifdef __cplusplus
}
#endif
