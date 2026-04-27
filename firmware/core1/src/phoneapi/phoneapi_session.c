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
{ phoneapi_cache_set_my_info((const phoneapi_my_info_t *)p); }
static void wrap_write_metadata(void *p)
{ phoneapi_cache_set_metadata((const phoneapi_metadata_t *)p); }
static void wrap_write_channel(void *p) {
    const phoneapi_channel_t *c = (const phoneapi_channel_t *)p;
    phoneapi_cache_set_channel(c->index, c);
}
static void wrap_write_node(void *p)
{ phoneapi_cache_upsert_node((const phoneapi_node_t *)p); }

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
    case FR_TAG_CONFIG_COMPLETE_ID:
        phoneapi_cache_commit(summary.variant_value);
        TRACE("phapi", "commit",
              "id=%u,nodes=%u",
              (unsigned)summary.variant_value,
              (unsigned)phoneapi_cache_node_count());
        break;
    case FR_TAG_PACKET: {
        // Steady-state mesh traffic. Phase D extracts TEXT_MESSAGE_APP
        // packets into the messages ring; other portnums are dropped
        // (telemetry / position / routing — not displayed in v1 LVGL).
        uint16_t sub_len = 0;
        const uint8_t *sub = phoneapi_find_variant_payload(payload, len,
                                                           FR_TAG_PACKET,
                                                           &sub_len);
        if (sub == NULL) break;
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
            TRACE("phapi", "rx_text",
                  "from=%u,len=%u",
                  (unsigned)m.from_node_id, (unsigned)m.text_len);
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

// TinyUSB CDC line-state callback. Fires from the USB device task
// whenever the host raises/lowers DTR (USB CDC's "port open" signal).
// `connected` follows DTR. We override TinyUSB's weak default here;
// putting it in phoneapi_session.c keeps the dependency gated by
// MOKYA_PHONEAPI_CASCADE — the file is only compiled with the flag on.
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)rts;
    phoneapi_session_set_usb_connected(dtr);
}

void phoneapi_session_feed_from_core0(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0u) {
        return;
    }
    phoneapi_framing_push(&s_framing, buf, len);
}
