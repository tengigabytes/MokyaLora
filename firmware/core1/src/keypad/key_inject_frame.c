/* key_inject_frame.c — parser state machine for MOKYA_KIJ wire frames.
 *
 * See key_inject_frame.h for the wire format. Parser resyncs on
 * MOKYA_KIJ_MAGIC0 after any protocol violation, so a host can safely
 * re-attach mid-stream.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "key_inject_frame.h"

enum {
    ST_MAGIC0 = 0,
    ST_MAGIC1,
    ST_TYPE,
    ST_LEN,
    ST_PAYLOAD,
    ST_CRC,
};

void mokya_kij_parser_reset(mokya_kij_parser_t *p)
{
    p->state = ST_MAGIC0;
    p->type = 0;
    p->len = 0;
    p->idx = 0;
}

void mokya_kij_parser_push(mokya_kij_parser_t *p,
                           const uint8_t *bytes, size_t n,
                           mokya_kij_frame_cb_t on_frame, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = bytes[i];
        switch (p->state) {
        case ST_MAGIC0:
            if (b == MOKYA_KIJ_MAGIC0) p->state = ST_MAGIC1;
            break;
        case ST_MAGIC1:
            if (b == MOKYA_KIJ_MAGIC1) p->state = ST_TYPE;
            else if (b == MOKYA_KIJ_MAGIC0) p->state = ST_MAGIC1;
            else p->state = ST_MAGIC0;
            break;
        case ST_TYPE:
            p->type = b;
            p->state = ST_LEN;
            break;
        case ST_LEN:
            p->len = b;
            if (b > MOKYA_KIJ_MAX_PAYLOAD) {
                /* Invalid length — resync. */
                p->state = ST_MAGIC0;
                break;
            }
            p->idx = 0;
            p->state = (b == 0) ? ST_CRC : ST_PAYLOAD;
            break;
        case ST_PAYLOAD:
            p->payload[p->idx++] = b;
            if (p->idx >= p->len) p->state = ST_CRC;
            break;
        case ST_CRC: {
            uint8_t buf[2 + MOKYA_KIJ_MAX_PAYLOAD];
            buf[0] = p->type;
            buf[1] = p->len;
            for (uint8_t k = 0; k < p->len; k++) buf[2 + k] = p->payload[k];
            if (mokya_kij_crc8(buf, (size_t)(2u + p->len)) == b) {
                on_frame(p->type, p->payload, p->len, ctx);
            }
            /* Whether CRC matched or not, reset for the next frame. */
            p->state = ST_MAGIC0;
            break;
        }
        default:
            p->state = ST_MAGIC0;
            break;
        }
    }
}
