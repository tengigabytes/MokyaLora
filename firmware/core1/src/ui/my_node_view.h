/* my_node_view.h — C-4 my (local) node info.
 *
 * Read-only display of the local node's identity (long/short name,
 * node id, hw, role, region, fw version, etc). Edit paths land in
 * the Settings App (Owner / Device / LoRa groups), so this view is
 * "show + remind to edit via Settings".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *my_node_view_descriptor(void);

#ifdef __cplusplus
}
#endif
