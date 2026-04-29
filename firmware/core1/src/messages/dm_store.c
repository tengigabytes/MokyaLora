/* dm_store.c — see dm_store.h.
 *
 * Per-peer fixed-size ring with mutex-protected reads. Peer slots are
 * allocated linearly; eviction picks the oldest by last_activity.
 *
 * Footprint: 8 peers × (16 metadata + 8 × 240 msg) ≈ 16 KB BSS. Sits
 * comfortably in PSRAM if BSS budget tightens later — but Phase 3 keeps
 * it in SRAM for SWD inspectability.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "dm_store.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "mokya_trace.h"

/* ── Peer table ──────────────────────────────────────────────────────── */

/* On-air struct sizes (post-A3 — verified via `arm-none-eabi-nm --print-size`):
 *
 *   sizeof(dm_msg_t)    = 232 B
 *     u32 seq + u32 epoch + u32 packet_id    = 12
 *     bool outbound + u8 ack_state +
 *       u16 text_len                          = 4   (no padding — already aligned)
 *     char text[200]                          = 200
 *     u32 ack_epoch                           = 4
 *     i16 rx_snr_x4 + i16 rx_rssi             = 4
 *     u8 hop_limit + u8 hop_start +
 *       u8 want_ack + u8 _pad                 = 4
 *
 *   sizeof(peer_slot_t) = 1872 B (8 × 232 + 16 metadata)
 *     bool in_use + 3 B pad                   = 4
 *     u32 peer_node_id + u32 last_activity_ms = 8
 *     u8 unread + u8 count + u8 head + 1 pad  = 4
 *     dm_msg_t ring[8] = 8 × 232              = 1856
 *
 *   sizeof(s_peers)     = 8 × 1872 = 14 976 B (~ +1 KB vs pre-A3)
 *
 * SWD readers walking ring[] should advance by 232 B per slot. */
typedef struct {
    bool     in_use;
    uint32_t peer_node_id;
    uint32_t last_activity_ms;
    uint8_t  unread;
    uint8_t  count;
    uint8_t  head;          /* next write slot (mod DM_STORE_MSGS_PER) */
    dm_msg_t ring[DM_STORE_MSGS_PER];
} peer_slot_t;

/* Peer table lives in PSRAM (.psram_bss at 0x11700000+) to keep ~15 KB
 * out of the tight 56 KB FreeRTOS heap region. Mutex stays in SRAM —
 * FreeRTOS objects must be in normal RAM. SWD readers should expect
 * cached PSRAM (per project_psram_swd_cache_coherence). */
static peer_slot_t       s_peers[DM_STORE_PEER_CAP] __attribute__((section(".psram_bss")));
static SemaphoreHandle_t s_mutex;
static uint32_t          s_next_local_seq;
static volatile uint32_t s_change_seq;

static inline void bump_change_seq(void)
{
    __atomic_add_fetch(&s_change_seq, 1u, __ATOMIC_RELEASE);
}

/* ── Locking helpers ─────────────────────────────────────────────────── */

static bool ensure_mutex(void)
{
    if (s_mutex != NULL) return true;
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL;
}

static bool lock(void)
{
    if (!ensure_mutex()) return false;
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
}

static void unlock(void)
{
    if (s_mutex) xSemaphoreGive(s_mutex);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ── Peer slot allocation ────────────────────────────────────────────── */

static int find_peer_unlocked(uint32_t peer_node_id)
{
    for (int i = 0; i < (int)DM_STORE_PEER_CAP; ++i) {
        if (s_peers[i].in_use && s_peers[i].peer_node_id == peer_node_id) {
            return i;
        }
    }
    return -1;
}

static int alloc_peer_unlocked(uint32_t peer_node_id)
{
    /* Reuse existing? */
    int idx = find_peer_unlocked(peer_node_id);
    if (idx >= 0) return idx;
    /* Free slot? */
    for (int i = 0; i < (int)DM_STORE_PEER_CAP; ++i) {
        if (!s_peers[i].in_use) {
            memset(&s_peers[i], 0, sizeof(s_peers[i]));
            s_peers[i].in_use       = true;
            s_peers[i].peer_node_id = peer_node_id;
            return i;
        }
    }
    /* Evict the oldest. */
    int      victim = 0;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < (int)DM_STORE_PEER_CAP; ++i) {
        if (s_peers[i].last_activity_ms < oldest) {
            oldest = s_peers[i].last_activity_ms;
            victim = i;
        }
    }
    memset(&s_peers[victim], 0, sizeof(s_peers[victim]));
    s_peers[victim].in_use       = true;
    s_peers[victim].peer_node_id = peer_node_id;
    return victim;
}

static void push_msg_unlocked(peer_slot_t *p, const dm_msg_t *m)
{
    p->ring[p->head] = *m;
    p->head = (uint8_t)((p->head + 1u) % DM_STORE_MSGS_PER);
    if (p->count < DM_STORE_MSGS_PER) p->count++;
    p->last_activity_ms = m->epoch;
}

/* Translate caller-facing "0 = oldest" index into ring offset. */
static int msg_index_unlocked(const peer_slot_t *p, uint8_t idx)
{
    if (idx >= p->count) return -1;
    /* head points to next-write slot; oldest = (head - count) mod cap. */
    int oldest = (int)p->head - (int)p->count;
    if (oldest < 0) oldest += DM_STORE_MSGS_PER;
    return (oldest + idx) % DM_STORE_MSGS_PER;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void dm_store_init(void)
{
    memset(s_peers, 0, sizeof(s_peers));
    s_next_local_seq = 1u;
    /* mutex created lazily on first lock attempt */
}

void dm_store_ingest_inbound(uint32_t  from_node_id,
                             uint32_t  seq,
                             const uint8_t *text,
                             uint16_t  text_len,
                             const dm_msg_meta_t *meta)
{
    if (from_node_id == 0u || text == NULL || text_len == 0u) return;
    if (!lock()) return;
    int idx = alloc_peer_unlocked(from_node_id);
    peer_slot_t *p = &s_peers[idx];
    dm_msg_t m;
    memset(&m, 0, sizeof(m));
    m.seq       = seq;
    m.epoch     = now_ms();
    m.outbound  = false;
    m.ack_state = DM_ACK_NONE;
    m.text_len  = text_len > DM_STORE_TEXT_MAX
                  ? (uint16_t)DM_STORE_TEXT_MAX : text_len;
    memcpy(m.text, text, m.text_len);
    if (meta != NULL) {
        m.rx_snr_x4 = meta->rx_snr_x4;
        m.rx_rssi   = meta->rx_rssi;
        m.hop_limit = meta->hop_limit;
        m.hop_start = meta->hop_start;
    } else {
        m.rx_snr_x4 = INT16_MIN;
        m.rx_rssi   = 0;
        m.hop_limit = 0xFFu;
        m.hop_start = 0xFFu;
    }
    push_msg_unlocked(p, &m);
    if (p->unread < 0xFFu) p->unread++;
    unlock();
    bump_change_seq();
}

void dm_store_ingest_outbound(uint32_t  to_node_id,
                              uint32_t  packet_id,
                              bool      want_ack,
                              const uint8_t *text,
                              uint16_t  text_len)
{
    if (to_node_id == 0u || text == NULL || text_len == 0u) return;
    if (!lock()) return;
    int idx = alloc_peer_unlocked(to_node_id);
    peer_slot_t *p = &s_peers[idx];
    dm_msg_t m;
    memset(&m, 0, sizeof(m));
    m.seq       = s_next_local_seq++;
    m.epoch     = now_ms();
    m.packet_id = packet_id;
    m.outbound  = true;
    m.ack_state = DM_ACK_SENDING;
    m.want_ack  = want_ack ? 1u : 0u;
    m.rx_snr_x4 = INT16_MIN;   /* not applicable to outbound */
    m.hop_limit = 0xFFu;
    m.hop_start = 0xFFu;
    m.text_len  = text_len > DM_STORE_TEXT_MAX
                  ? (uint16_t)DM_STORE_TEXT_MAX : text_len;
    memcpy(m.text, text, m.text_len);
    push_msg_unlocked(p, &m);
    unlock();
    bump_change_seq();
}

void dm_store_update_ack(uint32_t packet_id, dm_ack_state_t state)
{
    if (packet_id == 0u) return;
    if (!lock()) return;
    /* Linear scan — small fixed bound. Update the most recent matching
     * outbound message. */
    bool changed = false;
    int  matches = 0;
    uint32_t matched_peer = 0u;
    uint32_t now = now_ms();
    for (int i = 0; i < (int)DM_STORE_PEER_CAP; ++i) {
        peer_slot_t *p = &s_peers[i];
        if (!p->in_use) continue;
        for (int j = 0; j < (int)DM_STORE_MSGS_PER; ++j) {
            if (p->ring[j].outbound && p->ring[j].packet_id == packet_id) {
                if (p->ring[j].ack_state != (uint8_t)state) changed = true;
                p->ring[j].ack_state = (uint8_t)state;
                /* Stamp ack_epoch only on the first transition into a
                 * terminal state so we keep the original arrival time
                 * even if a duplicate ack lands later. */
                if (p->ring[j].ack_epoch == 0u &&
                    (state == DM_ACK_DELIVERED || state == DM_ACK_FAILED)) {
                    p->ring[j].ack_epoch = now;
                }
                matches++;
                matched_peer = p->peer_node_id;
            }
        }
    }
    unlock();
    TRACE("dm", "update_ack",
          "pid=%#lx state=%u matches=%u peer=%lu changed=%u",
          (unsigned long)packet_id, (unsigned)state, (unsigned)matches,
          (unsigned long)matched_peer, (unsigned)changed);
    if (changed) bump_change_seq();
}

uint32_t dm_store_peer_count(void)
{
    if (!lock()) return 0;
    uint32_t n = 0;
    for (int i = 0; i < (int)DM_STORE_PEER_CAP; ++i) {
        if (s_peers[i].in_use) n++;
    }
    unlock();
    return n;
}

bool dm_store_peer_at(uint32_t index, dm_peer_summary_t *out)
{
    if (out == NULL) return false;
    if (!lock()) return false;
    /* Build a sorted (by recency desc) view of the in-use slots. */
    int order[DM_STORE_PEER_CAP];
    int n = 0;
    for (int i = 0; i < (int)DM_STORE_PEER_CAP; ++i) {
        if (s_peers[i].in_use) order[n++] = i;
    }
    /* Insertion sort by last_activity_ms desc — small N so trivial. */
    for (int i = 1; i < n; ++i) {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 &&
               s_peers[order[j]].last_activity_ms <
               s_peers[key].last_activity_ms) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = key;
    }
    if (index >= (uint32_t)n) {
        unlock();
        return false;
    }
    peer_slot_t *p = &s_peers[order[index]];
    out->peer_node_id     = p->peer_node_id;
    out->last_activity_ms = p->last_activity_ms;
    out->unread           = p->unread;
    out->count            = p->count;
    out->head             = p->head;
    unlock();
    return true;
}

bool dm_store_get_peer(uint32_t peer_node_id, dm_peer_summary_t *out)
{
    if (out == NULL) return false;
    if (!lock()) return false;
    int idx = find_peer_unlocked(peer_node_id);
    bool ok = (idx >= 0);
    if (ok) {
        peer_slot_t *p = &s_peers[idx];
        out->peer_node_id     = p->peer_node_id;
        out->last_activity_ms = p->last_activity_ms;
        out->unread           = p->unread;
        out->count            = p->count;
        out->head             = p->head;
    }
    unlock();
    return ok;
}

bool dm_store_get_msg(uint32_t peer_node_id, uint8_t idx, dm_msg_t *out)
{
    if (out == NULL) return false;
    if (!lock()) return false;
    int slot = find_peer_unlocked(peer_node_id);
    if (slot < 0) { unlock(); return false; }
    peer_slot_t *p = &s_peers[slot];
    int ring_idx = msg_index_unlocked(p, idx);
    if (ring_idx < 0) { unlock(); return false; }
    *out = p->ring[ring_idx];
    unlock();
    return true;
}

void dm_store_mark_read(uint32_t peer_node_id)
{
    bool changed = false;
    if (!lock()) return;
    int idx = find_peer_unlocked(peer_node_id);
    if (idx >= 0 && s_peers[idx].unread != 0) {
        s_peers[idx].unread = 0;
        changed = true;
    }
    unlock();
    if (changed) bump_change_seq();
}

uint32_t dm_store_total_unread(void)
{
    if (!lock()) return 0;
    uint32_t n = 0;
    for (int i = 0; i < (int)DM_STORE_PEER_CAP; ++i) {
        if (s_peers[i].in_use) n += s_peers[i].unread;
    }
    unlock();
    return n;
}

uint32_t dm_store_change_seq(void)
{
    return __atomic_load_n(&s_change_seq, __ATOMIC_ACQUIRE);
}
