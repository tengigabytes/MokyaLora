// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors

#include "phoneapi_framing.h"

#include <string.h>

#define MAGIC1 0x94u
#define MAGIC2 0xC3u

void phoneapi_framing_init(phoneapi_framing_t *ctx,
                           phoneapi_frame_cb_t on_frame,
                           void *user)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state    = PHONEAPI_FRAMING_IDLE;
    ctx->on_frame = on_frame;
    ctx->user     = user;
}

void phoneapi_framing_push(phoneapi_framing_t *ctx,
                           const uint8_t *buf,
                           size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = buf[i];

        switch (ctx->state) {
        case PHONEAPI_FRAMING_IDLE:
            if (b == MAGIC1) {
                ctx->state = PHONEAPI_FRAMING_MAGIC2;
            }
            // any other byte: drop silently (idle resync)
            break;

        case PHONEAPI_FRAMING_MAGIC2:
            if (b == MAGIC2) {
                ctx->state = PHONEAPI_FRAMING_LEN_HI;
            } else if (b == MAGIC1) {
                // 0x94 0x94 — stay primed for MAGIC2
                ctx->state = PHONEAPI_FRAMING_MAGIC2;
            } else {
                ctx->state = PHONEAPI_FRAMING_IDLE;
                ctx->resync_drops++;
            }
            break;

        case PHONEAPI_FRAMING_LEN_HI:
            ctx->expected_len = ((uint16_t)b) << 8;
            ctx->state        = PHONEAPI_FRAMING_LEN_LO;
            break;

        case PHONEAPI_FRAMING_LEN_LO:
            ctx->expected_len |= (uint16_t)b;
            ctx->received_len  = 0;
            if (ctx->expected_len == 0u) {
                // zero-length frame — fire callback with empty payload, no
                // PhoneAPI message known to use this but be defensive
                if (ctx->on_frame) {
                    ctx->on_frame(ctx->buf, 0, ctx->user);
                }
                ctx->frames_ok++;
                ctx->state = PHONEAPI_FRAMING_IDLE;
            } else if (ctx->expected_len > PHONEAPI_FRAME_MAX) {
                // oversized — protocol max is 512; resync
                ctx->frames_oversized++;
                ctx->state = PHONEAPI_FRAMING_IDLE;
            } else {
                ctx->state = PHONEAPI_FRAMING_PAYLOAD;
            }
            break;

        case PHONEAPI_FRAMING_PAYLOAD: {
            // Bulk-copy as many bytes as possible without re-entering the loop
            uint16_t want      = ctx->expected_len - ctx->received_len;
            size_t   remaining = len - i;
            size_t   take      = (remaining < want) ? remaining : want;
            memcpy(&ctx->buf[ctx->received_len], &buf[i], take);
            ctx->received_len += (uint16_t)take;
            i                 += take - 1;  // -1 because for-loop will ++

            if (ctx->received_len >= ctx->expected_len) {
                if (ctx->on_frame) {
                    ctx->on_frame(ctx->buf, ctx->expected_len, ctx->user);
                }
                ctx->frames_ok++;
                ctx->state = PHONEAPI_FRAMING_IDLE;
            }
            break;
        }
        }
    }
}
