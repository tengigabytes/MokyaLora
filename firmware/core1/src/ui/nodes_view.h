/* nodes_view.h — C-1 node list (multi-row).
 *
 * Layout (panel 320 × 224):
 *   y   0..15  header "Nodes (N total)"
 *   y  16..207 8 visible rows × 24 px (one node per row)
 *   y 208..223 footer hint
 *
 * UP/DOWN moves focus, OK navigates to C-2 (VIEW_ID_NODE_DETAIL)
 * with the focused node id stashed via nodes_view_set_active_node.
 * BACK returns to L-0 home.
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

const view_descriptor_t *nodes_view_descriptor(void);

/* Cross-view stash: C-1 → C-2/C-3 hand-off. Set on OK from list,
 * read by node_detail_view / node_ops_view on create(). 0 = unset. */
void     nodes_view_set_active_node(uint32_t node_num);
uint32_t nodes_view_get_active_node(void);

#ifdef __cplusplus
}
#endif
