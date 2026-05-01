/* dm_store.h — Per-peer DM ring backing the A-1/A-2 views.
 *
 * Sits alongside `phoneapi_msgs_*` (which is a 4-entry pooled FIFO useful
 * for the L-0 home preview). dm_store keeps a richer per-peer history so
 * conversation_view can render bubble threads without requiring a host
 * round-trip.
 *
 * Capacity (Phase 3 minimal):
 *   - DM_STORE_PEER_CAP    = 8 distinct peers
 *   - DM_STORE_MSGS_PER    = 8 messages per peer
 *   - DM_STORE_TEXT_MAX    = 200 bytes UTF-8 (matches phoneapi_msgs)
 *
 * Eviction: oldest peer by `last_activity_epoch` is dropped when a 9th
 * peer DMs. Within a peer, the message ring overwrites the oldest.
 *
 * Threading: writers + readers acquire the same FreeRTOS mutex. Reads
 * copy the requested record out, so callers don't need to keep the lock
 * across LVGL drawing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_DM_STORE_H
#define MOKYA_CORE1_DM_STORE_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DM_STORE_PEER_CAP   8u
#define DM_STORE_MSGS_PER   8u
#define DM_STORE_TEXT_MAX   200u

/* Outbound ack states. Map onto messages_tx_status results. */
typedef enum {
    DM_ACK_NONE      = 0,
    DM_ACK_SENDING   = 1,
    DM_ACK_DELIVERED = 2,
    DM_ACK_FAILED    = 3,
} dm_ack_state_t;

typedef struct {
    uint32_t seq;             ///< Monotonic id, matches phoneapi_msg seq
                              ///  for inbound or 0 for purely-local outbound
    uint32_t epoch;           ///< Phase 9b: UTC unix seconds (truncated u32);
                              ///  0 = wall_clock unsynced when stored
    uint32_t packet_id;       ///< MeshPacket.id (for outbound only)
    bool     outbound;
    uint8_t  ack_state;       ///< dm_ack_state_t
    uint16_t text_len;
    char     text[DM_STORE_TEXT_MAX];
    /* A3 — radio metadata. Appended at the end so existing on-air
     * struct offsets don't shift (SWD readers ignore the trailing
     * bytes). Inbound only for snr/rssi/hop_*; outbound only for
     * want_ack + ack_epoch. */
    uint32_t ack_epoch;       ///< Phase 9b: UTC unix seconds when ack landed
                              ///  (outbound only, 0 if pending or unsynced)
    int16_t  rx_snr_x4;       ///< inbound; INT16_MIN = unknown (dB × 4)
    int16_t  rx_rssi;         ///< inbound; 0 = unknown (dBm signed)
    uint8_t  hop_limit;       ///< inbound; 0xFF = unknown (hops still allowed at receive)
    uint8_t  hop_start;       ///< inbound; 0xFF = unknown (hops the sender set)
    uint8_t  want_ack;        ///< outbound; 0/1
    uint8_t  _pad;            ///< natural alignment to 4 B
} dm_msg_t;

/* Radio metadata pulled off MeshPacket envelope at decode time. */
typedef struct {
    int16_t  rx_snr_x4;       ///< INT16_MIN = unknown
    int16_t  rx_rssi;         ///< 0 = unknown
    uint8_t  hop_limit;       ///< 0xFF = unknown
    uint8_t  hop_start;       ///< 0xFF = unknown
} dm_msg_meta_t;

#define DM_MSG_META_UNKNOWN { INT16_MIN, 0, 0xFFu, 0xFFu }

typedef struct {
    uint32_t peer_node_id;
    uint32_t last_activity_ms;
    uint8_t  unread;
    uint8_t  count;            ///< 0..DM_STORE_MSGS_PER (current ring fill)
    uint8_t  head;             ///< next write index (modulo DM_STORE_MSGS_PER)
} dm_peer_summary_t;

/* Initialise. Idempotent; safe pre-scheduler (mutex created lazily by
 * the first writer call after scheduler start). */
void dm_store_init(void);

/* Inbound = the cascade saw a TEXT_MESSAGE_APP from `from_node_id`.
 * `meta` is optional (NULL = all-unknown). */
void dm_store_ingest_inbound(uint32_t  from_node_id,
                             uint32_t  seq,
                             const uint8_t *text,
                             uint16_t  text_len,
                             const dm_msg_meta_t *meta);

/* Outbound = we just pushed a SEND to the peer. `packet_id` is the local
 * MeshPacket.id assigned by messages_send_text. State starts as SENDING. */
void dm_store_ingest_outbound(uint32_t  to_node_id,
                              uint32_t  packet_id,
                              bool      want_ack,
                              const uint8_t *text,
                              uint16_t  text_len);

/* Update the latest matching outbound message's ack state. Called from
 * the messages_tx_status publisher. No-op if no peer has an outbound
 * message with this packet_id. */
void dm_store_update_ack(uint32_t packet_id, dm_ack_state_t state);

/* Peer enumeration ordered most-recent-activity first. */
uint32_t dm_store_peer_count(void);
bool     dm_store_peer_at(uint32_t index, dm_peer_summary_t *out);
bool     dm_store_get_peer(uint32_t peer_node_id, dm_peer_summary_t *out);

/* Read a message at offset `idx` (0 = oldest in ring) for `peer_node_id`.
 * Returns false if the peer is missing or idx is out of range. */
bool     dm_store_get_msg(uint32_t peer_node_id, uint8_t idx, dm_msg_t *out);

/* Mark all messages of a peer as read. */
void     dm_store_mark_read(uint32_t peer_node_id);

/* Aggregate unread across all peers. */
uint32_t dm_store_total_unread(void);

/* Monotonic change counter. Bumps on every ingest_inbound /
 * ingest_outbound / update_ack / mark_read. Views should remember the
 * last value seen and rebuild when it changes — covers all mutation
 * paths in one cheap atomic load. */
uint32_t dm_store_change_seq(void);

/* ── Persistence bridge (Phase 3 dm_persist) ──────────────────────── *
 *
 * Forward-declared opaque to avoid pulling dm_persist.h into every
 * dm_store.c caller. Defined in dm_persist.h. */
struct dm_persist_record;

/* Read-out a peer's full slot snapshot for persistence. Returns true
 * if peer exists in cache. Output mirrors peer_slot_t fields. */
bool dm_store_snapshot_peer(uint32_t peer_node_id,
                            struct dm_persist_record *out);

/* Bulk restore — used at boot from dm_persist_load_all. Allocates or
 * reuses a slot for `in->peer_node_id`, copies ring + counters in.
 * If table is full, evicts the oldest peer (same policy as live
 * inserts). */
bool dm_store_restore_peer(const struct dm_persist_record *in);

/* Pop all currently-dirty peer ids into `out_ids` (caller-provided
 * buffer, size `cap`). Clears the dirty bits for the popped peers.
 * Returns count actually written (≤ cap). dm_persist's flush daemon
 * uses this to drain the dirty queue under the dm_store mutex. */
uint8_t dm_store_pop_dirty(uint32_t *out_ids, uint8_t cap);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOKYA_CORE1_DM_STORE_H */
