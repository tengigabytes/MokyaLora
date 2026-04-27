// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors

#include "phoneapi_session.h"

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
}

void phoneapi_session_init(void)
{
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
