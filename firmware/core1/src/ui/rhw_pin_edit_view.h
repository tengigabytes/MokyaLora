/* rhw_pin_edit_view.h — single-slot RemoteHardwarePin editor (S-7.10).
 *
 * Modal opened from rhw_pins_view via OK on a slot row. Edits 3 fields:
 * gpio_pin (uint8 0..255), name (UTF-8 ≤ 14 B, IME), type (3-value enum).
 * BACK pushes the local edit back to rhw_pins_view's working copy and
 * returns; nothing is committed here — pins_view's Apply does that.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *rhw_pin_edit_view_descriptor(void);

/* Set the slot index this editor will operate on next time it's opened.
 * Must be called by rhw_pins_view immediately before navigating to
 * VIEW_ID_T10_RHW_PIN_EDIT. */
void rhw_pin_edit_view_set_target(uint8_t slot);

#ifdef __cplusplus
}
#endif
