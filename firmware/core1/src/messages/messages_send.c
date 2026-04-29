/* messages_send.c — see messages_send.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "messages_send.h"

#include <string.h>

#include "phoneapi_encode.h"
#include "messages_tx_status.h"
#include "dm_store.h"
#include "global/status_bar.h"

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

    /* Phase 3: mirror the outbound message into dm_store FIRST so the
     * subsequent tx_status_publish(SENDING) finds an entry to update.
     * (ingest_outbound already initialises ack_state=SENDING, so the
     * publish is effectively a no-op for dm_store, but it keeps
     * messages_tx_status_get observers in sync.) Skip broadcasts —
     * they don't fit the per-peer thread model. */
    if (to_node_id != MESSAGES_SEND_BROADCAST) {
        dm_store_ingest_outbound(to_node_id, pid, want_ack, text, text_len);
    }

    /* Initial "sending" status — cascade ack handler updates to
     * delivered/failed when the corresponding FromRadio frame arrives. */
    messages_tx_status_publish(MESSAGES_TX_RESULT_SENDING, /*error=*/0u, pid);

    /* G-1 status bar: pulse TX activity indicator (250 ms). Cosmetic;
     * fires per cascade send so the user sees Mokya talking even when
     * the bubble's still grey "sending". */
    status_bar_pulse_tx();

    if (out_packet_id) *out_packet_id = pid;
    return true;
}
