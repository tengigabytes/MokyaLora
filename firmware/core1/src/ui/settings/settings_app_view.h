/* settings_app_view.h — S-0..S-X tree-based settings App (Phase 4).
 *
 * Replaces the flat settings_view at VIEW_ID_SETTINGS. Walks the
 * settings_tree (settings_tree.h); on entering a leaf, dispatches to
 * the matching template (toggle / enum / number / text) based on
 * settings_key_def_t.kind.
 *
 * Layout:
 *   y  0..15  status bar (global overlay)
 *   y 16..35  breadcrumb (settings_tree_format_breadcrumb)
 *   y 36..213 child list (12 visible rows × 16 px each)
 *   y 214..223 (no hint bar — settings App spec hides it)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "view_router.h"
#include "settings_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *settings_app_view_descriptor(void);

/* Stash a settings_group_t value to seed the cursor on the next view
 * activation. Used by modules_index_view (T2.4) to deep-link from the
 * S-7 list straight into a module's leaves without forcing the user
 * to re-navigate from the root each time.
 *
 * `valid` = false clears the stash (subsequent settings_view enter
 * starts at root as before). The stash is consumed exactly once: the
 * next create() reads it then re-arms valid=false.
 */
void settings_app_view_set_initial_group(settings_group_t g, bool valid);

#ifdef __cplusplus
}
#endif
