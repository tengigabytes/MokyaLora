/* channel_share_url.h — B-4 channel-share URL builder.
 *
 * Produces `https://meshtastic.org/e/#<base64>` where the body is a
 * url-safe base64 of an `apponly.proto` ChannelSet protobuf:
 *
 *   ChannelSet {
 *     repeated ChannelSettings settings = 1;
 *     Config.LoRaConfig lora_config = 2;
 *   }
 *
 * v1 emits exactly one ChannelSettings (the channel being shared) +
 * the cache's current LoRaConfig snapshot.  Multi-channel share is v2.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build the share URL for the channel at `index`. Returns the URL
 * string length (excluding NUL) on success, 0 on failure (channel
 * cache miss / lora cache miss / encoder overflow / `cap` too small).
 *
 * Suggested `cap`: 256 chars covers the worst case (32 B PSK + 11 B
 * name + module_settings + full LoRaConfig ≈ 110 B binary → 148 B
 * base64 + 26 B URL prefix = 174 chars; rounded for slack). */
size_t channel_share_url_build(uint8_t channel_index,
                                char *out, size_t cap);

#ifdef __cplusplus
}
#endif
