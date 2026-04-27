/* keypad_view.h — physical keypad debug view (M3.3 Phase C).
 *
 * Landscape grid + DPAD + status strip. Highlights the most recent
 * KeyEvent for ~1 frame and tracks pushed / dropped / rejected counters
 * in the footer. All data driven from key_event.h globals.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

const view_descriptor_t *keypad_view_descriptor(void);
