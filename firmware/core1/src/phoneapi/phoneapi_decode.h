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

// MeshPacket → if portnum == TEXT_MESSAGE_APP, fill `out_msg` and
// return true. For non-text packets returns false (caller should drop).
// `out_msg->seq` is left zero — caller assigns when publishing.
#define PHONEAPI_PORTNUM_TEXT_MESSAGE_APP 1u
#define PHONEAPI_PORTNUM_POSITION_APP    3u
#define PHONEAPI_PORTNUM_ROUTING_APP     5u
#define PHONEAPI_PORTNUM_TRACEROUTE_APP 70u
#define PHONEAPI_PORTNUM_NEIGHBORINFO_APP 71u
bool phoneapi_decode_text_packet(const uint8_t *buf, uint16_t len,
                                 phoneapi_text_msg_t *out_msg);

// MeshPacket carrying a TRACEROUTE_APP (portnum 70) reply. Fills
// `out_from_node` with the originating peer (MeshPacket.from) and
// `out_route` with the parsed RouteDiscovery payload (forward + back
// hops, capped at PHONEAPI_ROUTE_HOPS_MAX). Returns false for
// non-traceroute packets / malformed data.
bool phoneapi_decode_traceroute_packet(const uint8_t *buf, uint16_t len,
                                       uint32_t *out_from_node,
                                       phoneapi_last_route_t *out_route);

// MeshPacket carrying a POSITION_APP (portnum 3) reply. Fills
// `out_from_node` with MeshPacket.from and `out_pos` with the parsed
// Position fields v1 cares about (lat_e7, lon_e7, alt, time). Returns
// false for non-position packets / malformed data.
bool phoneapi_decode_position_packet(const uint8_t *buf, uint16_t len,
                                     uint32_t *out_from_node,
                                     phoneapi_last_position_t *out_pos);

// MeshPacket carrying a NEIGHBORINFO_APP (portnum 71) broadcast. Fills
// `out_from_node` with MeshPacket.from (= the source peer reporting its
// neighbour list) and `out_nb` with up to PHONEAPI_NEIGHBORS_MAX
// neighbours. Each neighbour's `snr_x4` is the SNR the source measured
// for that neighbour (saturated to int8); INT8_MIN if absent on wire.
bool phoneapi_decode_neighborinfo_packet(const uint8_t *buf, uint16_t len,
                                          uint32_t *out_from_node,
                                          phoneapi_neighbors_t *out_nb);

// MeshPacket carrying a Routing-app ACK (decoded.portnum == 5).
//   - `out_request_id` ← Data.request_id (the original packet id we sent)
//   - `out_error_reason` ← Routing.error_reason (0 = delivered OK,
//     non-zero = meshtastic_Routing_Error)
// Returns true only when the packet is a routing-ack with both fields
// successfully extracted; false for non-routing packets / malformed.
bool phoneapi_decode_routing_ack(const uint8_t *buf, uint16_t len,
                                 uint32_t *out_request_id,
                                 uint8_t  *out_error_reason);

// FromRadio.queue_status (oneof tag 11).
typedef struct {
    int32_t  res;                    ///< meshtastic_Routing_Error (0 = ok)
    uint32_t free;                   ///< Free packets in queue
    uint32_t maxlen;                 ///< Queue capacity
    uint32_t mesh_packet_id;         ///< Originating MeshPacket.id (0 if unknown)
} phoneapi_queue_status_t;

bool phoneapi_decode_queue_status(const uint8_t *buf, uint16_t len,
                                  phoneapi_queue_status_t *out);

// ── Config sub-oneof decoders (B3-P1 / Cut B) ────────────────────────
//
// Each takes the *raw bytes of the sub-message* (e.g. DeviceConfig
// bytes — without the outer Config oneof tag/length). Field numbers
// are documented inside the implementation next to each parser, with
// the source line in firmware/core0/meshtastic/protobufs/meshtastic/config.proto.

bool phoneapi_decode_config_device(const uint8_t *buf, uint16_t len,
                                   phoneapi_config_device_t *out);
bool phoneapi_decode_config_lora(const uint8_t *buf, uint16_t len,
                                 phoneapi_config_lora_t *out);
bool phoneapi_decode_config_position(const uint8_t *buf, uint16_t len,
                                     phoneapi_config_position_t *out);
bool phoneapi_decode_config_display(const uint8_t *buf, uint16_t len,
                                    phoneapi_config_display_t *out);
bool phoneapi_decode_config_power(const uint8_t *buf, uint16_t len,
                                  phoneapi_config_power_t *out);
bool phoneapi_decode_config_security(const uint8_t *buf, uint16_t len,
                                     phoneapi_config_security_t *out);

// ── ModuleConfig sub-oneof decoders (B3 cascade walk-down) ──────────
//
// One per ModuleConfig sub-message we expose through IPC
// (module_config.proto field-numbers in parens):
//   field  5 = range_test    → phoneapi_decode_module_range_test
//   field  6 = telemetry     → phoneapi_decode_module_telemetry
//   field  7 = canned_message→ phoneapi_decode_module_canned_msg
//   field 10 = neighbor_info → phoneapi_decode_module_neighbor
//   field 11 = ambient_lighting → phoneapi_decode_module_ambient
//   field 12 = detection_sensor → phoneapi_decode_module_detect
//   field 13 = paxcounter    → phoneapi_decode_module_paxcounter
//
// Each takes the raw sub-message bytes (no outer ModuleConfig oneof
// tag/length) and decodes only the fields exposed via IpcConfigKey.
// `out` is zero-initialised inside; caller does not need to memset.

bool phoneapi_decode_module_telemetry(const uint8_t *buf, uint16_t len,
                                      phoneapi_module_telemetry_t *out);
bool phoneapi_decode_module_neighbor(const uint8_t *buf, uint16_t len,
                                     phoneapi_module_neighbor_t *out);
bool phoneapi_decode_module_range_test(const uint8_t *buf, uint16_t len,
                                       phoneapi_module_range_test_t *out);
bool phoneapi_decode_module_detect(const uint8_t *buf, uint16_t len,
                                   phoneapi_module_detect_t *out);
bool phoneapi_decode_module_canned_msg(const uint8_t *buf, uint16_t len,
                                       phoneapi_module_canned_msg_t *out);
bool phoneapi_decode_module_ambient(const uint8_t *buf, uint16_t len,
                                    phoneapi_module_ambient_t *out);
bool phoneapi_decode_module_paxcounter(const uint8_t *buf, uint16_t len,
                                       phoneapi_module_paxcounter_t *out);

/* T2.4 — 4 new modules.
 *   field  2 = serial         → phoneapi_decode_module_serial
 *   field  3 = ext.notif.     → phoneapi_decode_module_ext_notif
 *   field  4 = store_forward  → phoneapi_decode_module_store_forward
 *   field  9 = remote_hardware→ phoneapi_decode_module_remote_hw
 */
bool phoneapi_decode_module_store_forward(const uint8_t *buf, uint16_t len,
                                          phoneapi_module_store_forward_t *out);
bool phoneapi_decode_module_serial(const uint8_t *buf, uint16_t len,
                                   phoneapi_module_serial_t *out);
bool phoneapi_decode_module_ext_notif(const uint8_t *buf, uint16_t len,
                                      phoneapi_module_ext_notif_t *out);
bool phoneapi_decode_module_remote_hw(const uint8_t *buf, uint16_t len,
                                      phoneapi_module_remote_hw_t *out);

// Walk the outer Config message and invoke `cb` once per LD sub-field
// (each oneof variant lives inside one LD field — device=1, position=2,
// power=3, network=4, display=5, lora=6, bluetooth=7, security=8,
// sessionkey=9, device_ui=10 — see config.proto:1221–1232). Returns
// false if the buffer is malformed; partial dispatch may still occur.
typedef void (*phoneapi_config_field_cb)(uint32_t field_num,
                                         const uint8_t *sub_buf,
                                         uint16_t sub_len,
                                         void *ctx);
bool phoneapi_walk_config_oneof(const uint8_t *buf, uint16_t len,
                                phoneapi_config_field_cb cb, void *ctx);

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
