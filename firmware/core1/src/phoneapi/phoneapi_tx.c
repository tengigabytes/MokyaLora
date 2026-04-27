// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors

#include "phoneapi_tx.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "ipc_protocol.h"
#include "ipc_ringbuf.h"
#include "ipc_shared_layout.h"

static SemaphoreHandle_t s_tx_lock = NULL;
static uint8_t           s_tx_seq  = 0;

void phoneapi_tx_init(void)
{
    if (s_tx_lock == NULL) {
        s_tx_lock = xSemaphoreCreateMutex();
    }
}

bool phoneapi_tx_push(const uint8_t *buf, size_t len)
{
    if (s_tx_lock == NULL || buf == NULL || len == 0u) {
        return false;
    }

    if (xSemaphoreTake(s_tx_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    const uint8_t *p   = buf;
    size_t remaining   = len;
    bool   ok          = true;

    while (remaining > 0u) {
        size_t chunk = (remaining > IPC_MSG_PAYLOAD_MAX)
                           ? IPC_MSG_PAYLOAD_MAX
                           : remaining;

        // Spin-with-yield while the ring is full — Core 0 should drain
        // promptly, and Phase C frames are small so this rarely waits.
        while (!ipc_ring_push(&g_ipc_shared.c1_to_c0_ctrl,
                               g_ipc_shared.c1_to_c0_slots,
                               IPC_RING_SLOT_COUNT,
                               IPC_MSG_SERIAL_BYTES,
                               s_tx_seq,
                               p,
                               (uint16_t)chunk)) {
            taskYIELD();
        }
        s_tx_seq++;
        p         += chunk;
        remaining -= chunk;
    }

    xSemaphoreGive(s_tx_lock);
    return ok;
}
