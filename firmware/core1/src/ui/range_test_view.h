/* range_test_view.h — T-2 Range Test diagnostics view.
 *
 * Renders the per-peer hit table maintained by range_test_log + the
 * S-7.3 module config snapshot. Read-only; no key dispatch other than
 * BACK → tools.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *range_test_view_descriptor(void);

#ifdef __cplusplus
}
#endif
