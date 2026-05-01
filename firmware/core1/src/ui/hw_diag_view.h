/* hw_diag_view.h — Hardware diagnostics view (L-1 row 4 col 1).
 *
 * Single view containing 8 sub-pages, switched via LEFT/RIGHT keys.
 * Pages cover GNSS NMEA stream, GNSS RF/sat diag, LED brightness,
 * TFT backlight, button matrix, sensor readings, charger physical
 * quantities, and charger control. UP/DOWN/OK are dispatched to the
 * active page handler. BACK exits back to the launcher.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *hw_diag_view_descriptor(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
