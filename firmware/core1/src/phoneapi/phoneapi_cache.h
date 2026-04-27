// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors
//
// In-memory cache of PhoneAPI state on Core 1.
//
// Phase B scope: my_info, channels, NodeDB, metadata. Config and
// ModuleConfig are tracked at oneof-tag level only (deferred to Phase
// F). Packet decoding (TEXT messages → messages cache) lands in Phase
// D.
//
// Concurrency model: one writer (`bridge_task` via `phoneapi_session`)
// and many readers (LVGL views). All public read APIs return copies
// taken under a FreeRTOS mutex. Writers also take the mutex around
// each commit. Lock granularity is per-API-call so a refresh tick
// never holds the mutex across LVGL drawing.
//
// Risk R1 (mid-stream interruption from concurrent want_config_id):
// `last_committed_seq` is bumped only on `config_complete_id`. The
// NodeDB has a `phase_seq` per entry — at start of a new
// configuration phase (signalled by `my_info`) the cache enters
// rebuild mode; entries written during rebuild carry the new
// phase_seq, and on `config_complete_id` the older entries are
// evicted. This avoids a full shadow buffer (no Core 1 RAM doubling)
// while still providing atomic-feeling reads.

#ifndef MOKYA_PHONEAPI_CACHE_H
#define MOKYA_PHONEAPI_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PHONEAPI_MSG_TEXT_MAX    200u
#define PHONEAPI_MSG_RING_CAP      4u
#define PHONEAPI_PIO_ENV_MAX     32u   // null-terminated
#define PHONEAPI_DEVICE_ID_MAX   16u   // bytes (raw, no NUL)
#define PHONEAPI_FW_VERSION_MAX  32u   // null-terminated
#define PHONEAPI_LONG_NAME_MAX   40u   // null-terminated
#define PHONEAPI_SHORT_NAME_MAX  8u    // null-terminated (proto says ~2 chars)
#define PHONEAPI_CHANNEL_NAME_MAX 12u  // null-terminated (proto says <12 bytes)
#define PHONEAPI_CHANNEL_COUNT   8u
#define PHONEAPI_NODES_CAP       32u

// MyNodeInfo subset
typedef struct {
    uint32_t my_node_num;
    uint32_t reboot_count;
    uint32_t min_app_version;
    uint32_t nodedb_count;
    uint8_t  firmware_edition;       // FirmwareEdition enum
    uint8_t  device_id_len;
    uint8_t  device_id[PHONEAPI_DEVICE_ID_MAX];
    char     pio_env[PHONEAPI_PIO_ENV_MAX];
} phoneapi_my_info_t;

// DeviceMetadata subset
typedef struct {
    uint32_t device_state_version;
    bool     can_shutdown;
    bool     has_wifi;
    bool     has_bluetooth;
    bool     has_ethernet;
    uint8_t  role;                   // Config.DeviceConfig.Role enum
    char     firmware_version[PHONEAPI_FW_VERSION_MAX];
} phoneapi_metadata_t;

typedef enum {
    PHONEAPI_CHAN_ROLE_DISABLED  = 0,
    PHONEAPI_CHAN_ROLE_PRIMARY   = 1,
    PHONEAPI_CHAN_ROLE_SECONDARY = 2,
} phoneapi_channel_role_t;

typedef struct {
    bool     in_use;
    uint8_t  index;
    uint8_t  role;                   // phoneapi_channel_role_t
    uint8_t  psk_len;                // 0 / 1 / 16 / 32
    uint32_t channel_id;             // fixed32 from settings.id
    char     name[PHONEAPI_CHANNEL_NAME_MAX];
} phoneapi_channel_t;

// NodeInfo subset (one entry per peer in the mesh)
typedef struct {
    bool     in_use;
    uint32_t num;
    char     long_name[PHONEAPI_LONG_NAME_MAX];
    char     short_name[PHONEAPI_SHORT_NAME_MAX];
    uint8_t  hw_model;
    uint8_t  role;
    bool     via_mqtt;
    bool     is_favorite;
    bool     is_unmessagable;
    uint8_t  channel;                // local channel index
    uint8_t  hops_away;              // 0xFF if unset
    int32_t  snr_x100;               // SNR (dB) × 100; INT32_MIN if unset
    uint32_t last_heard;             // fixed32 epoch (seconds)
    // DeviceMetrics summary — 0xFF / INT16_MIN / 0 if unset
    uint8_t  battery_level;
    int16_t  voltage_mv;
    uint8_t  channel_util_pct;
    uint8_t  air_util_tx_pct;
    uint32_t uptime_seconds;
    // Bookkeeping
    uint32_t phase_seq;              // matches cache.current_phase_seq if fresh
} phoneapi_node_t;

// Decoded TEXT_MESSAGE_APP payload — published by the cascade decoder
// when a FromRadio.packet with portnum==TEXT_MESSAGE_APP is seen.
// Field shape matches `messages_inbox_entry_t` so messages_view can be
// migrated with minimal code change.
typedef struct {
    uint32_t seq;            ///< Monotonic id assigned at publish
    uint32_t from_node_id;
    uint32_t to_node_id;
    uint8_t  channel_index;
    uint16_t text_len;
    uint8_t  text[PHONEAPI_MSG_TEXT_MAX];
} phoneapi_text_msg_t;

// Public API ---------------------------------------------------------

void phoneapi_cache_init(void);

// Bump phase_seq — called when `my_info` arrives (start of a new
// want_config_id sequence). Existing nodes are not yet evicted; they
// only become eligible for eviction once `phoneapi_cache_commit()` is
// called on `config_complete_id`.
void phoneapi_cache_phase_begin(void);

// Atomically mark the current phase complete: bump committed_seq, evict
// nodes whose phase_seq is older than the current phase. Called when
// FromRadio.config_complete_id arrives.
void phoneapi_cache_commit(uint32_t complete_id);

// Writers — called from the framing on-frame callback after decoding.
void phoneapi_cache_set_my_info(const phoneapi_my_info_t *info);
void phoneapi_cache_set_metadata(const phoneapi_metadata_t *meta);
void phoneapi_cache_set_channel(uint8_t index, const phoneapi_channel_t *chan);
// node->phase_seq is set internally; caller fills the rest.
void phoneapi_cache_upsert_node(const phoneapi_node_t *node);

// Readers — copy out under the mutex.
bool phoneapi_cache_get_my_info(phoneapi_my_info_t *out);
bool phoneapi_cache_get_metadata(phoneapi_metadata_t *out);
bool phoneapi_cache_get_channel(uint8_t index, phoneapi_channel_t *out);
uint32_t phoneapi_cache_node_count(void);
// Copy node by relative index (0..count-1, ordered most-recent first).
bool phoneapi_cache_take_node_at(uint32_t index, phoneapi_node_t *out);
// Copy node by absolute node_id; returns false if not present.
bool phoneapi_cache_get_node_by_id(uint32_t node_id, phoneapi_node_t *out);

uint32_t phoneapi_cache_change_seq(void);
uint32_t phoneapi_cache_committed_seq(void);
bool     phoneapi_cache_config_complete(void);

// Inbound text-message ring (FIFO of last PHONEAPI_MSG_RING_CAP).
// Producer = phoneapi_session decoder; consumer = messages_view.
void     phoneapi_msgs_publish(uint32_t from_node_id,
                                uint32_t to_node_id,
                                uint8_t  channel_index,
                                const uint8_t *text,
                                uint16_t text_len);
uint32_t phoneapi_msgs_count(void);
uint32_t phoneapi_msgs_latest_seq(void);
// `offset` 0 = newest, 1 = next-newest. Returns false if offset >= count.
bool     phoneapi_msgs_take_at_offset(uint32_t offset, phoneapi_text_msg_t *out);

#ifdef __cplusplus
}
#endif

#endif  // MOKYA_PHONEAPI_CACHE_H
