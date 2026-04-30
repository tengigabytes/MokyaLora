// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors

#include "phoneapi_session.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "pico/time.h"

#include "phoneapi_cache.h"
#include "phoneapi_decode.h"
#include "phoneapi_encode.h"
#include "phoneapi_framing.h"
#include "phoneapi_tx.h"
#include "dm_store.h"
#include "messages_tx_status.h"
#include "global/status_bar.h"
#include "history.h"
#include "mokya_trace.h"

static phoneapi_framing_t s_framing;

// Aggregate stats — inspectable via SWD for Phase A validation.
static struct {
    uint32_t frames_parsed;
    uint32_t frames_malformed;
    uint32_t variant_counts[18];   // index = field number (0..17)
} s_stats;

// Session mode + nonce. Read via the public inspectors; written from
// the bridge / USB callback context (single-writer per direction).
static phoneapi_mode_t s_mode       = PHONEAPI_MODE_STANDALONE;
static uint32_t        s_last_nonce = 0;

// Heartbeat timer. Period chosen well under PhoneAPI's 15-min serial
// timeout (`SerialConsole.cpp:27`). Started in STANDALONE, stopped in
// FORWARD (the USB host's heartbeats keep Core 0 alive in that mode).
#define PHONEAPI_HEARTBEAT_PERIOD_MS (5u * 60u * 1000u)
static TimerHandle_t s_heartbeat_timer = NULL;

static void heartbeat_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (s_mode == PHONEAPI_MODE_STANDALONE) {
        if (phoneapi_encode_heartbeat()) {
            TRACE_BARE("phapi", "tx_hb");
        }
    }
}

// Phase C nonce source: a simple LCG seeded by the µs counter. PhoneAPI
// uses the nonce only to tag `config_complete_id` echoes back to the
// originating client; cryptographic uniqueness is not required.
static uint32_t fresh_nonce(void)
{
    static uint32_t s_seed = 0;
    uint32_t        t      = (uint32_t)to_us_since_boot(get_absolute_time());
    s_seed = s_seed * 1103515245u + 12345u + t;
    if (s_seed == 0u) s_seed = 1u;
    return s_seed;
}

static void issue_want_config_id(const char *cause)
{
    s_last_nonce = fresh_nonce();
    if (phoneapi_encode_want_config_id(s_last_nonce)) {
        TRACE("phapi", "tx_wcid", "nonce=%u,cause=%s",
              (unsigned)s_last_nonce, cause);
    }
}

// Locate the variant's sub-message bytes inside a FromRadio frame and
// hand them to `decoder` + writer. Used for LD-variant tags only.
static void dispatch_to_cache_writer(const uint8_t *payload, uint16_t len,
                                     from_radio_tag_t tag,
                                     bool (*decode_fn)(const uint8_t *, uint16_t, void *),
                                     void (*write_fn)(void *),
                                     void *staging)
{
    uint16_t sub_len = 0;
    const uint8_t *sub = phoneapi_find_variant_payload(payload, len, tag, &sub_len);
    if (sub == NULL) {
        TRACE("phapi", "no_payload", "tag=%u", (unsigned)tag);
        return;
    }
    if (!decode_fn(sub, sub_len, staging)) {
        TRACE("phapi", "decode_fail", "tag=%u,len=%u",
              (unsigned)tag, (unsigned)sub_len);
        return;
    }
    write_fn(staging);
}

// Adapters so we can take function pointers of slightly-different signatures.
static bool wrap_decode_my_info(const uint8_t *b, uint16_t l, void *p)
{ return phoneapi_decode_my_info(b, l, (phoneapi_my_info_t *)p); }
static bool wrap_decode_metadata(const uint8_t *b, uint16_t l, void *p)
{ return phoneapi_decode_metadata(b, l, (phoneapi_metadata_t *)p); }
static bool wrap_decode_channel(const uint8_t *b, uint16_t l, void *p)
{ return phoneapi_decode_channel(b, l, (phoneapi_channel_t *)p); }
static bool wrap_decode_node_info(const uint8_t *b, uint16_t l, void *p)
{ return phoneapi_decode_node_info(b, l, (phoneapi_node_t *)p); }

static void wrap_write_my_info(void *p)
{
    const phoneapi_my_info_t *mi = (const phoneapi_my_info_t *)p;
    TRACE("phapi", "set_my_info",
          "num=%lu reboots=%u nodedb=%u",
          (unsigned long)mi->my_node_num,
          (unsigned)mi->reboot_count,
          (unsigned)mi->nodedb_count);
    phoneapi_cache_set_my_info(mi);
}
static void wrap_write_metadata(void *p)
{ phoneapi_cache_set_metadata((const phoneapi_metadata_t *)p); }
static void wrap_write_channel(void *p) {
    const phoneapi_channel_t *c = (const phoneapi_channel_t *)p;
    phoneapi_cache_set_channel(c->index, c);
}
static void wrap_write_node(void *p)
{ phoneapi_cache_upsert_node((const phoneapi_node_t *)p); }

// Config oneof callback — Cut B FR_TAG_CONFIG dispatcher. Fields
// 1, 2, 5, 6 land in the four sub-decoders we ship in B3-P1; the
// rest (power, network, bluetooth, security, sessionkey, device_ui)
// fall through to a tag-only trace and are deferred to later phases.
static void config_oneof_cb(uint32_t field_num,
                            const uint8_t *sub_buf, uint16_t sub_len,
                            void *ctx)
{
    (void)ctx;
    switch (field_num) {
    case 1u: {
        phoneapi_config_device_t cfg;
        if (phoneapi_decode_config_device(sub_buf, sub_len, &cfg)) {
            phoneapi_cache_set_config_device(&cfg);
            TRACE("phapi", "cfg_device", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "cfg_device_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 2u: {
        phoneapi_config_position_t cfg;
        if (phoneapi_decode_config_position(sub_buf, sub_len, &cfg)) {
            phoneapi_cache_set_config_position(&cfg);
            TRACE("phapi", "cfg_position", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "cfg_position_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 5u: {
        phoneapi_config_display_t cfg;
        if (phoneapi_decode_config_display(sub_buf, sub_len, &cfg)) {
            phoneapi_cache_set_config_display(&cfg);
            TRACE("phapi", "cfg_display", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "cfg_display_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 6u: {
        phoneapi_config_lora_t cfg;
        if (phoneapi_decode_config_lora(sub_buf, sub_len, &cfg)) {
            phoneapi_cache_set_config_lora(&cfg);
            TRACE("phapi", "cfg_lora", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "cfg_lora_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 3u: {  /* PowerConfig (B3-P2) */
        phoneapi_config_power_t cfg;
        if (phoneapi_decode_config_power(sub_buf, sub_len, &cfg)) {
            phoneapi_cache_set_config_power(&cfg);
            TRACE("phapi", "cfg_power", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "cfg_power_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 8u: {  /* SecurityConfig (B3-P2) */
        phoneapi_config_security_t cfg;
        if (phoneapi_decode_config_security(sub_buf, sub_len, &cfg)) {
            phoneapi_cache_set_config_security(&cfg);
            TRACE("phapi", "cfg_security",
                  "len=%u,pkl=%u",
                  (unsigned)sub_len, (unsigned)cfg.public_key_len);
        } else {
            TRACE("phapi", "cfg_security_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    default:
        // network / bluetooth / sessionkey / device_ui
        TRACE("phapi", "cfg_skip",
              "f=%u,len=%u", (unsigned)field_num, (unsigned)sub_len);
        break;
    }
}

// ModuleConfig oneof callback — same shape as config_oneof_cb. Field
// numbers from module_config.proto (range_test=5, telemetry=6,
// canned_message=7, neighbor_info=10, ambient_lighting=11,
// detection_sensor=12, paxcounter=13). MQTT/Serial/External
// notification/StoreForward/Audio/RemoteHardware/StatusMessage/
// TrafficManagement are not exposed through IpcConfigKey, so they
// fall through to a tag-only trace.
static void module_config_oneof_cb(uint32_t field_num,
                                   const uint8_t *sub_buf, uint16_t sub_len,
                                   void *ctx)
{
    (void)ctx;
    switch (field_num) {
    case 5u: {  /* RangeTestConfig */
        phoneapi_module_range_test_t m;
        if (phoneapi_decode_module_range_test(sub_buf, sub_len, &m)) {
            phoneapi_cache_set_module_range_test(&m);
            TRACE("phapi", "mc_range", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "mc_range_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 6u: {  /* TelemetryConfig */
        phoneapi_module_telemetry_t m;
        if (phoneapi_decode_module_telemetry(sub_buf, sub_len, &m)) {
            phoneapi_cache_set_module_telemetry(&m);
            TRACE("phapi", "mc_telem", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "mc_telem_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 7u: {  /* CannedMessageConfig */
        phoneapi_module_canned_msg_t m;
        if (phoneapi_decode_module_canned_msg(sub_buf, sub_len, &m)) {
            phoneapi_cache_set_module_canned_msg(&m);
            TRACE("phapi", "mc_canned", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "mc_canned_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 10u: {  /* NeighborInfoConfig */
        phoneapi_module_neighbor_t m;
        if (phoneapi_decode_module_neighbor(sub_buf, sub_len, &m)) {
            phoneapi_cache_set_module_neighbor(&m);
            TRACE("phapi", "mc_neighbor", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "mc_neighbor_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 11u: {  /* AmbientLightingConfig */
        phoneapi_module_ambient_t m;
        if (phoneapi_decode_module_ambient(sub_buf, sub_len, &m)) {
            phoneapi_cache_set_module_ambient(&m);
            TRACE("phapi", "mc_ambient", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "mc_ambient_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 12u: {  /* DetectionSensorConfig */
        phoneapi_module_detect_t m;
        if (phoneapi_decode_module_detect(sub_buf, sub_len, &m)) {
            phoneapi_cache_set_module_detect(&m);
            TRACE("phapi", "mc_detect", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "mc_detect_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 13u: {  /* PaxcounterConfig */
        phoneapi_module_paxcounter_t m;
        if (phoneapi_decode_module_paxcounter(sub_buf, sub_len, &m)) {
            phoneapi_cache_set_module_paxcounter(&m);
            TRACE("phapi", "mc_pax", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "mc_pax_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 2u: {  /* SerialConfig — T2.4.2 */
        phoneapi_module_serial_t m;
        if (phoneapi_decode_module_serial(sub_buf, sub_len, &m)) {
            phoneapi_cache_set_module_serial(&m);
            TRACE("phapi", "mc_serial", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "mc_serial_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 3u: {  /* ExternalNotificationConfig — T2.4.3 */
        phoneapi_module_ext_notif_t m;
        if (phoneapi_decode_module_ext_notif(sub_buf, sub_len, &m)) {
            phoneapi_cache_set_module_ext_notif(&m);
            TRACE("phapi", "mc_extnot", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "mc_extnot_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 4u: {  /* StoreForwardConfig — T2.4.1 */
        phoneapi_module_store_forward_t m;
        if (phoneapi_decode_module_store_forward(sub_buf, sub_len, &m)) {
            phoneapi_cache_set_module_store_forward(&m);
            TRACE("phapi", "mc_sf", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "mc_sf_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    case 9u: {  /* RemoteHardwareConfig — T2.4.4 */
        phoneapi_module_remote_hw_t m;
        if (phoneapi_decode_module_remote_hw(sub_buf, sub_len, &m)) {
            phoneapi_cache_set_module_remote_hw(&m);
            TRACE("phapi", "mc_rhw", "len=%u", (unsigned)sub_len);
        } else {
            TRACE("phapi", "mc_rhw_fail", "len=%u", (unsigned)sub_len);
        }
        break;
    }
    default:
        TRACE("phapi", "mc_skip",
              "f=%u,len=%u", (unsigned)field_num, (unsigned)sub_len);
        break;
    }
}

static void on_frame(const uint8_t *payload, uint16_t len, void *user)
{
    (void)user;

    phoneapi_from_radio_summary_t summary;
    if (!phoneapi_decode_from_radio(payload, len, &summary)) {
        s_stats.frames_malformed++;
        TRACE("phapi", "bad_frame", "len=%u", (unsigned)len);
        return;
    }

    s_stats.frames_parsed++;
    if ((unsigned)summary.variant_tag < (sizeof(s_stats.variant_counts) /
                                         sizeof(s_stats.variant_counts[0]))) {
        s_stats.variant_counts[summary.variant_tag]++;
    }

    TRACE("phapi", "rx_frame",
          "len=%u,id=%u,tag=%s,val=%u",
          (unsigned)len,
          (unsigned)summary.frame_id,
          phoneapi_from_radio_tag_name(summary.variant_tag),
          (unsigned)summary.variant_value);

    // Dispatch into cache. Local staging structs are stack-allocated to
    // avoid a second copy; the cache writer takes the mutex internally.
    switch (summary.variant_tag) {
    case FR_TAG_MY_INFO: {
        // my_info marks the start of a new phase — bump phase_seq so
        // node_info entries that arrive next can be tagged as fresh.
        phoneapi_cache_phase_begin();
        phoneapi_my_info_t mi;
        dispatch_to_cache_writer(payload, len, FR_TAG_MY_INFO,
                                 wrap_decode_my_info, wrap_write_my_info, &mi);
        break;
    }
    case FR_TAG_METADATA: {
        phoneapi_metadata_t md;
        dispatch_to_cache_writer(payload, len, FR_TAG_METADATA,
                                 wrap_decode_metadata, wrap_write_metadata, &md);
        break;
    }
    case FR_TAG_CHANNEL: {
        phoneapi_channel_t ch;
        dispatch_to_cache_writer(payload, len, FR_TAG_CHANNEL,
                                 wrap_decode_channel, wrap_write_channel, &ch);
        break;
    }
    case FR_TAG_NODE_INFO: {
        phoneapi_node_t nd;
        dispatch_to_cache_writer(payload, len, FR_TAG_NODE_INFO,
                                 wrap_decode_node_info, wrap_write_node, &nd);
        break;
    }
    case FR_TAG_CONFIG: {
        // Cascade replays the full Config tree on every want_config_id
        // (and on every host --info).  We dispatch each sub-oneof field
        // (device=1, position=2, display=5, lora=6) into its decoder
        // and into phoneapi_cache so settings_view can read straight
        // from cache without an IPC GET burst (M5F.3).
        uint16_t cfg_len = 0;
        const uint8_t *cfg = phoneapi_find_variant_payload(payload, len,
                                                           FR_TAG_CONFIG,
                                                           &cfg_len);
        if (cfg == NULL) break;
        (void)phoneapi_walk_config_oneof(cfg, cfg_len, config_oneof_cb, NULL);
        break;
    }
    case FR_TAG_MODULE_CONFIG: {
        // Same model as FR_TAG_CONFIG: cascade emits each ModuleConfig
        // sub-oneof on want_config_id replay and on AdminModule SETs.
        // Walk the LD fields and route each to its decoder + cache
        // writer (B3-P3/P4 follow-up).
        uint16_t mc_len = 0;
        const uint8_t *mc = phoneapi_find_variant_payload(payload, len,
                                                          FR_TAG_MODULE_CONFIG,
                                                          &mc_len);
        if (mc == NULL) break;
        (void)phoneapi_walk_config_oneof(mc, mc_len, module_config_oneof_cb, NULL);
        break;
    }
    case FR_TAG_CONFIG_COMPLETE_ID:
        phoneapi_cache_commit(summary.variant_value);
        TRACE("phapi", "commit",
              "id=%u,nodes=%u",
              (unsigned)summary.variant_value,
              (unsigned)phoneapi_cache_node_count());
        break;
    case FR_TAG_PACKET: {
        // Steady-state mesh traffic. Phase D extracts TEXT_MESSAGE_APP
        // packets into the messages ring; M5F.1 also extracts
        // ROUTING_APP acks → messages_tx_status. Other portnums are
        // dropped (telemetry / position — not displayed in v1 LVGL).
        uint16_t sub_len = 0;
        const uint8_t *sub = phoneapi_find_variant_payload(payload, len,
                                                           FR_TAG_PACKET,
                                                           &sub_len);
        if (sub == NULL) break;

        /* T2.6 — tap envelope SNR for the F-4 history ring. The text
         * decoder parses MeshPacket.rx_snr (field 8) before checking
         * portnum, so rx_snr_x4 is set even when the call returns false
         * for non-TEXT packets. We don't care whether the payload is
         * text — any RX with a populated rx_snr counts as a signal-
         * level data point. */
        {
            phoneapi_text_msg_t env;
            (void)phoneapi_decode_text_packet(sub, sub_len, &env);
            if (env.rx_snr_x4 != INT16_MIN) {
                metrics_history_note_rx_snr_x4(env.rx_snr_x4);
            }
        }

        uint32_t ack_pid = 0u;
        uint8_t  ack_err = 0u;
        if (phoneapi_decode_routing_ack(sub, sub_len, &ack_pid, &ack_err)) {
            uint8_t result = (ack_err == 0u)
                                 ? MESSAGES_TX_RESULT_DELIVERED
                                 : MESSAGES_TX_RESULT_FAILED;
            messages_tx_status_publish(result, ack_err, ack_pid);
            TRACE("phapi", "rx_ack",
                  "pid=%u,err=%u",
                  (unsigned)ack_pid, (unsigned)ack_err);
            break;
        }

        /* C-3 OP_TRACEROUTE / OP_REQUEST_POS reply dispatch.  Tried
         * before the text path because traceroute / position payloads
         * can't masquerade as TEXT_MESSAGE_APP (different portnum).  */
        {
            uint32_t from_tr = 0u;
            phoneapi_last_route_t r;
            if (phoneapi_decode_traceroute_packet(sub, sub_len,
                                                  &from_tr, &r)) {
                /* RouteDiscovery has no wire timestamp. Stamp epoch=1
                 * as a sentinel so render code can distinguish "no
                 * reply yet" (epoch==0) from "0-hop direct neighbour"
                 * (epoch==1, hop_count==0). */
                if (r.epoch == 0u) r.epoch = 1u;
                phoneapi_cache_set_last_route(from_tr, &r);
                TRACE("phapi", "rx_route",
                      "from=%u,fwd_hops=%u,back_hops=%u",
                      (unsigned)from_tr, (unsigned)r.hop_count,
                      (unsigned)r.hops_back_count);
                break;
            }
            uint32_t from_pos = 0u;
            phoneapi_last_position_t p;
            if (phoneapi_decode_position_packet(sub, sub_len,
                                                &from_pos, &p)) {
                /* Position carries time on the wire. If absent (peer
                 * has no GPS-derived timestamp), stamp epoch=1 so
                 * render still treats it as "have data". */
                if (p.epoch == 0u) p.epoch = 1u;
                phoneapi_cache_set_last_position(from_pos, &p);
                TRACE("phapi", "rx_pos",
                      "from=%u,lat=%d,lon=%d,alt=%d,t=%u",
                      (unsigned)from_pos, (int)p.lat_e7,
                      (int)p.lon_e7, (int)p.alt_m, (unsigned)p.epoch);
                break;
            }
        }

        phoneapi_text_msg_t m;
        bool is_text = phoneapi_decode_text_packet(sub, sub_len, &m);
        TRACE("phapi", "rx_packet",
              "sub_len=%u,is_text=%u,from=%u,to=%u,len=%u",
              (unsigned)sub_len, (unsigned)is_text,
              (unsigned)m.from_node_id, (unsigned)m.to_node_id,
              (unsigned)m.text_len);
        if (is_text) {
            phoneapi_msgs_publish(m.from_node_id, m.to_node_id,
                                  m.channel_index, m.text, m.text_len);
            /* Phase 3: also publish into the per-peer DM store so
             * chat_list / conversation_view can render thread history.
             * Only DMs (to_node_id == own node num) are useful for
             * conversation threading; broadcasts (to_node_id ==
             * 0xFFFFFFFFu) and channel-only traffic don't fit the
             * one-thread-per-peer model. */
            if (m.to_node_id != 0xFFFFFFFFu) {
                /* phoneapi_msgs_publish bumps an internal seq we can't
                 * easily read without taking the cache mutex; pass 0
                 * and let dm_store assign its own monotonic id from the
                 * outbound counter (inbound seq isn't load-bearing).
                 * Forward the radio metadata pulled off the MeshPacket
                 * envelope so the long-press detail modal (A3) can
                 * surface SNR / RSSI / hops. */
                dm_msg_meta_t meta = {
                    .rx_snr_x4 = m.rx_snr_x4,
                    .rx_rssi   = m.rx_rssi,
                    .hop_limit = m.hop_limit,
                    .hop_start = m.hop_start,
                };
                dm_store_ingest_inbound(m.from_node_id, 0u,
                                        m.text, m.text_len, &meta);
            }
            TRACE("phapi", "rx_text",
                  "from=%u,len=%u",
                  (unsigned)m.from_node_id, (unsigned)m.text_len);
            /* G-1 status bar: pulse RX activity (250 ms). Cosmetic. */
            status_bar_pulse_rx();
        }
        break;
    }
    case FR_TAG_QUEUE_STATUS: {
        // Local Core 0 queue feedback for our last submission. If
        // mesh_packet_id matches a still-pending send and `res` is
        // non-zero, treat as a fast-fail (e.g. queue overflow,
        // routing rejected before air). On res==0 we wait for the
        // routing-ack instead — queue OK doesn't mean delivered.
        uint16_t sub_len = 0;
        const uint8_t *sub = phoneapi_find_variant_payload(payload, len,
                                                           FR_TAG_QUEUE_STATUS,
                                                           &sub_len);
        if (sub == NULL) break;
        phoneapi_queue_status_t qs;
        if (!phoneapi_decode_queue_status(sub, sub_len, &qs)) break;
        TRACE("phapi", "rx_queue",
              "pid=%u,res=%d,free=%u",
              (unsigned)qs.mesh_packet_id, (int)qs.res, (unsigned)qs.free);
        if (qs.mesh_packet_id != 0u && qs.res != 0) {
            messages_tx_status_publish(MESSAGES_TX_RESULT_FAILED,
                                       (uint8_t)qs.res,
                                       qs.mesh_packet_id);
        }
        break;
    }
    default:
        // CONFIG / MODULE_CONFIG / FILE_INFO / DEVICEUI / etc — Phase A
        // tag-only tracing already happened above. Field-level decode
        // for these lands in Phase F (admin/settings UX).
        break;
    }
}

void phoneapi_session_init(void)
{
    phoneapi_cache_init();
    phoneapi_tx_init();
    phoneapi_framing_init(&s_framing, on_frame, NULL);
    s_stats.frames_parsed    = 0;
    s_stats.frames_malformed = 0;
    for (size_t i = 0; i < sizeof(s_stats.variant_counts) /
                           sizeof(s_stats.variant_counts[0]); i++) {
        s_stats.variant_counts[i] = 0;
    }

    s_mode = PHONEAPI_MODE_STANDALONE;
    s_heartbeat_timer = xTimerCreate("phapi_hb",
                                     pdMS_TO_TICKS(PHONEAPI_HEARTBEAT_PERIOD_MS),
                                     pdTRUE,           // auto-reload
                                     NULL,
                                     heartbeat_timer_cb);
    if (s_heartbeat_timer != NULL) {
        xTimerStart(s_heartbeat_timer, 0);
    }

    // Boot-time want_config_id. Core 0 may not have its PhoneAPI fully
    // initialised yet; if it drops the request, we send another one
    // anyway when the USB host plugs in (FORWARD → STANDALONE) or via
    // the heartbeat-driven probe. For Phase C this single send is
    // sufficient — Core 0 is up by the time bridge_task starts in
    // practice (see vApplicationIdleHook deferred launch).
    issue_want_config_id("boot");

    TRACE_BARE("phapi", "init");
}

void phoneapi_session_set_usb_connected(bool connected)
{
    phoneapi_mode_t prev = s_mode;
    s_mode = connected ? PHONEAPI_MODE_FORWARD : PHONEAPI_MODE_STANDALONE;

    if (prev == s_mode) return;

    TRACE("phapi", "mode",
          "prev=%u,now=%u",
          (unsigned)prev, (unsigned)s_mode);

    if (s_mode == PHONEAPI_MODE_STANDALONE) {
        // Re-take ownership of the session: USB host's mid-stream state
        // may have left things half-baked. Issue fresh want_config_id
        // and resume heartbeats.
        issue_want_config_id("usb_unplug");
        if (s_heartbeat_timer != NULL) {
            xTimerReset(s_heartbeat_timer, 0);
        }
    } else {
        // USB host now drives the session; suspend our heartbeat (host
        // sends its own per Meshtastic CLI behaviour).
        if (s_heartbeat_timer != NULL) {
            xTimerStop(s_heartbeat_timer, 0);
        }
    }
}

phoneapi_mode_t phoneapi_session_mode(void)        { return s_mode; }
uint32_t        phoneapi_session_last_nonce(void)  { return s_last_nonce; }

void phoneapi_session_close(void)
{
    if (s_heartbeat_timer != NULL) {
        xTimerStop(s_heartbeat_timer, 0);
    }
    if (phoneapi_encode_disconnect()) {
        TRACE_BARE("phapi", "tx_disconnect");
    }
    s_last_nonce = 0u;
    /* Cache preserved; next set_usb_connected(false) will re-arm. */
}

// M5E.4 (2026-04-28) — removed the tud_cdc_line_state_cb override.
// bridge_task in main_core1_bridge.c now polls cdc_host_active() each
// iteration and calls phoneapi_session_set_usb_connected() on edges.
// That source of truth covers DTR-less hosts (Chrome WebSerial) which
// the raw line-state callback could not detect.

void phoneapi_session_feed_from_core0(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0u) {
        return;
    }
    phoneapi_framing_push(&s_framing, buf, len);
}
