// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors

#include "phoneapi_encode.h"

#include <string.h>

#include "hardware/timer.h"

#include "phoneapi_cache.h"
#include "phoneapi_tx.h"
#include "mokya_trace.h"

#define MAGIC1 0x94u
#define MAGIC2 0xC3u

// Encode a varint into `dst` at offset `*pos`. Returns false on overflow.
static bool put_varint(uint8_t *dst, size_t cap, size_t *pos, uint64_t v)
{
    do {
        if (*pos >= cap) return false;
        uint8_t b = (uint8_t)(v & 0x7Fu);
        v >>= 7;
        if (v != 0u) b |= 0x80u;
        dst[(*pos)++] = b;
    } while (v != 0u);
    return true;
}

static bool put_tag(uint8_t *dst, size_t cap, size_t *pos,
                    uint32_t field_num, uint8_t wire_type)
{
    uint64_t tag = ((uint64_t)field_num << 3) | (wire_type & 0x07u);
    return put_varint(dst, cap, pos, tag);
}

// Wrap `payload[len]` with stream-protocol header and push. Sized to
// hold the largest ToRadio we emit (text packet ≤ 256 B body + ~32 B
// scaffolding + 4 B framing).
#define PHONEAPI_FRAME_TX_MAX 320u
static bool frame_and_push(const uint8_t *payload, size_t payload_len)
{
    if (payload_len + 4u > PHONEAPI_FRAME_TX_MAX) return false;
    uint8_t frame[PHONEAPI_FRAME_TX_MAX];
    frame[0] = MAGIC1;
    frame[1] = MAGIC2;
    frame[2] = (uint8_t)((payload_len >> 8) & 0xFFu);
    frame[3] = (uint8_t)(payload_len & 0xFFu);
    memcpy(&frame[4], payload, payload_len);
    return phoneapi_tx_push(frame, payload_len + 4u);
}

bool phoneapi_encode_want_config_id(uint32_t nonce)
{
    // ToRadio.want_config_id = field 3, wire VARINT (0)
    uint8_t buf[16];
    size_t  pos = 0;
    if (!put_tag(buf, sizeof(buf), &pos, 3u, 0u)) return false;
    if (!put_varint(buf, sizeof(buf), &pos, nonce)) return false;
    return frame_and_push(buf, pos);
}

bool phoneapi_encode_heartbeat(void)
{
    // ToRadio.heartbeat = field 7, wire LEN (2). Heartbeat sub-message
    // is empty (zero-length LD payload).
    uint8_t buf[8];
    size_t  pos = 0;
    if (!put_tag(buf, sizeof(buf), &pos, 7u, 2u)) return false;
    if (!put_varint(buf, sizeof(buf), &pos, 0u)) return false;  // sub-msg length = 0
    return frame_and_push(buf, pos);
}

bool phoneapi_encode_disconnect(void)
{
    // ToRadio.disconnect = field 4, wire VARINT (0). Bool true.
    uint8_t buf[8];
    size_t  pos = 0;
    if (!put_tag(buf, sizeof(buf), &pos, 4u, 0u)) return false;
    if (!put_varint(buf, sizeof(buf), &pos, 1u)) return false;
    return frame_and_push(buf, pos);
}

// Helper: write a length-delimited field of known maximum size. We
// reserve `len_size` bytes for the length prefix so we can backfill
// after writing the body. For our scales (≤ 200 B text + small
// scaffolding) a single-byte length is rarely sufficient, so use 2.
static bool put_ld_with_2byte_len(uint8_t *dst, size_t cap, size_t *pos,
                                  uint32_t field_num,
                                  size_t (*body_writer)(uint8_t *, size_t, void *),
                                  void *ctx)
{
    if (!put_tag(dst, cap, pos, field_num, 2u)) return false;
    if (*pos + 2u > cap) return false;
    size_t len_pos = *pos;
    *pos += 2u;                              // reserve 2 bytes for varint length
    size_t body_start = *pos;
    size_t body_len   = body_writer(dst + body_start, cap - body_start, ctx);
    if (body_len == SIZE_MAX) return false;
    *pos = body_start + body_len;
    // Encode body_len as 2-byte varint (always 2 bytes for sizes < 16384).
    if (body_len > 16383u) {
        // body too long for 2-byte varint slot — would need to shift bytes
        return false;
    }
    dst[len_pos]     = (uint8_t)((body_len & 0x7Fu) | 0x80u);
    dst[len_pos + 1] = (uint8_t)((body_len >> 7) & 0x7Fu);
    return true;
}

typedef struct {
    uint32_t       portnum;
    const uint8_t *payload;
    uint16_t       payload_len;
    bool           want_response;     /* Data.want_response (field 3) */
} data_body_ctx_t;

static size_t write_data_body(uint8_t *dst, size_t cap, void *vctx)
{
    data_body_ctx_t *ctx = (data_body_ctx_t *)vctx;
    size_t pos = 0;
    // Data.portnum (field 1, varint)
    if (!put_tag(dst, cap, &pos, 1u, 0u)) return SIZE_MAX;
    if (!put_varint(dst, cap, &pos, ctx->portnum)) return SIZE_MAX;
    // Data.payload (field 2, LD). Always emit, even if zero-length —
    // empty bytes is a valid encoding (e.g. traceroute carries an
    // empty RouteDiscovery on initial send).
    if (!put_tag(dst, cap, &pos, 2u, 2u)) return SIZE_MAX;
    if (!put_varint(dst, cap, &pos, ctx->payload_len)) return SIZE_MAX;
    if (pos + ctx->payload_len > cap) return SIZE_MAX;
    if (ctx->payload_len > 0u && ctx->payload != NULL) {
        memcpy(dst + pos, ctx->payload, ctx->payload_len);
        pos += ctx->payload_len;
    }
    // Data.want_response (field 3, varint bool) — emit only when true
    if (ctx->want_response) {
        if (!put_tag(dst, cap, &pos, 3u, 0u)) return SIZE_MAX;
        if (!put_varint(dst, cap, &pos, 1u)) return SIZE_MAX;
    }
    return pos;
}

typedef struct {
    uint32_t to_node_id;
    uint8_t  channel_index;
    bool     want_ack;
    uint32_t packet_id;
    /* Sub-message contents */
    uint32_t       portnum;
    const uint8_t *payload;
    uint16_t       payload_len;
    bool           want_response;
} mesh_packet_ctx_t;

static size_t write_mesh_packet_body(uint8_t *dst, size_t cap, void *vctx)
{
    mesh_packet_ctx_t *ctx = (mesh_packet_ctx_t *)vctx;
    size_t pos = 0;
    // MeshPacket.to = field 2, fixed32
    if (!put_tag(dst, cap, &pos, 2u, 5u)) return SIZE_MAX;
    if (pos + 4u > cap) return SIZE_MAX;
    dst[pos++] = (uint8_t)(ctx->to_node_id & 0xFFu);
    dst[pos++] = (uint8_t)((ctx->to_node_id >> 8)  & 0xFFu);
    dst[pos++] = (uint8_t)((ctx->to_node_id >> 16) & 0xFFu);
    dst[pos++] = (uint8_t)((ctx->to_node_id >> 24) & 0xFFu);
    // MeshPacket.channel = field 3, varint
    if (!put_tag(dst, cap, &pos, 3u, 0u)) return SIZE_MAX;
    if (!put_varint(dst, cap, &pos, ctx->channel_index)) return SIZE_MAX;
    // MeshPacket.decoded = field 4, LD (Data sub-message)
    data_body_ctx_t data_ctx = {
        .portnum       = ctx->portnum,
        .payload       = ctx->payload,
        .payload_len   = ctx->payload_len,
        .want_response = ctx->want_response,
    };
    if (!put_ld_with_2byte_len(dst, cap, &pos, 4u, write_data_body, &data_ctx))
        return SIZE_MAX;
    // MeshPacket.id = field 6, FIXED32 (wire type 5) per mesh.proto.
    // Self-assigned so we can correlate the eventual Routing-app ACK /
    // QueueStatus back to this send. Core 0's Router honours a non-zero
    // host-supplied id. Encoding it as varint causes nanopb on Core 0
    // to reject the whole frame with "wrong wire type" (P0-1, 2026-04-29).
    if (!put_tag(dst, cap, &pos, 6u, 5u)) return SIZE_MAX;
    if (pos + 4u > cap) return SIZE_MAX;
    dst[pos++] = (uint8_t)(ctx->packet_id        & 0xFFu);
    dst[pos++] = (uint8_t)((ctx->packet_id >> 8)  & 0xFFu);
    dst[pos++] = (uint8_t)((ctx->packet_id >> 16) & 0xFFu);
    dst[pos++] = (uint8_t)((ctx->packet_id >> 24) & 0xFFu);
    // MeshPacket.want_ack = field 10, varint (bool)
    if (ctx->want_ack) {
        if (!put_tag(dst, cap, &pos, 10u, 0u)) return SIZE_MAX;
        if (!put_varint(dst, cap, &pos, 1u)) return SIZE_MAX;
    }
    return pos;
}

// Local packet_id allocator. Seeded from timer_hw->timerawl on the
// first call so two MokyaLora units don't collide in the air, and
// monotonically increments thereafter (skipping zero, which Meshtastic
// treats as "router, please assign").
static uint32_t next_packet_id(void)
{
    static uint32_t s_next = 0u;
    if (s_next == 0u) {
        s_next = timer_hw->timerawl;
        if (s_next == 0u) s_next = 1u;
    }
    uint32_t id = s_next++;
    if (s_next == 0u) s_next = 1u;
    return id;
}

bool phoneapi_encode_text_packet(uint32_t to_node_id,
                                  uint8_t  channel_index,
                                  bool     want_ack,
                                  const uint8_t *text,
                                  uint16_t text_len,
                                  uint32_t *out_packet_id)
{
    // Cap at a safe single-frame size: stream-protocol max payload is
    // 512 B, and the protobuf scaffolding around the text adds ~16 B.
    if (text == NULL && text_len > 0u) return false;
    if (text_len > 256u) text_len = 256u;

    uint32_t packet_id = next_packet_id();

    uint8_t buf[384];                        // ToRadio body buffer
    size_t  pos = 0;
    mesh_packet_ctx_t mp = {
        .to_node_id    = to_node_id,
        .channel_index = channel_index,
        .want_ack      = want_ack,
        .packet_id     = packet_id,
        .portnum       = 1u,                 // TEXT_MESSAGE_APP
        .payload       = text,
        .payload_len   = text_len,
        .want_response = false,              // text packets don't request app-level reply
    };
    // ToRadio.packet = field 1, LD (MeshPacket)
    if (!put_ld_with_2byte_len(buf, sizeof(buf), &pos, 1u,
                               write_mesh_packet_body, &mp)) {
        return false;
    }
    bool pushed = frame_and_push(buf, pos);
    TRACE("phapi", "tx_text",
          "to=%lu chan=%u ack=%u pid=%#lx body=%u pushed=%u",
          (unsigned long)to_node_id, (unsigned)channel_index,
          (unsigned)want_ack, (unsigned long)packet_id,
          (unsigned)pos, (unsigned)pushed);
    if (!pushed) return false;
    if (out_packet_id != NULL) {
        *out_packet_id = packet_id;
    }
    return true;
}

/* Helper that hides the buf + ToRadio scaffolding so additional
 * portnum-specific encoders stay short. Returns the resulting
 * packet_id via out param; pushes the framed bytes onto the c1→c0
 * SERIAL_BYTES ring under the cascade tx mutex. */
static bool encode_app_packet(uint32_t to_node_id,
                              uint8_t  channel_index,
                              uint32_t portnum,
                              const uint8_t *payload,
                              uint16_t payload_len,
                              bool     want_ack,
                              bool     want_response,
                              uint32_t *out_packet_id,
                              const char *trace_label)
{
    if (payload_len > 256u) payload_len = 256u;
    uint32_t packet_id = next_packet_id();
    uint8_t  buf[384];
    size_t   pos = 0;
    mesh_packet_ctx_t mp = {
        .to_node_id    = to_node_id,
        .channel_index = channel_index,
        .want_ack      = want_ack,
        .packet_id     = packet_id,
        .portnum       = portnum,
        .payload       = payload,
        .payload_len   = payload_len,
        .want_response = want_response,
    };
    if (!put_ld_with_2byte_len(buf, sizeof(buf), &pos, 1u,
                               write_mesh_packet_body, &mp)) {
        return false;
    }
    bool pushed = frame_and_push(buf, pos);
    TRACE("phapi", "tx_app",
          "label=%s to=%lu pn=%lu ack=%u wr=%u pid=%#lx body=%u pushed=%u",
          trace_label ? trace_label : "?",
          (unsigned long)to_node_id, (unsigned long)portnum,
          (unsigned)want_ack, (unsigned)want_response,
          (unsigned long)packet_id, (unsigned)pos, (unsigned)pushed);
    if (!pushed) return false;
    if (out_packet_id) *out_packet_id = packet_id;
    return true;
}

/* Traceroute (TRACEROUTE_APP = portnum 70).  Initial packet carries
 * an empty RouteDiscovery body; the destination fills it on reply.
 * want_response=true so the destination's TraceRouteModule sends a
 * response back. want_ack=false because the response itself stands
 * in for the ack. */
bool phoneapi_encode_traceroute(uint32_t to_node_id,
                                uint8_t  channel_index,
                                uint32_t *out_packet_id)
{
    return encode_app_packet(to_node_id, channel_index,
                             /*portnum=*/70u,
                             /*payload=*/NULL,
                             /*payload_len=*/0u,
                             /*want_ack=*/false,
                             /*want_response=*/true,
                             out_packet_id,
                             "traceroute");
}

/* Position request (POSITION_APP = portnum 3).  Empty Data.payload
 * with want_response=true triggers PositionModule on the peer to
 * reply with its current Position. */
bool phoneapi_encode_position_request(uint32_t to_node_id,
                                      uint8_t  channel_index,
                                      uint32_t *out_packet_id)
{
    return encode_app_packet(to_node_id, channel_index,
                             /*portnum=*/3u,
                             /*payload=*/NULL,
                             /*payload_len=*/0u,
                             /*want_ack=*/false,
                             /*want_response=*/true,
                             out_packet_id,
                             "pos_req");
}

/* P0-3 helper: build an AdminMessage payload containing one varint
 * field {field_num, value}. Returns the byte length written into
 * `dst`, or 0 on overflow. AdminMessage in admin.proto only ever
 * carries one oneof variant per packet, so a single field is the
 * whole body. */
static size_t encode_admin_single_varint(uint8_t *dst, size_t cap,
                                          uint32_t field_num, uint64_t value)
{
    size_t pos = 0;
    if (!put_tag(dst, cap, &pos, field_num, /*WT_VARINT=*/0u)) return 0u;
    if (!put_varint(dst, cap, &pos, value)) return 0u;
    return pos;
}

/* Common path for self-admin packets that carry one varint field. */
static bool send_self_admin_varint(uint32_t peer_node_num,
                                   uint32_t admin_field,
                                   const char *trace_label,
                                   uint32_t *out_packet_id)
{
    phoneapi_my_info_t mi;
    if (!phoneapi_cache_get_my_info(&mi) || mi.my_node_num == 0u) {
        return false;
    }
    /* KNOWN BUG (P0-3.1, 2026-04-29): cascade FromRadio.my_info ships
     * a *peer's* node_num in MyNodeInfo.my_node_num (set_my_info
     * RTT trace — num=2975709282 on a board where host `--info`
     * reports 1401875431). Bytes-on-wire genuinely carry the wrong
     * varint at field 1 — Core 0 Meshtastic-side dual-PhoneAPI-session
     * interference (the "PhoneAPI internal globals + dual-session
     * race" cited in CLAUDE.md). AdminMessage payload + local
     * AdminModule both verified working once MeshPacket.to is the
     * real node_num, so this single-board hardcode unblocks the C-3
     * favorite/ignore feature on the dev unit while a proper fix is
     * scoped (probably IPC_CMD_GET_MY_NODE_NUM on Core 0).            */
    TRACE("phapi", "self_admin_my_node",
          "cached=%lu (using hardcode for P0-3.1 bug)",
          (unsigned long)mi.my_node_num);
    mi.my_node_num = 1401875431u;  /* TNGBpicoC-ebe7-T (this dev unit) */
    /* AdminMessage body: one varint field, ≤ 7 bytes for the largest
     * field number we use (47/48). */
    uint8_t admin_body[8];
    size_t  body_len = encode_admin_single_varint(admin_body,
                                                   sizeof(admin_body),
                                                   admin_field,
                                                   peer_node_num);
    if (body_len == 0u) return false;

    /* Wrap as MeshPacket.decoded.payload, portnum 6 (ADMIN_APP). To
     * = self, so AdminModule on Core 0 picks it up locally. want_ack
     * = false (self-admin doesn't generate a routing-app ack), and
     * want_response = false (AdminMessage variants 39/40/47/48 do
     * not request a return-trip AdminMessage). */
    return encode_app_packet(mi.my_node_num,
                             /*channel_index=*/0u,
                             /*portnum=*/6u,
                             admin_body,
                             (uint16_t)body_len,
                             /*want_ack=*/false,
                             /*want_response=*/false,
                             out_packet_id,
                             trace_label);
}

bool phoneapi_encode_admin_set_favorite(uint32_t peer_node_num,
                                        bool     set,
                                        uint32_t *out_packet_id)
{
    if (peer_node_num == 0u) return false;
    /* admin.proto: set_favorite_node = 39, remove_favorite_node = 40 */
    return send_self_admin_varint(peer_node_num,
                                  set ? 39u : 40u,
                                  set ? "admin_set_fav" : "admin_clr_fav",
                                  out_packet_id);
}

bool phoneapi_encode_admin_set_ignored(uint32_t peer_node_num,
                                       bool     set,
                                       uint32_t *out_packet_id)
{
    if (peer_node_num == 0u) return false;
    /* admin.proto: set_ignored_node = 47, remove_ignored_node = 48 */
    return send_self_admin_varint(peer_node_num,
                                  set ? 47u : 48u,
                                  set ? "admin_set_ign" : "admin_clr_ign",
                                  out_packet_id);
}
