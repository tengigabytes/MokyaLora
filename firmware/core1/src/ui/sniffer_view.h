/* sniffer_view.h — T-4 packet sniffer (newest-first packet log).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *sniffer_view_descriptor(void);

#ifdef __cplusplus
}
#endif
