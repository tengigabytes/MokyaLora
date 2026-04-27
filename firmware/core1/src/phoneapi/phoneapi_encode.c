// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors

#include "phoneapi_encode.h"

#include <string.h>

#include "phoneapi_tx.h"

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
    const uint8_t *text;
    uint16_t       text_len;
} data_body_ctx_t;

static size_t write_data_body(uint8_t *dst, size_t cap, void *vctx)
{
    data_body_ctx_t *ctx = (data_body_ctx_t *)vctx;
    size_t pos = 0;
    if (!put_tag(dst, cap, &pos, 1u, 0u)) return SIZE_MAX;       // Data.portnum
    if (!put_varint(dst, cap, &pos, ctx->portnum)) return SIZE_MAX;
    if (!put_tag(dst, cap, &pos, 2u, 2u)) return SIZE_MAX;       // Data.payload
    if (!put_varint(dst, cap, &pos, ctx->text_len)) return SIZE_MAX;
    if (pos + ctx->text_len > cap) return SIZE_MAX;
    if (ctx->text_len > 0u) {
        memcpy(dst + pos, ctx->text, ctx->text_len);
        pos += ctx->text_len;
    }
    return pos;
}

typedef struct {
    uint32_t to_node_id;
    uint8_t  channel_index;
    bool     want_ack;
    const uint8_t *text;
    uint16_t text_len;
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
        .portnum  = 1u,                       // TEXT_MESSAGE_APP
        .text     = ctx->text,
        .text_len = ctx->text_len,
    };
    if (!put_ld_with_2byte_len(dst, cap, &pos, 4u, write_data_body, &data_ctx))
        return SIZE_MAX;
    // MeshPacket.want_ack = field 10, varint (bool)
    if (ctx->want_ack) {
        if (!put_tag(dst, cap, &pos, 10u, 0u)) return SIZE_MAX;
        if (!put_varint(dst, cap, &pos, 1u)) return SIZE_MAX;
    }
    return pos;
}

bool phoneapi_encode_text_packet(uint32_t to_node_id,
                                  uint8_t  channel_index,
                                  bool     want_ack,
                                  const uint8_t *text,
                                  uint16_t text_len)
{
    // Cap at a safe single-frame size: stream-protocol max payload is
    // 512 B, and the protobuf scaffolding around the text adds ~16 B.
    if (text == NULL && text_len > 0u) return false;
    if (text_len > 256u) text_len = 256u;

    uint8_t buf[384];                        // ToRadio body buffer
    size_t  pos = 0;
    mesh_packet_ctx_t mp = {
        .to_node_id    = to_node_id,
        .channel_index = channel_index,
        .want_ack      = want_ack,
        .text          = text,
        .text_len      = text_len,
    };
    // ToRadio.packet = field 1, LD (MeshPacket)
    if (!put_ld_with_2byte_len(buf, sizeof(buf), &pos, 1u,
                               write_mesh_packet_body, &mp)) {
        return false;
    }
    return frame_and_push(buf, pos);
}
