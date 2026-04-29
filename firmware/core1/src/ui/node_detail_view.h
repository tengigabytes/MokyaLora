/* node_detail_view.h — C-2 single-node full detail.
 *
 * Reads the active node id from nodes_view_get_active_node() and
 * renders all phoneapi_node_t fields. OK enters C-3 ops menu;
 * BACK returns to C-1 list.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *node_detail_view_descriptor(void);

#ifdef __cplusplus
}
#endif
