/* sys_diag_view.h — System diagnostics view (L-1 row 4 col 2).
 *
 * Three sub-pages, switched via LEFT/RIGHT keys:
 *   1. Resources — heap / SRAM / PSRAM / flash / LFS / MSP / uptime
 *   2. CPU + Tasks — Core 1 busy% (1s + 10s avg) + top tasks by stack
 *   3. Screen — FPS overlay toggle + pixel test (R/G/B/W/K)
 *
 * BACK exits to the launcher caller (matches HW Diag pattern). UP/DOWN/OK
 * are dispatched to the active page handler.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *sys_diag_view_descriptor(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
