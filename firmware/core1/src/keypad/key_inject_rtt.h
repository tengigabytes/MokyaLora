/* key_inject_rtt.h — SEGGER RTT down-channel transport for virtual
 * key injection. Coexists with the SWD-ring path in key_inject.h; both
 * parse into key_event_push_inject_flags().
 *
 * Uses RTT down-buffer index MOKYA_RTT_KEYINJ_CHAN (1), registered at
 * task start via SEGGER_RTT_ConfigDownBuffer. Channel 0 is left alone
 * for stdio / TRACE output.
 *
 * Start from main after SEGGER_RTT_Init() + FreeRTOS scheduler running.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define MOKYA_RTT_KEYINJ_CHAN   1

void key_inject_rtt_task_start(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
