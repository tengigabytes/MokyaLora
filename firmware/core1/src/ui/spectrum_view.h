/* spectrum_view.h — T-3 訊號頻譜 (per-peer SNR bar chart).
 *
 * v1: passive — visualises the SNR / hops / heard-age the cache
 * already has from NodeInfo broadcasts. No active SX1262 RSSI scan
 * (that needs Core 0 IPC + a Meshtastic submodule patch — v2).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *spectrum_view_descriptor(void);

#ifdef __cplusplus
}
#endif
