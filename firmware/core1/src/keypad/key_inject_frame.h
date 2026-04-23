/* key_inject_frame.h — shared wire-frame definition for non-SWD key
 * injection transports (RTT down-channel, USB-CDC secondary interface).
 *
 * Frame layout (little-endian where multi-byte):
 *
 *     +----+----+------+------+--------------+------+
 *     | M0 | M1 | type | len  | payload[len] | crc8 |
 *     +----+----+------+------+--------------+------+
 *        2 B         1 B    1 B      len B       1 B
 *
 *   M0 M1  : fixed magic 0xAA 0x55. Distinguishes from ASCII noise on a
 *            hot-attached CDC, and lets a parser resync after dropped bytes.
 *   type   : frame type (see MOKYA_KIJ_TYPE_* below).
 *   len    : payload length in bytes; 0 is legal.
 *   payload: type-specific.
 *   crc8   : CRC-8/ITU (poly 0x07, init 0x00) over {type, len, payload}.
 *            Cheap, catches single-byte corruption and truncation.
 *
 * Transports live in key_inject_rtt.c (this header) and, future, a
 * second USB-CDC interface. Both parse into the same API
 * (key_event_push_inject_flags) that the SWD ring-buffer consumer uses,
 * so the three transports stay byte-for-byte equivalent from the IME's
 * point of view.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOKYA_KIJ_MAGIC0      0xAAu
#define MOKYA_KIJ_MAGIC1      0x55u
#define MOKYA_KIJ_MAX_PAYLOAD 16u  /* future-proofing; keep parser small */

/* Frame types. */
enum {
    MOKYA_KIJ_TYPE_KEY_EVENT   = 0x01u,  /* payload: key_byte, flags_byte */
    MOKYA_KIJ_TYPE_FORCE_SAVE  = 0x02u,  /* payload: (empty) */
    MOKYA_KIJ_TYPE_NOP         = 0x03u,  /* payload: (empty) — keepalive  */
};

/* CRC-8/ITU (poly 0x07, init 0x00). Static inline so both fw and any
 * host test that compiles against this header share one definition. */
static inline uint8_t mokya_kij_crc8(const uint8_t *data, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u)
                                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* Minimum + maximum on-wire frame size (header 4 + crc 1 + payload 0..MAX). */
#define MOKYA_KIJ_FRAME_MIN   5u
#define MOKYA_KIJ_FRAME_MAX   (5u + MOKYA_KIJ_MAX_PAYLOAD)

/* Parser state machine. Hosts call mokya_kij_parser_reset() once, then
 * feed received bytes into mokya_kij_parser_push() as they arrive; the
 * parser invokes `on_frame(type, payload, payload_len, ctx)` for every
 * validated frame. Bad magic / bad CRC / truncated frames are dropped
 * silently and the parser resyncs on the next MOKYA_KIJ_MAGIC0. */
typedef void (*mokya_kij_frame_cb_t)(uint8_t type,
                                     const uint8_t *payload,
                                     uint8_t payload_len,
                                     void *ctx);

typedef struct {
    uint8_t state;                       /* internal */
    uint8_t type;
    uint8_t len;
    uint8_t idx;
    uint8_t payload[MOKYA_KIJ_MAX_PAYLOAD];
} mokya_kij_parser_t;

void mokya_kij_parser_reset(mokya_kij_parser_t *p);
void mokya_kij_parser_push(mokya_kij_parser_t *p,
                           const uint8_t *bytes, size_t n,
                           mokya_kij_frame_cb_t on_frame, void *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif
