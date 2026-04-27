/* settings_view.h — LVGL panel for Core 1 settings UI (Stage 2/3).
 *
 * Lets the user browse and edit a curated subset of Meshtastic config
 * keys via the physical keypad. UP/DOWN cursor; L/R cycles groups; OK
 * edits or Apply; BACK clears pending.
 *
 * Talks to Core 0 through settings_client. SK_KIND_STR keys hand off
 * to ime_request_text (Stage 3 IME wiring).
 *
 * Per-key cache (s_cache[]) and UI mode survive destroy/recreate via
 * the view-owned static struct in settings_view.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

const view_descriptor_t *settings_view_descriptor(void);
