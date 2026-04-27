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

// Wrap `payload[len]` with stream-protocol header and push.
static bool frame_and_push(const uint8_t *payload, size_t payload_len)
{
    if (payload_len > 0xFFFFu) return false;
    uint8_t frame[4 + 64];
    if (payload_len + 4u > sizeof(frame)) return false;
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
