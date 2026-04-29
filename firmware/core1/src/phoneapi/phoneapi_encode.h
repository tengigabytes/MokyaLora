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

/* Same as set_favorite but for the ignore list (set_ignored_node = 47,
 * remove_ignored_node = 48). NodeInfo.is_unmessagable mirrors the
 * resulting state. */
bool phoneapi_encode_admin_set_ignored(uint32_t peer_node_num,
                                       bool     set,
                                       uint32_t *out_packet_id);

#ifdef __cplusplus
}
#endif

#endif  // MOKYA_PHONEAPI_ENCODE_H
