/* rf_debug_view.h — LVGL view for Teseo-LIV3FL RF / signal diagnostics.
 *
 * Displays the driver-maintained teseo_rf_state_t + teseo_state_t
 * snapshots: noise floor, CPU, ANF status per band, per-sat C/N0.
 *
 * The underlying $PSTM* sentences are not enabled by default — call
 * teseo_enable_rf_debug_messages(true) once to flip them on in NVM.
 *
 * Debug-only: included in the build when MOKYA_DEBUG_VIEWS is defined.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

const view_descriptor_t *rf_debug_view_descriptor(void);
