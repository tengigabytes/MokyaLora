/* tools_view.h — T-0 tools / diagnostics list.
 *
 * Plain selectable list. OK navigates to the focused diagnostic view.
 * Phase 3 entries: Keypad debug grid, RF debug, Font test (under
 * MOKYA_DEBUG_VIEWS).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_TOOLS_VIEW_H
#define MOKYA_CORE1_TOOLS_VIEW_H

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *tools_view_descriptor(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOKYA_CORE1_TOOLS_VIEW_H */
