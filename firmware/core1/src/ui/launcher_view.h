/* launcher_view.h — L-1 nine-grid app launcher (modal).
 *
 * Entered as a modal from any non-IME view via FUNC short-press. OK
 * launches the focused app; BACK cancels back to caller view.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_LAUNCHER_VIEW_H
#define MOKYA_CORE1_LAUNCHER_VIEW_H

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *launcher_view_descriptor(void);

/* Returns the currently-focused tile's target view_id. Used by the
 * router after modal_finish(true) to navigate to the picked app. May
 * return VIEW_ID_COUNT if a placeholder slot is focused; the router
 * should treat that as a no-op. */
view_id_t launcher_view_picked(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  /* MOKYA_CORE1_LAUNCHER_VIEW_H */
