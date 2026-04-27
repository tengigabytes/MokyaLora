/* messages_send.c — see messages_send.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "messages_send.h"

#include <string.h>

#ifdef MOKYA_PHONEAPI_CASCADE
#include "phoneapi_encode.h"
#else
#include "ipc_protocol.h"
#include "ipc_shared_layout.h"
#include "ipc_ringbuf.h"
#endif

static uint8_t s_seq;

bool messages_send_text(uint32_t to_node_id,
                        uint8_t  channel_index,
                        bool     want_ack,
                        const uint8_t *text,
                        uint16_t text_len,
                        uint8_t *out_seq)
{
    if (text_len > MESSAGES_SEND_TEXT_MAX) {
        text_len = MESSAGES_SEND_TEXT_MAX;
    }

    uint8_t seq = s_seq++;
    if (out_seq) *out_seq = seq;

#ifdef MOKYA_PHONEAPI_CASCADE
    /* Cascade path: build a ToRadio.packet directly and push framed
     * bytes into the c1→c0 SERIAL_BYTES ring under the TX mutex. Core
     * 0's PhoneAPI parses it just like any USB-host TO_RADIO message —
     * no IPC_CMD_SEND_TEXT stub, no IPC_MSG_TX_ACK back-channel
     * (acknowledgement arrives later as FromRadio.queueStatus or as
     * the next FromRadio.packet echoing our TX). */
    return phoneapi_encode_text_packet(to_node_id, channel_index, want_ack,
                                       text, text_len);
#else
    uint8_t buf[IPC_MSG_PAYLOAD_MAX];
    IpcPayloadText *out = (IpcPayloadText *)buf;

    const uint16_t header_size = (uint16_t)offsetof(IpcPayloadText, text);

    out->from_node_id  = 0u;             /* 0 = self; Core 0 fills in real id */
    out->to_node_id    = to_node_id;
    out->channel_index = channel_index;
    out->want_ack      = want_ack ? 1u : 0u;
    out->text_len      = text_len;
    if (text_len > 0 && text != NULL) {
        memcpy(out->text, text, text_len);
    }

    const uint16_t total = (uint16_t)(header_size + text_len);
    return ipc_ring_push(&g_ipc_shared.c1_to_c0_ctrl,
                         g_ipc_shared.c1_to_c0_slots,
                         IPC_RING_SLOT_COUNT,
                         IPC_CMD_SEND_TEXT,
                         seq,
                         buf,
                         total);
#endif
}
