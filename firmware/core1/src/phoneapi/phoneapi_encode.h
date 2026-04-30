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

// Phase D: outbound TEXT_MESSAGE_APP. Builds
//   ToRadio { packet { id=v, to=fixed32, channel=v,
//                       decoded { portnum=1, payload=<text> },
//                       want_ack=v } }
// `text_len` is the UTF-8 byte length (no NUL needed). Max ~200 B
// (longer mesh packets exist but the LVGL compose buffer caps at
// MESSAGES_SEND_TEXT_MAX). On success the locally-assigned, non-zero
// MeshPacket.id is written to *out_packet_id (M5F.1) so the caller can
// match it against an incoming Routing-app ACK or QueueStatus. Pass
// NULL if you don't care.
// Returns true on a successful TX-ring push.
bool phoneapi_encode_text_packet(uint32_t to_node_id,
                                  uint8_t  channel_index,
                                  bool     want_ack,
                                  const uint8_t *text,
                                  uint16_t text_len,
                                  uint32_t *out_packet_id);

/* C-3 OP_TRACEROUTE: send an empty TRACEROUTE_APP (portnum 70)
 * packet with want_response=true to `to_node_id`. The destination's
 * TraceRouteModule replies with a populated RouteDiscovery payload
 * carrying hop list + SNRs. Initial outgoing payload is empty —
 * Meshtastic Router fills route hops as the packet propagates.
 * out_packet_id receives the self-assigned MeshPacket.id so the
 * caller can match the eventual reply via cascade rx_packet. */
bool phoneapi_encode_traceroute(uint32_t to_node_id,
                                uint8_t  channel_index,
                                uint32_t *out_packet_id);

/* C-3 OP_REQUEST_POS: send an empty POSITION_APP (portnum 3) packet
 * with want_response=true to `to_node_id`. The destination's
 * PositionModule replies with its current Position payload. */
bool phoneapi_encode_position_request(uint32_t to_node_id,
                                      uint8_t  channel_index,
                                      uint32_t *out_packet_id);

/* P0-3 OP_FAVORITE / OP_IGNORE: send an AdminMessage (portnum 6,
 * ADMIN_APP) to OURSELVES (MeshPacket.to = my_node_num from
 * phoneapi_cache.my_info) carrying a single AdminMessage field that
 * targets `peer_node_num`. The local AdminModule processes it and
 * mutates the matching NodeInfo entry (is_favorite / is_unmessagable),
 * then re-emits NodeInfo through the cascade so phoneapi_cache picks
 * up the change.
 *
 *   set=true  → set_favorite_node    (admin.proto field 39, varint)
 *   set=false → remove_favorite_node (admin.proto field 40, varint)
 *
 * Returns false if my_info isn't cached yet (no my_node_num to address
 * the self-admin packet to).
 */
bool phoneapi_encode_admin_set_favorite(uint32_t peer_node_num,
                                        bool     set,
                                        uint32_t *out_packet_id);

/* B-3 加入頻道 — self-admin AdminMessage.set_channel (field 8).
 *
 * Builds a Channel { index, settings { psk, name }, role } sub-message
 * inside the AdminMessage and sends it locally (mp.from = mp.to = self).
 * AdminModule on Core 0 picks it up and writes channelFile.channels[N]
 * + persists. v1 only sets {psk, name} inside ChannelSettings — id,
 * uplink/downlink/module_settings stay at proto defaults.
 *
 * `name_len` must be ≤ 11 (proto cap). `psk_len` must be 0, 1, 16, or
 * 32 (proto convention; 1 = "use default LongFast PSK"). `role` is one
 * of phoneapi_channel_role_t. Returns false on body overflow / missing
 * my_node_num / out-of-range index. */
bool phoneapi_encode_admin_set_channel(uint8_t channel_index,
                                        const char *name, uint8_t name_len,
                                        const uint8_t *psk, uint8_t psk_len,
                                        uint8_t role,
                                        uint32_t *out_packet_id);

/* Same as set_favorite but for the ignore list (set_ignored_node = 47,
 * remove_ignored_node = 48). NodeInfo.is_unmessagable mirrors the
 * resulting state. */
bool phoneapi_encode_admin_set_ignored(uint32_t peer_node_num,
                                       bool     set,
                                       uint32_t *out_packet_id);

/* T2.5 OP_REMOTE_ADMIN — generic remote-admin AdminMessage encoder.
 *
 * Wraps a single varint field {admin_field, value} as the
 * AdminMessage body and ships it on portnum 6 (ADMIN_APP) with
 * MeshPacket.to = target_node_num.  Authentication on the remote
 * side is target-controlled: the target either has
 * config.security.admin_channel_enabled = true (admin_channel
 * lookup) or has our public key in its admin_key list.  This encoder
 * doesn't sign anything itself — Meshtastic 2.5+ admin_key signing
 * happens inside Core 0's MeshService::sendToMesh path.
 *
 * `want_ack=true` waits for routing-ack; `want_response=true` asks
 * the remote AdminModule to reply with a status frame.  Most reboot/
 * shutdown actions don't reply (the target is going down) so callers
 * typically pass want_response=false for those.
 *
 * Returns false on cascade-encode failure (TX ring full, my_info
 * missing — also blocks self-admin as a safety net).
 */
bool phoneapi_encode_admin_remote_varint(uint32_t target_node_num,
                                         uint8_t  channel_index,
                                         uint32_t admin_field,
                                         uint64_t value,
                                         bool     want_ack,
                                         bool     want_response,
                                         const char *trace_label,
                                         uint32_t *out_packet_id);

/* T2.5 thin wrappers for the common admin actions exposed by C-3 OP_
 * REMOTE_ADMIN sub-menu.  Field numbers come from admin.proto:
 *   reboot_seconds          = 97 (int32)
 *   shutdown_seconds        = 98 (int32)
 *   factory_reset_config    = 99 (int32, value > 0 triggers)
 *   factory_reset_device    = 94 (int32, value > 0 triggers)
 *   nodedb_reset            = 100 (bool)
 */
bool phoneapi_encode_admin_reboot(uint32_t target_node_num,
                                  uint8_t  channel_index,
                                  int32_t  seconds,
                                  uint32_t *out_packet_id);
bool phoneapi_encode_admin_shutdown(uint32_t target_node_num,
                                    uint8_t  channel_index,
                                    int32_t  seconds,
                                    uint32_t *out_packet_id);
bool phoneapi_encode_admin_factory_reset_config(uint32_t target_node_num,
                                                uint8_t  channel_index,
                                                uint32_t *out_packet_id);
bool phoneapi_encode_admin_factory_reset_device(uint32_t target_node_num,
                                                uint8_t  channel_index,
                                                uint32_t *out_packet_id);
bool phoneapi_encode_admin_nodedb_reset(uint32_t target_node_num,
                                        uint8_t  channel_index,
                                        uint32_t *out_packet_id);

#ifdef __cplusplus
}
#endif

#endif  // MOKYA_PHONEAPI_ENCODE_H
