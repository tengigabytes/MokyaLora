/* pairing_view.h — T-7 配對碼 (admin pairing code) display.
 *
 * Shows this device's Curve25519 public key from cascade-cached
 * `phoneapi_config_security`. Used to give to a peer node so it
 * can authorize this device as an admin (via admin_key[] config).
 *
 * Format: 32 B pubkey rendered as both:
 *   - 64-char hex (uppercase, two rows of 32 chars)
 *   - standard base64 (44 chars with '=' padding) — matches
 *     `meshtastic --info publicKey` byte-for-byte
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *pairing_view_descriptor(void);

#ifdef __cplusplus
}
#endif
