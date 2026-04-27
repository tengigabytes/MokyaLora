// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors
//
// Hand-written ToRadio protobuf encoders. Phase C only emits the small
// upstream messages Core 1 needs to drive its own PhoneAPI session:
//   - want_config_id (field 3, varint)  : start a fresh state-stream
//   - heartbeat      (field 7, sub-msg) : keep the session alive
//   - disconnect     (field 4, bool)    : tell Core 0 we're going away
//
// MeshPacket-bearing ToRadio (field 1) for outbound text comes in
// Phase D once messages_send.c migrates off the IPC_CMD_SEND_TEXT stub.
//
// Each encoder builds the full stream-protocol frame
// (`0x94 0xC3 LEN_HI LEN_LO <payload>`) on the stack and pushes it
// through phoneapi_tx_push() under the TX mutex.

#ifndef MOKYA_PHONEAPI_ENCODE_H
#define MOKYA_PHONEAPI_ENCODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool phoneapi_encode_want_config_id(uint32_t nonce);
bool phoneapi_encode_heartbeat(void);
bool phoneapi_encode_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif  // MOKYA_PHONEAPI_ENCODE_H
