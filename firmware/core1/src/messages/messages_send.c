/* messages_send.c — see messages_send.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "messages_send.h"

#include <string.h>

#include "phoneapi_encode.h"
#include "messages_tx_status.h"

bool messages_send_text(uint32_t to_node_id,
                        uint8_t  channel_index,
                        bool     want_ack,
                        const uint8_t *text,
                        uint16_t text_len,
                        uint32_t *out_packet_id)
{
    if (text_len > MESSAGES_SEND_TEXT_MAX) {
        text_len = MESSAGES_SEND_TEXT_MAX;
    }

    /* Cascade path: build a ToRadio.packet directly and push framed
     * bytes into the c1→c0 SERIAL_BYTES ring under the TX mutex. Core
     * 0's PhoneAPI parses it just like any USB-host TO_RADIO message.
     * Acknowledgement arrives later as FromRadio.queue_status or as
     * a FromRadio.packet on portnum=ROUTING_APP carrying request_id. */
    uint32_t pid = 0u;
    bool ok = phoneapi_encode_text_packet(to_node_id, channel_index, want_ack,
                                          text, text_len, &pid);
    if (!ok) return false;

    /* Initial "sending" status — cascade ack handler updates to
     * delivered/failed when the corresponding FromRadio frame arrives. */
    messages_tx_status_publish(MESSAGES_TX_RESULT_SENDING, /*error=*/0u, pid);

    if (out_packet_id) *out_packet_id = pid;
    return true;
}
