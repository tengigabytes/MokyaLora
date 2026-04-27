// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors
//
// Single TX gateway: push raw bytes (Meshtastic stream-protocol frames,
// already framed with 0x94 0xC3 LEN_HI LEN_LO + protobuf payload) onto
// the c1→c0 SERIAL_BYTES ring under a FreeRTOS mutex.
//
// Three callers will share this gateway in the cascade design:
//   1. USB-RX bridge task forwarding host bytes (Phase A pass-through).
//   2. LVGL outbound encoder (Phase D `phoneapi_encode_text_packet`).
//   3. Mode-transition logic (this Phase C: want_config_id, heartbeat,
//      disconnect).
//
// Mutex granularity is "one full ToRadio frame at a time". A frame may
// span multiple 256-byte ring slots; the mutex ensures a frame is not
// interleaved with another producer's bytes mid-stream (Core 0's
// PhoneAPI parser would lose alignment).

#ifndef MOKYA_PHONEAPI_TX_H
#define MOKYA_PHONEAPI_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void phoneapi_tx_init(void);

// Push `len` bytes as IPC_MSG_SERIAL_BYTES onto the c1→c0 ring,
// chunking into IPC_MSG_PAYLOAD_MAX-sized slots. The mutex is held
// across the entire push so the chunks are contiguous on the wire.
// Returns true on success; false if the mutex was unavailable or a
// chunk push failed (e.g. Core 0 not draining).
bool phoneapi_tx_push(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif  // MOKYA_PHONEAPI_TX_H
