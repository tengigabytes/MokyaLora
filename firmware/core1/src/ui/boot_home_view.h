/* boot_home_view.h — L-0 Home / Dashboard.
 *
 * Boot view per docs/ui/20-launcher-home.md. Status dashboard rows on
 * top, message zone and event zone underneath. No hint bar.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_BOOT_HOME_VIEW_H
#define MOKYA_CORE1_BOOT_HOME_VIEW_H

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *boot_home_view_descriptor(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  /* MOKYA_CORE1_BOOT_HOME_VIEW_H */
