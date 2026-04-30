/* firmware_info_view.h — T-8 韌體資訊.
 *
 * Read-only summary screen reachable from T-0 tools.  Shows what's
 * running on each core: Core 0 firmware version (Meshtastic, from
 * phoneapi_cache_get_metadata), Core 1 git hash (CMake-injected
 * MOKYA_CORE1_GIT_HASH), HW model, pio_env, role, capability flags.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *firmware_info_view_descriptor(void);

#ifdef __cplusplus
}
#endif
