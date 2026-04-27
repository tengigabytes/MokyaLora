// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors
//
// Hand-written minimal protobuf wire-format reader for FromRadio.
//
// Phase A scope (this file): walk the FromRadio oneof, identify which
// payload variant is present, RTT-trace it, and skip the rest. No
// field-level decoding yet — that lands in Phase B.
//
// Field tags below mirror Meshtastic's public mesh.proto FromRadio.
// Field numbers are wire-format interop facts and are not derived from
// any GPL-3.0 source code.

#ifndef MOKYA_PHONEAPI_DECODE_H
#define MOKYA_PHONEAPI_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "phoneapi_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

// FromRadio oneof tags (mesh.proto: message FromRadio { oneof payload_variant })
typedef enum {
    FR_TAG_NONE                  = 0,
    FR_TAG_PACKET                = 2,   // MeshPacket
    FR_TAG_MY_INFO               = 3,   // MyNodeInfo
    FR_TAG_NODE_INFO             = 4,   // NodeInfo
    FR_TAG_CONFIG                = 5,   // Config (sub-oneof)
    FR_TAG_LOG_RECORD            = 6,   // LogRecord (drop on Core 1)
    FR_TAG_CONFIG_COMPLETE_ID    = 7,   // uint32
    FR_TAG_REBOOTED              = 8,   // bool
    FR_TAG_MODULE_CONFIG         = 9,   // ModuleConfig
    FR_TAG_CHANNEL               = 10,  // Channel
    FR_TAG_QUEUE_STATUS          = 11,  // QueueStatus
    FR_TAG_XMODEM                = 12,  // XModem (drop)
    FR_TAG_METADATA              = 13,  // DeviceMetadata
    FR_TAG_MQTT_CLIENT_PROXY     = 14,  // MqttClientProxyMessage (drop)
    FR_TAG_FILE_INFO             = 15,  // FileInfo
    FR_TAG_CLIENT_NOTIFICATION   = 16,  // ClientNotification
    FR_TAG_DEVICEUI_CONFIG       = 17,  // DeviceUIConfig
} from_radio_tag_t;

// Phase A: identify the payload-variant tag and the (top-level) FromRadio.id.
// Returns FR_TAG_NONE if no payload-variant field is present (e.g. malformed
// frame, or only the top-level id).
typedef struct {
    uint32_t         frame_id;        // FromRadio.id (field 1, top-level)
    from_radio_tag_t variant_tag;     // which oneof field was present
    uint32_t         variant_value;   // for fixed-length scalar variants:
                                      //   config_complete_id, rebooted (0/1).
                                      // for length-delimited variants this
                                      // field holds the *byte length* of the
                                      // sub-message (Phase A inspection only).
} phoneapi_from_radio_summary_t;

// Parse a complete FromRadio frame (raw protobuf payload, NOT including
// the 4-byte stream header). Fills `out` with the identified variant.
// Returns true on a successful (well-formed) parse, false on malformed
// wire data.
bool phoneapi_decode_from_radio(const uint8_t *buf,
                                uint16_t       len,
                                phoneapi_from_radio_summary_t *out);

// Convenience: human-readable name for a tag (for tracing). Returns a
// short static string; safe to format with %s.
const char *phoneapi_from_radio_tag_name(from_radio_tag_t tag);

// ─── Field-level decoders (Phase B) ─────────────────────────────────
//
// Each function takes the *raw bytes of the sub-message* (without the
// outer FromRadio framing or the variant tag), as identified by the
// summary API above. They populate `out` and return true on success.
// On failure `out` may be partially populated.

bool phoneapi_decode_my_info(const uint8_t *buf, uint16_t len,
                             phoneapi_my_info_t *out);

bool phoneapi_decode_metadata(const uint8_t *buf, uint16_t len,
                              phoneapi_metadata_t *out);

bool phoneapi_decode_channel(const uint8_t *buf, uint16_t len,
                             phoneapi_channel_t *out);

bool phoneapi_decode_node_info(const uint8_t *buf, uint16_t len,
                               phoneapi_node_t *out);

// Helper to locate the variant payload within a FromRadio frame.
// Returns pointer + length of the variant sub-message bytes, or NULL.
// (For varint variants like config_complete_id, returns NULL — callers
// should use the summary API's variant_value field instead.)
const uint8_t *phoneapi_find_variant_payload(const uint8_t *buf, uint16_t len,
                                             from_radio_tag_t expected_tag,
                                             uint16_t *out_len);

#ifdef __cplusplus
}
#endif

#endif  // MOKYA_PHONEAPI_DECODE_H
