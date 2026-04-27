// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors

#include "phoneapi_session.h"

#include "phoneapi_cache.h"
#include "phoneapi_decode.h"
#include "phoneapi_framing.h"
#include "mokya_trace.h"

static phoneapi_framing_t s_framing;

// Aggregate stats — inspectable via SWD for Phase A validation.
static struct {
    uint32_t frames_parsed;
    uint32_t frames_malformed;
    uint32_t variant_counts[18];   // index = field number (0..17)
} s_stats;

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
    phoneapi_framing_init(&s_framing, on_frame, NULL);
    s_stats.frames_parsed    = 0;
    s_stats.frames_malformed = 0;
    for (size_t i = 0; i < sizeof(s_stats.variant_counts) /
                           sizeof(s_stats.variant_counts[0]); i++) {
        s_stats.variant_counts[i] = 0;
    }
    TRACE_BARE("phapi", "init");
}

void phoneapi_session_feed_from_core0(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0u) {
        return;
    }
    phoneapi_framing_push(&s_framing, buf, len);
}
