/* lora_test_view.h — T-5 LoRa self-test diagnostics view.
 *
 * Read-only dashboard surfacing the counters in `lora_test_log`.
 * Active loopback (TX self → RX self) is v2.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *lora_test_view_descriptor(void);

#ifdef __cplusplus
}
#endif
