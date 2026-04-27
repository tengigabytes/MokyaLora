// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors
//
// Cascade PhoneAPI session entry point for Core 1.
//
// Phase A scope: byte-stream tap from the c0→c1 SERIAL_BYTES ring. The
// bridge task calls `phoneapi_session_feed_from_core0()` whenever it
// pops a SERIAL_BYTES slot, in addition to its existing pass-through
// to TinyUSB CDC. Frames are parsed, identified, and RTT-traced; no
// cache is built yet.
//
// Phase C will add the mode state machine (STANDALONE / FORWARD), the
// USB DTR observer, and Core 1's self-issued want_config_id.

#ifndef MOKYA_PHONEAPI_SESSION_H
#define MOKYA_PHONEAPI_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHONEAPI_MODE_STANDALONE = 0,  ///< Core 1 owns the session
    PHONEAPI_MODE_FORWARD    = 1,  ///< USB host has opened CDC; Core 1 mirrors
} phoneapi_mode_t;

// Initialise the cascade tap. Call once during Core 1 startup, before
// the bridge task starts pumping the IPC rings.
void phoneapi_session_init(void);

// Feed bytes received from Core 0 (SERIAL_BYTES payload) into the tap.
// Safe to call with arbitrary chunk sizes — the framing parser handles
// frames split across multiple chunks.
void phoneapi_session_feed_from_core0(const uint8_t *buf, size_t len);

// Notify the session of a USB CDC connect/disconnect (DTR transition).
// Triggers mode transitions and any associated upstream messages
// (e.g. fresh `want_config_id` on FORWARD → STANDALONE).
void phoneapi_session_set_usb_connected(bool connected);

// Inspectors (for SWD / status line).
phoneapi_mode_t phoneapi_session_mode(void);
uint32_t        phoneapi_session_last_nonce(void);

#ifdef __cplusplus
}
#endif

#endif  // MOKYA_PHONEAPI_SESSION_H
