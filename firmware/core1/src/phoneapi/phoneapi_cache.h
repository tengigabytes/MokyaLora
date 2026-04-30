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
    // ChannelSettings.module_settings (B3-P2)
    bool     has_module_settings;
    uint32_t module_position_precision;
    bool     module_is_muted;
} phoneapi_channel_t;

// Last RouteDiscovery (TRACEROUTE_APP, portnum 70) reply seen for a peer.
// Stored per-node so node_detail_view can render hop counts + per-hop
// SNR after the user issues a C-3 OP_TRACEROUTE.
//
// Meshtastic RouteDiscovery (mesh.proto): up to 8 hops + 8 SNRs per
// direction in the wire format; we cap at 4 each to keep per-node
// memory bounded — a 5+ hop traceroute reply truncates to the first 4
// in the listed direction. SNRs are int32 × 4 (dB×4); we saturate to
// int8 to keep storage tight.
#define PHONEAPI_ROUTE_HOPS_MAX  4u

typedef struct {
    uint8_t  hop_count;                          // 0..4, 0 = no data
    uint8_t  hops_back_count;                    // 0..4, 0 = no return path
    uint32_t hops_full[PHONEAPI_ROUTE_HOPS_MAX]; // forward route node_num
    uint32_t hops_back_full[PHONEAPI_ROUTE_HOPS_MAX];
    int8_t   snr_fwd[PHONEAPI_ROUTE_HOPS_MAX];   // dB × 4, INT8_MIN if unknown
    int8_t   snr_back[PHONEAPI_ROUTE_HOPS_MAX];
    uint32_t epoch;                              // last_heard epoch of this reply
} phoneapi_last_route_t;

// Last POSITION_APP (portnum 3) reply seen for a peer. v1 surfaces the
// four most useful fields; PDOP / sats / precision_bits / source are
// not displayed in the C-2 detail view.
typedef struct {
    int32_t  lat_e7;                             // latitude × 1e7 (sint32)
    int32_t  lon_e7;                             // longitude × 1e7 (sint32)
    int32_t  alt_m;                              // metres, INT32_MIN = unset
    uint32_t epoch;                              // 0 = no data
} phoneapi_last_position_t;

// Last NEIGHBORINFO_APP (portnum 71) reply seen for a peer. Capped at
// 8 neighbours per source node — Meshtastic NeighborInfo can carry more
// in principle, but F-3 v1 only renders a single line per source so the
// cap is a render-side choice as much as a memory one.
//
// `entries[]` stores the SNR the SOURCE peer measures for each of its
// neighbours (i.e. peer X reports "I hear node Y at +5.5 dB"); not the
// SNR self → X (which lives in phoneapi_node_t.snr_x100).
//
// snr_x4 = dB × 4, saturated to int8 ([-32, +32] dB range), INT8_MIN if
// the wire didn't include the SNR field.
#define PHONEAPI_NEIGHBORS_MAX  8u

typedef struct {
    uint32_t node_num;
    int8_t   snr_x4;                             // INT8_MIN = unknown
} phoneapi_neighbor_entry_t;

typedef struct {
    uint8_t  count;                              // 0..PHONEAPI_NEIGHBORS_MAX
    uint32_t epoch;                              // 0 = no data, 1 = sentinel
    phoneapi_neighbor_entry_t entries[PHONEAPI_NEIGHBORS_MAX];
} phoneapi_neighbors_t;

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
    // User extras (B3-P2 — User.is_licensed=6, User.public_key=8)
    bool     is_licensed;
    uint8_t  public_key_len;         // 0 if absent, else 32 (Curve25519)
    uint8_t  public_key[32];
    // C-3 OP_TRACEROUTE / OP_REQUEST_POS reply caches. Populated by
    // FR_TAG_PACKET dispatch when a portnum 70 / 3 reply lands; read by
    // C-2 detail view.
    phoneapi_last_route_t    last_route;
    phoneapi_last_position_t last_position;
    phoneapi_neighbors_t     last_neighbors;
    // Bookkeeping
    uint32_t phase_seq;              // matches cache.current_phase_seq if fresh
} phoneapi_node_t;

// ── Config sub-oneof caches (B3-P1 / Cut B) ────────────────────────
//
// Each struct mirrors the subset of the corresponding meshtastic
// Config sub-message that B3-P1 exposes through settings_view.
// Field numbers used by the decoder are documented in
// firmware/core1/src/phoneapi/phoneapi_decode.c next to each parser.
//
// Buffers are populated by the cascade FR_TAG_CONFIG handler — every
// `--info` request from a USB host (or our own want_config_id at
// session start) replays the full Config tree, so the cache reflects
// the device's current durable state without any extra IPC GETs.

#define PHONEAPI_TZDEF_MAX  65u   // includes NUL — matches proto max_size

typedef struct {
    uint8_t  role;
    uint8_t  rebroadcast_mode;
    uint32_t node_info_broadcast_secs;
    bool     double_tap_as_button_press;
    bool     disable_triple_click;
    bool     led_heartbeat_disabled;
    char     tzdef[PHONEAPI_TZDEF_MAX];
} phoneapi_config_device_t;

typedef struct {
    bool     use_preset;
    uint8_t  modem_preset;
    uint32_t bandwidth;
    uint32_t spread_factor;
    uint32_t coding_rate;
    uint8_t  region;
    uint32_t hop_limit;
    bool     tx_enabled;
    int32_t  tx_power;
    uint32_t channel_num;
    bool     override_duty_cycle;
    bool     sx126x_rx_boosted_gain;
    uint8_t  fem_lna_mode;
} phoneapi_config_lora_t;

typedef struct {
    uint8_t  gps_mode;
    uint32_t gps_update_interval;
    uint32_t position_broadcast_secs;
    bool     position_broadcast_smart_enabled;
    bool     fixed_position;
    uint32_t position_flags;
    uint32_t broadcast_smart_minimum_distance;
    uint32_t broadcast_smart_minimum_interval_secs;
} phoneapi_config_position_t;

typedef struct {
    bool     is_power_saving;
    uint32_t on_battery_shutdown_after_secs;
    uint32_t wait_bluetooth_secs;
    uint32_t sds_secs;
    uint32_t ls_secs;
    uint32_t min_wake_secs;
    uint32_t device_battery_ina_address;
    uint32_t powermon_enables_lo;     // low 32 bits of u64 powermon_enables
} phoneapi_config_power_t;

typedef struct {
    uint8_t  public_key_len;          // 0 / 32
    uint8_t  public_key[32];
    bool     is_managed;
    bool     serial_enabled;
    bool     debug_log_api_enabled;
    bool     admin_channel_enabled;
} phoneapi_config_security_t;

typedef struct {
    uint32_t screen_on_secs;
    uint32_t auto_screen_carousel_secs;
    bool     flip_screen;
    uint8_t  units;
    uint8_t  oled;
    uint8_t  displaymode;
    bool     heading_bold;
    bool     wake_on_tap_or_motion;
    uint8_t  compass_orientation;
    bool     use_12h_clock;
    bool     use_long_node_name;
    bool     enable_message_bubbles;
} phoneapi_config_display_t;

// ── ModuleConfig sub-oneof caches (B3 follow-up — cascade walk-down) ──
//
// Same shape as phoneapi_config_*_t. Field numbers are documented in
// phoneapi_decode.c next to each parser. The IPC handler exposes a
// strict subset of fields per group (see ipc_protocol.h IpcConfigKey
// 0x10xx — 0x16xx); cached structs hold only those — the user sees
// nothing else through the settings UI, so decoding more would be
// dead Core 1 RAM.

typedef struct {
    uint32_t device_update_interval;
    uint32_t environment_update_interval;
    bool     environment_measurement_enabled;
    bool     environment_screen_enabled;
    bool     environment_display_fahrenheit;
    bool     power_measurement_enabled;
    uint32_t power_update_interval;
    bool     power_screen_enabled;
    bool     device_telemetry_enabled;
} phoneapi_module_telemetry_t;

typedef struct {
    bool     enabled;
    uint32_t update_interval;
    bool     transmit_over_lora;
} phoneapi_module_neighbor_t;

typedef struct {
    bool     enabled;
    uint32_t sender;
} phoneapi_module_range_test_t;

#define PHONEAPI_DETECT_NAME_MAX 20u
typedef struct {
    bool     enabled;
    uint32_t minimum_broadcast_secs;
    uint32_t state_broadcast_secs;
    char     name[PHONEAPI_DETECT_NAME_MAX];
    uint8_t  detection_trigger_type;
    bool     use_pullup;
} phoneapi_module_detect_t;

typedef struct {
    bool updown1_enabled;
    bool send_bell;
} phoneapi_module_canned_msg_t;

typedef struct {
    bool    led_state;
    uint8_t current;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} phoneapi_module_ambient_t;

typedef struct {
    bool     enabled;
    uint32_t paxcounter_update_interval;
} phoneapi_module_paxcounter_t;

/* T2.4.1 ModuleConfig.StoreForward — proto:563 */
typedef struct {
    bool     enabled;
    bool     heartbeat;
    uint32_t records;
    uint32_t history_return_max;
    uint32_t history_return_window;
    bool     is_server;
} phoneapi_module_store_forward_t;

/* T2.4.2 ModuleConfig.Serial — proto:379 */
typedef struct {
    bool     enabled;
    bool     echo;
    uint32_t rxd;
    uint32_t txd;
    uint8_t  baud;          /* Serial_Baud enum 0..15 */
    uint32_t timeout;
    uint8_t  mode;          /* Serial_Mode enum 0..10 */
    bool     override_console_serial_port;
} phoneapi_module_serial_t;

/* T2.4.3 ModuleConfig.ExternalNotification — proto:472 */
typedef struct {
    bool     enabled;
    uint32_t output_ms;
    uint32_t output;
    uint32_t output_vibra;
    uint32_t output_buzzer;
    bool     active;
    bool     alert_message;
    bool     alert_message_vibra;
    bool     alert_message_buzzer;
    bool     alert_bell;
    bool     alert_bell_vibra;
    bool     alert_bell_buzzer;
    bool     use_pwm;
    uint32_t nag_timeout;
    bool     use_i2s_as_buzzer;
} phoneapi_module_ext_notif_t;

/* T2.4.4 ModuleConfig.RemoteHardware — proto:110.  available_pins[] is
 * a repeated nested message and not exposed in v1 (needs list editor). */
typedef struct {
    bool enabled;
    bool allow_undefined_pin_access;
} phoneapi_module_remote_hw_t;

// Decoded TEXT_MESSAGE_APP payload — published by the cascade decoder
// when a FromRadio.packet with portnum==TEXT_MESSAGE_APP is seen.
// Field shape matches `messages_inbox_entry_t` so messages_view can be
// migrated with minimal code change.
//
// A3: trailing radio metadata fields lifted off the MeshPacket envelope
// (rx_snr/rx_rssi/hop_limit/hop_start) feed dm_store so the long-press
// detail modal can show signal context. Sentinel values document
// "decoder didn't see this field on the wire" cases.
typedef struct {
    uint32_t seq;            ///< Monotonic id assigned at publish
    uint32_t from_node_id;
    uint32_t to_node_id;
    uint8_t  channel_index;
    uint16_t text_len;
    uint8_t  text[PHONEAPI_MSG_TEXT_MAX];
    int16_t  rx_snr_x4;      ///< INT16_MIN if MeshPacket.rx_snr absent
    int16_t  rx_rssi;        ///< 0 if MeshPacket.rx_rssi absent (dBm signed)
    uint8_t  hop_limit;      ///< 0xFF if MeshPacket.hop_limit absent
    uint8_t  hop_start;      ///< 0xFF if MeshPacket.hop_start absent
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

// Config sub-oneof writers (B3-P1 / Cut B)
void phoneapi_cache_set_config_device(const phoneapi_config_device_t *cfg);
void phoneapi_cache_set_config_lora(const phoneapi_config_lora_t *cfg);
void phoneapi_cache_set_config_position(const phoneapi_config_position_t *cfg);
void phoneapi_cache_set_config_display(const phoneapi_config_display_t *cfg);
// B3-P2 additions
void phoneapi_cache_set_config_power(const phoneapi_config_power_t *cfg);
void phoneapi_cache_set_config_security(const phoneapi_config_security_t *cfg);

// ModuleConfig writers (B3 cascade walk-down follow-up)
void phoneapi_cache_set_module_telemetry(const phoneapi_module_telemetry_t *m);
void phoneapi_cache_set_module_neighbor(const phoneapi_module_neighbor_t *m);
void phoneapi_cache_set_module_range_test(const phoneapi_module_range_test_t *m);
void phoneapi_cache_set_module_detect(const phoneapi_module_detect_t *m);
void phoneapi_cache_set_module_canned_msg(const phoneapi_module_canned_msg_t *m);
void phoneapi_cache_set_module_ambient(const phoneapi_module_ambient_t *m);
void phoneapi_cache_set_module_paxcounter(const phoneapi_module_paxcounter_t *m);

/* T2.4 — 4 new modules' setters. */
void phoneapi_cache_set_module_store_forward(const phoneapi_module_store_forward_t *m);
void phoneapi_cache_set_module_serial(const phoneapi_module_serial_t *m);
void phoneapi_cache_set_module_ext_notif(const phoneapi_module_ext_notif_t *m);
void phoneapi_cache_set_module_remote_hw(const phoneapi_module_remote_hw_t *m);

// Readers — copy out under the mutex.
bool phoneapi_cache_get_my_info(phoneapi_my_info_t *out);
bool phoneapi_cache_get_metadata(phoneapi_metadata_t *out);
bool phoneapi_cache_get_channel(uint8_t index, phoneapi_channel_t *out);

// Config sub-oneof readers — return false if cascade hasn't yet
// delivered that sub-message in the current session.
bool phoneapi_cache_get_config_device(phoneapi_config_device_t *out);
bool phoneapi_cache_get_config_lora(phoneapi_config_lora_t *out);
bool phoneapi_cache_get_config_position(phoneapi_config_position_t *out);
bool phoneapi_cache_get_config_display(phoneapi_config_display_t *out);
bool phoneapi_cache_get_config_power(phoneapi_config_power_t *out);
bool phoneapi_cache_get_config_security(phoneapi_config_security_t *out);

bool phoneapi_cache_get_module_telemetry(phoneapi_module_telemetry_t *out);
bool phoneapi_cache_get_module_neighbor(phoneapi_module_neighbor_t *out);
bool phoneapi_cache_get_module_range_test(phoneapi_module_range_test_t *out);
bool phoneapi_cache_get_module_detect(phoneapi_module_detect_t *out);
bool phoneapi_cache_get_module_canned_msg(phoneapi_module_canned_msg_t *out);
bool phoneapi_cache_get_module_ambient(phoneapi_module_ambient_t *out);
bool phoneapi_cache_get_module_paxcounter(phoneapi_module_paxcounter_t *out);

/* T2.4 — 4 new modules' getters.  Each returns false if cascade has not
 * yet delivered the sub-message in the current session. */
bool phoneapi_cache_get_module_store_forward(phoneapi_module_store_forward_t *out);
bool phoneapi_cache_get_module_serial(phoneapi_module_serial_t *out);
bool phoneapi_cache_get_module_ext_notif(phoneapi_module_ext_notif_t *out);
bool phoneapi_cache_get_module_remote_hw(phoneapi_module_remote_hw_t *out);
uint32_t phoneapi_cache_node_count(void);
// Copy node by relative index (0..count-1, ordered most-recent first).
bool phoneapi_cache_take_node_at(uint32_t index, phoneapi_node_t *out);
// Copy node by absolute node_id; returns false if not present.
bool phoneapi_cache_get_node_by_id(uint32_t node_id, phoneapi_node_t *out);

// C-3 OP_TRACEROUTE / OP_REQUEST_POS reply writers. No-op if the
// referenced node isn't in the cache. Bumps change_seq so the C-2
// detail view's refresh hook re-renders. Safe to call from the
// session task (mutex-protected).
void phoneapi_cache_set_last_route(uint32_t node_num,
                                   const phoneapi_last_route_t *r);
void phoneapi_cache_set_last_position(uint32_t node_num,
                                      const phoneapi_last_position_t *p);
void phoneapi_cache_set_last_neighbors(uint32_t node_num,
                                       const phoneapi_neighbors_t *n);

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
