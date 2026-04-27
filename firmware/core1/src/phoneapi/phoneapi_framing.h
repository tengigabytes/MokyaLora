// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors
//
// Meshtastic stream-protocol framing parser (Core 1, cascade PhoneAPI client).
//
// Wire format (interop facts, originally documented in Meshtastic's
// public protobufs repository — no GPL code is reproduced here):
//
//     0x94 0xC3 LEN_HI LEN_LO <protobuf payload, LEN bytes>
//
// LEN is big-endian, max 512. Frames larger than the buffer are dropped
// and the parser re-syncs by sliding-searching for the next 0x94 0xC3.
//
// Designed to be fed bytes incrementally — `phoneapi_framing_push()` may
// be called with arbitrary chunk sizes (including across IPC ring slot
// boundaries). The on-frame callback fires once per complete frame.

#ifndef MOKYA_PHONEAPI_FRAMING_H
#define MOKYA_PHONEAPI_FRAMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PHONEAPI_FRAME_MAX 512u

typedef void (*phoneapi_frame_cb_t)(const uint8_t *payload, uint16_t len, void *user);

typedef enum {
    PHONEAPI_FRAMING_IDLE = 0,   // looking for 0x94
    PHONEAPI_FRAMING_MAGIC2,     // saw 0x94, expect 0xC3
    PHONEAPI_FRAMING_LEN_HI,     // saw 0x94 0xC3, expect LEN_HI
    PHONEAPI_FRAMING_LEN_LO,     // expect LEN_LO
    PHONEAPI_FRAMING_PAYLOAD,    // collecting LEN payload bytes
} phoneapi_framing_state_t;

typedef struct {
    phoneapi_framing_state_t state;
    uint16_t                 expected_len;
    uint16_t                 received_len;
    uint8_t                  buf[PHONEAPI_FRAME_MAX];
    phoneapi_frame_cb_t      on_frame;
    void                     *user;

    // Stats (inspectable via SWD)
    uint32_t frames_ok;
    uint32_t frames_oversized;
    uint32_t resync_drops;
} phoneapi_framing_t;

// Initialise a parser. `on_frame` may be NULL (frames will be parsed but discarded).
void phoneapi_framing_init(phoneapi_framing_t *ctx,
                           phoneapi_frame_cb_t on_frame,
                           void *user);

// Feed `len` bytes into the parser. Drives the state machine; fires
// `on_frame` once per complete frame.
void phoneapi_framing_push(phoneapi_framing_t *ctx,
                           const uint8_t *buf,
                           size_t len);

#ifdef __cplusplus
}
#endif

#endif  // MOKYA_PHONEAPI_FRAMING_H
