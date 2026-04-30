/* rhw_pins_view.h — S-7.10 RemoteHardware available_pins[] editor.
 *
 * Manages the 3 module-level config keys (enabled / allow_undef /
 * pin_count) plus a 4-slot list of RemoteHardwarePin entries. OK on
 * a slot row opens rhw_pin_edit_view for that slot; OK on Apply
 * pushes IPC SET frames + COMMIT_CONFIG; BACK returns to
 * modules_index_view without committing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "view_router.h"
#include "phoneapi_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *rhw_pins_view_descriptor(void);

/* Read-only access to the working copy — used by rhw_pin_edit_view to
 * pull initial values and push back the edited slot. */
const phoneapi_module_remote_hw_t *rhw_pins_view_get_working(void);
void rhw_pins_view_apply_slot_edit(uint8_t slot,
                                    const phoneapi_remote_hw_pin_t *pin);

#ifdef __cplusplus
}
#endif
