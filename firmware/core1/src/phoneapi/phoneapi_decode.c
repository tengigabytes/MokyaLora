// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors

#include "phoneapi_decode.h"

#include <limits.h>
#include <string.h>

// Protobuf wire types (https://protobuf.dev/programming-guides/encoding/)
#define WT_VARINT  0  // int32, int64, uint32, uint64, bool, enum
#define WT_I64     1  // fixed64, sfixed64, double
#define WT_LEN     2  // length-delimited (string, bytes, embedded message)
#define WT_I32     5  // fixed32, sfixed32, float

// Decode a varint from buf+pos. On success, advance *pos and store value.
// Returns false on truncation or >10-byte varint.
static bool read_varint(const uint8_t *buf, uint16_t len, uint16_t *pos, uint64_t *out)
{
    uint64_t result = 0;
    int      shift  = 0;
    while (*pos < len) {
        uint8_t b = buf[(*pos)++];
        result |= ((uint64_t)(b & 0x7Fu)) << shift;
        if ((b & 0x80u) == 0u) {
            *out = result;
            return true;
        }
        shift += 7;
        if (shift >= 70) {
            return false;
        }
    }
    return false;
}

// Skip a field of given wire type. Advances *pos past the value.
static bool skip_field(const uint8_t *buf, uint16_t len, uint16_t *pos, uint8_t wire_type)
{
    switch (wire_type) {
    case WT_VARINT: {
        uint64_t dummy;
        return read_varint(buf, len, pos, &dummy);
    }
    case WT_I64:
        if (*pos + 8u > len) return false;
        *pos += 8u;
        return true;
    case WT_LEN: {
        uint64_t sub_len;
        if (!read_varint(buf, len, pos, &sub_len)) return false;
        if (sub_len > (uint64_t)(len - *pos)) return false;
        *pos += (uint16_t)sub_len;
        return true;
    }
    case WT_I32:
        if (*pos + 4u > len) return false;
        *pos += 4u;
        return true;
    default:
        return false;  // groups (3,4) deprecated, not used by Meshtastic
    }
}

bool phoneapi_decode_from_radio(const uint8_t *buf,
                                uint16_t       len,
                                phoneapi_from_radio_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    out->variant_tag = FR_TAG_NONE;

    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) {
            return false;
        }
        uint32_t field_num = (uint32_t)(tag_word >> 3);
        uint8_t  wire_type = (uint8_t)(tag_word & 0x07u);

        // Field 1 — top-level FromRadio.id (uint32, varint)
        if (field_num == 1u && wire_type == WT_VARINT) {
            uint64_t v;
            if (!read_varint(buf, len, &pos, &v)) return false;
            out->frame_id = (uint32_t)v;
            continue;
        }

        // Fields 2..17 — oneof payload_variant. We record the first one
        // we encounter (a well-formed FromRadio carries exactly one).
        if (field_num >= 2u && field_num <= 17u) {
            from_radio_tag_t tag = (from_radio_tag_t)field_num;

            if (out->variant_tag == FR_TAG_NONE) {
                out->variant_tag = tag;

                if (wire_type == WT_VARINT) {
                    // config_complete_id (uint32) or rebooted (bool)
                    uint64_t v;
                    if (!read_varint(buf, len, &pos, &v)) return false;
                    out->variant_value = (uint32_t)v;
                    continue;
                } else if (wire_type == WT_LEN) {
                    // length-delimited sub-message: record length, skip bytes
                    uint64_t sub_len;
                    if (!read_varint(buf, len, &pos, &sub_len)) return false;
                    if (sub_len > (uint64_t)(len - pos)) return false;
                    out->variant_value = (uint32_t)sub_len;
                    pos += (uint16_t)sub_len;
                    continue;
                } else {
                    // unexpected wire type for FromRadio fields
                    return false;
                }
            }
            // already captured a variant; skip any extras defensively
        }

        if (!skip_field(buf, len, &pos, wire_type)) {
            return false;
        }
    }
    return true;
}

// ── Helpers ─────────────────────────────────────────────────────────

static bool read_fixed32(const uint8_t *buf, uint16_t len, uint16_t *pos,
                         uint32_t *out)
{
    if (*pos + 4u > len) return false;
    *out = (uint32_t)buf[*pos]
         | ((uint32_t)buf[*pos + 1] << 8)
         | ((uint32_t)buf[*pos + 2] << 16)
         | ((uint32_t)buf[*pos + 3] << 24);
    *pos += 4u;
    return true;
}

// IEEE 754 little-endian → host float (assumes host is also LE on Cortex-M33).
static bool read_float(const uint8_t *buf, uint16_t len, uint16_t *pos,
                       float *out)
{
    uint32_t bits;
    if (!read_fixed32(buf, len, pos, &bits)) return false;
    union { uint32_t u; float f; } u = { .u = bits };
    *out = u.f;
    return true;
}

// Copy an LD field's bytes as a NUL-terminated C string into `dst`,
// truncating to dst_max-1 (always reserves room for the terminator).
static bool read_string_into(const uint8_t *buf, uint16_t len, uint16_t *pos,
                             char *dst, size_t dst_max)
{
    uint64_t slen;
    if (!read_varint(buf, len, pos, &slen)) return false;
    if (slen > (uint64_t)(len - *pos)) return false;
    size_t copy = (slen < (dst_max - 1u)) ? (size_t)slen : (dst_max - 1u);
    memcpy(dst, &buf[*pos], copy);
    dst[copy] = '\0';
    *pos += (uint16_t)slen;
    return true;
}

// Copy an LD field's raw bytes into `dst`, recording length (no NUL).
static bool read_bytes_into(const uint8_t *buf, uint16_t len, uint16_t *pos,
                            uint8_t *dst, uint8_t dst_max, uint8_t *out_len)
{
    uint64_t slen;
    if (!read_varint(buf, len, pos, &slen)) return false;
    if (slen > (uint64_t)(len - *pos)) return false;
    uint8_t copy = (slen < dst_max) ? (uint8_t)slen : dst_max;
    memcpy(dst, &buf[*pos], copy);
    *out_len = copy;
    *pos += (uint16_t)slen;
    return true;
}

// Walk LD-tagged fields; for each match against `inner_decoder`, hand
// the sub-message bytes to it. Used to decode sub-messages-of-sub-
// messages without an extra copy (e.g. NodeInfo.user, .device_metrics).
typedef bool (*sub_decoder_fn)(const uint8_t *buf, uint16_t len, void *ctx);

static bool dispatch_sub(const uint8_t *outer, uint16_t outer_len,
                         uint16_t *outer_pos,
                         sub_decoder_fn fn, void *ctx)
{
    uint64_t sub_len;
    if (!read_varint(outer, outer_len, outer_pos, &sub_len)) return false;
    if (sub_len > (uint64_t)(outer_len - *outer_pos)) return false;
    bool ok = fn(&outer[*outer_pos], (uint16_t)sub_len, ctx);
    *outer_pos += (uint16_t)sub_len;
    return ok;
}

// ── FromRadio variant locator ───────────────────────────────────────

const uint8_t *phoneapi_find_variant_payload(const uint8_t *buf, uint16_t len,
                                             from_radio_tag_t expected_tag,
                                             uint16_t *out_len)
{
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return NULL;
        uint32_t field_num = (uint32_t)(tag_word >> 3);
        uint8_t  wt        = (uint8_t)(tag_word & 0x07u);

        if ((from_radio_tag_t)field_num == expected_tag && wt == WT_LEN) {
            uint64_t sub_len;
            if (!read_varint(buf, len, &pos, &sub_len)) return NULL;
            if (sub_len > (uint64_t)(len - pos)) return NULL;
            *out_len = (uint16_t)sub_len;
            return &buf[pos];
        }

        if (!skip_field(buf, len, &pos, wt)) return NULL;
    }
    return NULL;
}

// ── MyNodeInfo decoder ──────────────────────────────────────────────
// Fields: my_node_num=1 v, reboot_count=8 v, min_app_version=11 v,
//         device_id=12 b,  pio_env=13 s,    firmware_edition=14 v,
//         nodedb_count=15 v.

bool phoneapi_decode_my_info(const uint8_t *buf, uint16_t len,
                             phoneapi_my_info_t *out)
{
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->my_node_num = (uint32_t)v;
        } else if (f == 8u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->reboot_count = (uint32_t)v;
        } else if (f == 11u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->min_app_version = (uint32_t)v;
        } else if (f == 12u && w == WT_LEN) {
            if (!read_bytes_into(buf, len, &pos, out->device_id,
                                 PHONEAPI_DEVICE_ID_MAX,
                                 &out->device_id_len)) return false;
        } else if (f == 13u && w == WT_LEN) {
            if (!read_string_into(buf, len, &pos, out->pio_env,
                                  PHONEAPI_PIO_ENV_MAX)) return false;
        } else if (f == 14u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->firmware_edition = (uint8_t)v;
        } else if (f == 15u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->nodedb_count = (uint32_t)v;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// ── DeviceMetadata decoder ──────────────────────────────────────────
// Fields: firmware_version=1 s, device_state_version=2 v,
//         canShutdown=3 b, hasWifi=4 b, hasBluetooth=5 b,
//         hasEthernet=6 b, role=7 v.

bool phoneapi_decode_metadata(const uint8_t *buf, uint16_t len,
                              phoneapi_metadata_t *out)
{
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_LEN) {
            if (!read_string_into(buf, len, &pos, out->firmware_version,
                                  PHONEAPI_FW_VERSION_MAX)) return false;
        } else if (f == 2u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->device_state_version = (uint32_t)v;
        } else if (f == 3u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->can_shutdown = (v != 0u);
        } else if (f == 4u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->has_wifi = (v != 0u);
        } else if (f == 5u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->has_bluetooth = (v != 0u);
        } else if (f == 6u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->has_ethernet = (v != 0u);
        } else if (f == 7u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->role = (uint8_t)v;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// ── ChannelSettings sub-decoder (used by Channel.settings) ──────────
// Fields: psk=2 b, name=3 s, id=4 fixed32 (others ignored).

typedef struct {
    char     *name;
    size_t    name_max;
    uint8_t  *psk_len;
    uint32_t *channel_id;
    /* B3-P2 — ChannelSettings.module_settings (channel.proto:95) */
    bool     *has_module_settings;
    uint32_t *module_position_precision;
    bool     *module_is_muted;
} chan_settings_ctx_t;

static bool decode_module_settings(const uint8_t *buf, uint16_t len, void *vctx)
{
    chan_settings_ctx_t *ctx = (chan_settings_ctx_t *)vctx;
    *ctx->has_module_settings = true;
    *ctx->module_position_precision = 0u;
    *ctx->module_is_muted = false;
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            *ctx->module_position_precision = (uint32_t)v;
        } else if (f == 2u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            *ctx->module_is_muted = (v != 0u);
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

static bool decode_channel_settings(const uint8_t *buf, uint16_t len, void *vctx)
{
    chan_settings_ctx_t *ctx = (chan_settings_ctx_t *)vctx;
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 2u && w == WT_LEN) {
            uint64_t plen;
            if (!read_varint(buf, len, &pos, &plen)) return false;
            if (plen > (uint64_t)(len - pos)) return false;
            *ctx->psk_len = (uint8_t)((plen > 0xFFu) ? 0xFFu : plen);
            pos += (uint16_t)plen;
        } else if (f == 3u && w == WT_LEN) {
            if (!read_string_into(buf, len, &pos, ctx->name, ctx->name_max))
                return false;
        } else if (f == 4u && w == WT_I32) {
            if (!read_fixed32(buf, len, &pos, ctx->channel_id)) return false;
        } else if (f == 7u && w == WT_LEN) {
            if (!dispatch_sub(buf, len, &pos, decode_module_settings, ctx))
                return false;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// ── Channel decoder ─────────────────────────────────────────────────
// Fields: index=1 v (int32), settings=2 LD, role=3 v.

bool phoneapi_decode_channel(const uint8_t *buf, uint16_t len,
                             phoneapi_channel_t *out)
{
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->index = (uint8_t)(v & 0xFFu);
        } else if (f == 2u && w == WT_LEN) {
            chan_settings_ctx_t ctx = {
                .name                       = out->name,
                .name_max                   = PHONEAPI_CHANNEL_NAME_MAX,
                .psk_len                    = &out->psk_len,
                .channel_id                 = &out->channel_id,
                .has_module_settings        = &out->has_module_settings,
                .module_position_precision  = &out->module_position_precision,
                .module_is_muted            = &out->module_is_muted,
            };
            if (!dispatch_sub(buf, len, &pos, decode_channel_settings, &ctx))
                return false;
        } else if (f == 3u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->role = (uint8_t)v;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// ── User sub-decoder (NodeInfo.user) ────────────────────────────────
// Fields: long_name=2 s, short_name=3 s, hw_model=5 v, role=7 v,
//         is_unmessagable=9 b.

static bool decode_user(const uint8_t *buf, uint16_t len, void *vctx)
{
    phoneapi_node_t *out = (phoneapi_node_t *)vctx;
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 2u && w == WT_LEN) {
            if (!read_string_into(buf, len, &pos, out->long_name,
                                  PHONEAPI_LONG_NAME_MAX)) return false;
        } else if (f == 3u && w == WT_LEN) {
            if (!read_string_into(buf, len, &pos, out->short_name,
                                  PHONEAPI_SHORT_NAME_MAX)) return false;
        } else if (f == 5u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->hw_model = (uint8_t)v;
        } else if (f == 6u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->is_licensed = (v != 0u);
        } else if (f == 7u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->role = (uint8_t)v;
        } else if (f == 8u && w == WT_LEN) {
            if (!read_bytes_into(buf, len, &pos, out->public_key,
                                 (uint8_t)sizeof(out->public_key),
                                 &out->public_key_len)) return false;
        } else if (f == 9u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->is_unmessagable = (v != 0u);
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// ── DeviceMetrics sub-decoder (NodeInfo.device_metrics) ─────────────
// Fields: battery_level=1 v, voltage=2 float, channel_utilization=3 f,
//         air_util_tx=4 f, uptime_seconds=5 v.

static bool decode_device_metrics(const uint8_t *buf, uint16_t len, void *vctx)
{
    phoneapi_node_t *out = (phoneapi_node_t *)vctx;
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->battery_level = (uint8_t)((v > 200u) ? 200u : v);
        } else if (f == 2u && w == WT_I32) {
            float fv;
            if (!read_float(buf, len, &pos, &fv)) return false;
            int32_t mv = (int32_t)(fv * 1000.0f);
            if (mv < INT16_MIN) mv = INT16_MIN;
            if (mv > INT16_MAX) mv = INT16_MAX;
            out->voltage_mv = (int16_t)mv;
        } else if (f == 3u && w == WT_I32) {
            float fv;
            if (!read_float(buf, len, &pos, &fv)) return false;
            if (fv < 0.0f) fv = 0.0f;
            if (fv > 100.0f) fv = 100.0f;
            out->channel_util_pct = (uint8_t)(fv + 0.5f);
        } else if (f == 4u && w == WT_I32) {
            float fv;
            if (!read_float(buf, len, &pos, &fv)) return false;
            if (fv < 0.0f) fv = 0.0f;
            if (fv > 100.0f) fv = 100.0f;
            out->air_util_tx_pct = (uint8_t)(fv + 0.5f);
        } else if (f == 5u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->uptime_seconds = (uint32_t)v;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// ── NodeInfo decoder ────────────────────────────────────────────────
// Fields: num=1 v, user=2 LD, position=3 LD (skipped), snr=4 float,
//         last_heard=5 fixed32, device_metrics=6 LD, channel=7 v,
//         via_mqtt=8 b, hops_away=9 v, is_favorite=10 b.

bool phoneapi_decode_node_info(const uint8_t *buf, uint16_t len,
                               phoneapi_node_t *out)
{
    memset(out, 0, sizeof(*out));
    out->battery_level    = 0xFFu;
    out->voltage_mv       = INT16_MIN;
    out->channel_util_pct = 0xFFu;
    out->air_util_tx_pct  = 0xFFu;
    out->hops_away        = 0xFFu;
    out->snr_x100         = INT32_MIN;

    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->num = (uint32_t)v;
        } else if (f == 2u && w == WT_LEN) {
            if (!dispatch_sub(buf, len, &pos, decode_user, out))
                return false;
        } else if (f == 4u && w == WT_I32) {
            float fv;
            if (!read_float(buf, len, &pos, &fv)) return false;
            int32_t s = (int32_t)(fv * 100.0f);
            out->snr_x100 = s;
        } else if (f == 5u && w == WT_I32) {
            uint32_t v; if (!read_fixed32(buf, len, &pos, &v)) return false;
            out->last_heard = v;
        } else if (f == 6u && w == WT_LEN) {
            if (!dispatch_sub(buf, len, &pos, decode_device_metrics, out))
                return false;
        } else if (f == 7u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->channel = (uint8_t)v;
        } else if (f == 8u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->via_mqtt = (v != 0u);
        } else if (f == 9u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->hops_away = (uint8_t)v;
        } else if (f == 10u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->is_favorite = (v != 0u);
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return out->num != 0u;
}

// ── Data sub-decoder (MeshPacket.decoded) ───────────────────────────
// Fields: portnum=1 v, payload=2 b. Other fields ignored.

typedef struct {
    uint32_t portnum;
    const uint8_t *payload;
    uint16_t       payload_len;
} data_subview_t;

static bool decode_data(const uint8_t *buf, uint16_t len, void *vctx)
{
    data_subview_t *out = (data_subview_t *)vctx;
    out->portnum = 0;
    out->payload = NULL;
    out->payload_len = 0;
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->portnum = (uint32_t)v;
        } else if (f == 2u && w == WT_LEN) {
            uint64_t plen;
            if (!read_varint(buf, len, &pos, &plen)) return false;
            if (plen > (uint64_t)(len - pos)) return false;
            out->payload     = &buf[pos];
            out->payload_len = (uint16_t)plen;
            pos += (uint16_t)plen;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// ── MeshPacket → text decoder ───────────────────────────────────────
// Fields: from=1 fixed32, to=2 fixed32, channel=3 v, decoded=4 LD.

bool phoneapi_decode_text_packet(const uint8_t *buf, uint16_t len,
                                 phoneapi_text_msg_t *out_msg)
{
    memset(out_msg, 0, sizeof(*out_msg));
    /* A3 — sentinels for "field absent on wire". */
    out_msg->rx_snr_x4 = INT16_MIN;
    out_msg->rx_rssi   = 0;
    out_msg->hop_limit = 0xFFu;
    out_msg->hop_start = 0xFFu;

    data_subview_t data = { 0, NULL, 0 };
    bool           have_data = false;

    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_I32) {
            uint32_t v; if (!read_fixed32(buf, len, &pos, &v)) return false;
            out_msg->from_node_id = v;
        } else if (f == 2u && w == WT_I32) {
            uint32_t v; if (!read_fixed32(buf, len, &pos, &v)) return false;
            out_msg->to_node_id = v;
        } else if (f == 3u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out_msg->channel_index = (uint8_t)v;
        } else if (f == 4u && w == WT_LEN) {
            if (!dispatch_sub(buf, len, &pos, decode_data, &data)) return false;
            have_data = true;
        } else if (f == 8u && w == WT_I32) {
            /* MeshPacket.rx_snr (mesh.proto:1591) — float dB.
             * Stored as int16 × 4 dB so dm_msg has uniform metadata
             * without bringing float into the on-air struct. */
            float snr;
            if (!read_float(buf, len, &pos, &snr)) return false;
            float scaled = snr * 4.0f;
            int32_t v = (int32_t)(scaled >= 0.0f ? scaled + 0.5f
                                                  : scaled - 0.5f);
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            out_msg->rx_snr_x4 = (int16_t)v;
        } else if (f == 9u && w == WT_VARINT) {
            /* MeshPacket.hop_limit (mesh.proto:1599) — uint32. */
            uint64_t v;
            if (!read_varint(buf, len, &pos, &v)) return false;
            out_msg->hop_limit = (v > 0xFFu) ? 0xFFu : (uint8_t)v;
        } else if (f == 12u && w == WT_VARINT) {
            /* MeshPacket.rx_rssi (mesh.proto:1623) — int32 dBm signed
             * varint (two's complement). */
            uint64_t v;
            if (!read_varint(buf, len, &pos, &v)) return false;
            int32_t s = (int32_t)(int64_t)v;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            out_msg->rx_rssi = (int16_t)s;
        } else if (f == 15u && w == WT_VARINT) {
            /* MeshPacket.hop_start (mesh.proto:1639) — uint32. */
            uint64_t v;
            if (!read_varint(buf, len, &pos, &v)) return false;
            out_msg->hop_start = (v > 0xFFu) ? 0xFFu : (uint8_t)v;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }

    if (!have_data || data.portnum != PHONEAPI_PORTNUM_TEXT_MESSAGE_APP) {
        return false;
    }
    uint16_t copy = (data.payload_len > PHONEAPI_MSG_TEXT_MAX)
                        ? PHONEAPI_MSG_TEXT_MAX
                        : data.payload_len;
    if (copy > 0u && data.payload != NULL) {
        memcpy(out_msg->text, data.payload, copy);
    }
    out_msg->text_len = copy;
    return true;
}

// ── Routing(ACK) decoder ────────────────────────────────────────────
// Wire layout (Meshtastic mesh.proto):
//   MeshPacket {
//     fixed32 from = 1;
//     fixed32 to   = 2;
//     uint32  channel = 3;
//     Data    decoded = 4;       // LD, sub-message
//     ...
//   }
//   Data {
//     PortNum portnum = 1;        // varint; 5 = ROUTING_APP
//     bytes   payload = 2;        // LD; Routing protobuf
//     bool    want_response = 3;
//     fixed32 dest = 4;
//     fixed32 source = 5;
//     fixed32 request_id = 6;     // the packet id this ack refers to
//     ...
//   }
//   Routing {                     // payload bytes inside Data.payload
//     oneof variant {
//       RouteDiscovery route_request = 1;
//       RouteDiscovery route_reply  = 2;
//       Error          error_reason = 3;  // varint; 0 = NONE (delivered ok)
//     }
//   }

typedef struct {
    uint32_t portnum;
    const uint8_t *payload;
    uint16_t       payload_len;
    uint32_t       request_id;
    bool           have_request_id;
} routing_data_view_t;

static bool decode_routing_data(const uint8_t *buf, uint16_t len, void *vctx)
{
    routing_data_view_t *out = (routing_data_view_t *)vctx;
    out->portnum = 0;
    out->payload = NULL;
    out->payload_len = 0;
    out->request_id = 0;
    out->have_request_id = false;
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->portnum = (uint32_t)v;
        } else if (f == 2u && w == WT_LEN) {
            uint64_t plen;
            if (!read_varint(buf, len, &pos, &plen)) return false;
            if (plen > (uint64_t)(len - pos)) return false;
            out->payload     = &buf[pos];
            out->payload_len = (uint16_t)plen;
            pos += (uint16_t)plen;
        } else if (f == 6u && w == WT_I32) {
            uint32_t v; if (!read_fixed32(buf, len, &pos, &v)) return false;
            out->request_id = v;
            out->have_request_id = true;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

static bool decode_routing_inner(const uint8_t *buf, uint16_t len,
                                 uint8_t *out_error_reason)
{
    *out_error_reason = 0u;
    uint16_t pos = 0;
    bool found = false;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 3u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            *out_error_reason = (uint8_t)v;
            found = true;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return found;
}

bool phoneapi_decode_routing_ack(const uint8_t *buf, uint16_t len,
                                 uint32_t *out_request_id,
                                 uint8_t  *out_error_reason)
{
    *out_request_id = 0u;
    *out_error_reason = 0u;

    routing_data_view_t data = { 0, NULL, 0, 0, false };
    bool have_data = false;

    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 4u && w == WT_LEN) {
            if (!dispatch_sub(buf, len, &pos, decode_routing_data, &data))
                return false;
            have_data = true;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }

    if (!have_data) return false;
    if (data.portnum != PHONEAPI_PORTNUM_ROUTING_APP) return false;
    if (!data.have_request_id) return false;
    if (data.payload == NULL || data.payload_len == 0u) {
        // Routing with empty payload = error_reason field absent = NONE.
        *out_request_id = data.request_id;
        *out_error_reason = 0u;
        return true;
    }
    uint8_t err = 0u;
    (void)decode_routing_inner(data.payload, data.payload_len, &err);
    *out_request_id = data.request_id;
    *out_error_reason = err;
    return true;
}

// ── QueueStatus decoder ─────────────────────────────────────────────
// Wire (mesh.proto):
//   message QueueStatus {
//     int32  res = 1;             // varint, signed (Routing.Error)
//     uint32 free = 2;            // varint
//     uint32 maxlen = 3;          // varint
//     uint32 mesh_packet_id = 4;  // varint
//   }

bool phoneapi_decode_queue_status(const uint8_t *buf, uint16_t len,
                                  phoneapi_queue_status_t *out)
{
    if (out == NULL) return false;
    out->res = 0;
    out->free = 0u;
    out->maxlen = 0u;
    out->mesh_packet_id = 0u;
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->res = (int32_t)(uint32_t)v;
        } else if (f == 2u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->free = (uint32_t)v;
        } else if (f == 3u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->maxlen = (uint32_t)v;
        } else if (f == 4u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->mesh_packet_id = (uint32_t)v;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// ── RouteDiscovery decoder (TRACEROUTE_APP, portnum 70) ─────────────
//
// RouteDiscovery (mesh.proto):
//   repeated fixed32 route        = 1;  // forward-path node nums
//   repeated int32   snr_towards  = 2;  // dB × 4, parallel to route[]
//   repeated fixed32 route_back   = 3;  // return-path node nums
//   repeated int32   snr_back     = 4;  // dB × 4, parallel to route_back[]
//
// Repeated scalar fields are usually **packed** on the wire (single LD
// element with a concatenated stream of values), but Meshtastic's older
// senders sometimes emit non-packed (one LD/varint per element). We
// handle both forms.

static int8_t saturate_snr_x4_to_i8(int32_t v)
{
    if (v >  127) return  127;
    if (v < -128) return -128;
    return (int8_t)v;
}

static bool decode_route_discovery_inner(const uint8_t *buf, uint16_t len,
                                         phoneapi_last_route_t *out)
{
    /* Caller pre-zeroed *out. We append into hops_full / hops_back_full /
     * snr arrays in source order, capping at PHONEAPI_ROUTE_HOPS_MAX. */
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if ((f == 1u || f == 3u) && w == WT_LEN) {
            // Packed repeated fixed32 — N × 4 bytes.
            uint64_t plen;
            if (!read_varint(buf, len, &pos, &plen)) return false;
            if (plen > (uint64_t)(len - pos)) return false;
            uint16_t end = pos + (uint16_t)plen;
            uint32_t *dst = (f == 1u) ? out->hops_full : out->hops_back_full;
            uint8_t  *cnt = (f == 1u) ? &out->hop_count : &out->hops_back_count;
            while (pos + 4u <= end) {
                uint32_t v;
                if (!read_fixed32(buf, len, &pos, &v)) return false;
                if (*cnt < PHONEAPI_ROUTE_HOPS_MAX) {
                    dst[*cnt] = v;
                    (*cnt)++;
                }
            }
            if (pos != end) return false;
        } else if ((f == 1u || f == 3u) && w == WT_I32) {
            // Non-packed repeated fixed32 — single value.
            uint32_t v;
            if (!read_fixed32(buf, len, &pos, &v)) return false;
            uint32_t *dst = (f == 1u) ? out->hops_full : out->hops_back_full;
            uint8_t  *cnt = (f == 1u) ? &out->hop_count : &out->hops_back_count;
            if (*cnt < PHONEAPI_ROUTE_HOPS_MAX) {
                dst[*cnt] = v;
                (*cnt)++;
            }
        } else if ((f == 2u || f == 4u) && w == WT_LEN) {
            // Packed repeated int32 (varint, two's-complement). Walk the
            // packed payload and pick off varints in parallel order with
            // route[] / route_back[].
            uint64_t plen;
            if (!read_varint(buf, len, &pos, &plen)) return false;
            if (plen > (uint64_t)(len - pos)) return false;
            uint16_t end = pos + (uint16_t)plen;
            int8_t   *dst = (f == 2u) ? out->snr_fwd : out->snr_back;
            uint8_t   k   = 0;
            while (pos < end) {
                uint64_t v;
                if (!read_varint(buf, len, &pos, &v)) return false;
                if (k < PHONEAPI_ROUTE_HOPS_MAX) {
                    dst[k] = saturate_snr_x4_to_i8((int32_t)(int64_t)v);
                    k++;
                }
            }
            if (pos != end) return false;
        } else if ((f == 2u || f == 4u) && w == WT_VARINT) {
            // Non-packed repeated int32 — single value. Place at the
            // next slot in the parallel array (best-effort — without
            // explicit indexing we assume non-packed senders interleave
            // route+snr pairs in source order, which matches Meshtastic
            // RouteDiscoveryModule's emission pattern).
            uint64_t v;
            if (!read_varint(buf, len, &pos, &v)) return false;
            int8_t   *dst = (f == 2u) ? out->snr_fwd : out->snr_back;
            uint8_t   k   = (f == 2u) ? out->hop_count
                                      : out->hops_back_count;
            // Drop into the slot for the just-pushed hop entry.
            uint8_t   slot = (k > 0u) ? (uint8_t)(k - 1u) : 0u;
            if (slot < PHONEAPI_ROUTE_HOPS_MAX) {
                dst[slot] = saturate_snr_x4_to_i8((int32_t)(int64_t)v);
            }
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

bool phoneapi_decode_traceroute_packet(const uint8_t *buf, uint16_t len,
                                       uint32_t *out_from_node,
                                       phoneapi_last_route_t *out_route)
{
    if (out_from_node == NULL || out_route == NULL) return false;
    *out_from_node = 0u;
    memset(out_route, 0, sizeof(*out_route));
    /* Mark all SNR slots unknown up-front; the decoder only fills
     * slots it actually saw. */
    for (uint8_t i = 0; i < PHONEAPI_ROUTE_HOPS_MAX; i++) {
        out_route->snr_fwd[i]  = INT8_MIN;
        out_route->snr_back[i] = INT8_MIN;
    }

    data_subview_t data = { 0, NULL, 0 };
    bool have_data = false;
    uint32_t from = 0u;

    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_I32) {
            uint32_t v; if (!read_fixed32(buf, len, &pos, &v)) return false;
            from = v;
        } else if (f == 4u && w == WT_LEN) {
            if (!dispatch_sub(buf, len, &pos, decode_data, &data)) return false;
            have_data = true;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    if (!have_data || data.portnum != PHONEAPI_PORTNUM_TRACEROUTE_APP) {
        return false;
    }
    *out_from_node = from;
    if (data.payload != NULL && data.payload_len > 0u) {
        if (!decode_route_discovery_inner(data.payload, data.payload_len,
                                           out_route)) {
            return false;
        }
    }
    return true;
}

// ── Position decoder (POSITION_APP, portnum 3) ──────────────────────
//
// Position (mesh.proto): we want only
//   sfixed32 latitude_i  = 1;  // degrees × 1e7
//   sfixed32 longitude_i = 2;  // degrees × 1e7
//   int32    altitude    = 3;  // metres (varint, two's-complement)
//   fixed32  time        = 4;  // unix epoch seconds (when fix taken)

static bool decode_position_inner(const uint8_t *buf, uint16_t len,
                                  phoneapi_last_position_t *out)
{
    out->lat_e7 = 0;
    out->lon_e7 = 0;
    out->alt_m  = INT32_MIN;
    out->epoch  = 0u;
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_I32) {
            uint32_t v; if (!read_fixed32(buf, len, &pos, &v)) return false;
            out->lat_e7 = (int32_t)v;
        } else if (f == 2u && w == WT_I32) {
            uint32_t v; if (!read_fixed32(buf, len, &pos, &v)) return false;
            out->lon_e7 = (int32_t)v;
        } else if (f == 3u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->alt_m = (int32_t)(int64_t)v;
        } else if (f == 4u && w == WT_I32) {
            uint32_t v; if (!read_fixed32(buf, len, &pos, &v)) return false;
            out->epoch = v;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

bool phoneapi_decode_position_packet(const uint8_t *buf, uint16_t len,
                                     uint32_t *out_from_node,
                                     phoneapi_last_position_t *out_pos)
{
    if (out_from_node == NULL || out_pos == NULL) return false;
    *out_from_node = 0u;
    memset(out_pos, 0, sizeof(*out_pos));
    out_pos->alt_m = INT32_MIN;

    data_subview_t data = { 0, NULL, 0 };
    bool have_data = false;
    uint32_t from = 0u;

    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_I32) {
            uint32_t v; if (!read_fixed32(buf, len, &pos, &v)) return false;
            from = v;
        } else if (f == 4u && w == WT_LEN) {
            if (!dispatch_sub(buf, len, &pos, decode_data, &data)) return false;
            have_data = true;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    if (!have_data || data.portnum != PHONEAPI_PORTNUM_POSITION_APP) {
        return false;
    }
    *out_from_node = from;
    if (data.payload != NULL && data.payload_len > 0u) {
        if (!decode_position_inner(data.payload, data.payload_len, out_pos)) {
            return false;
        }
    }
    return true;
}

// ── NeighborInfo decoder (NEIGHBORINFO_APP, portnum 71) ─────────────
//
// NeighborInfo (mesh.proto):
//   uint32 node_id                          = 1;  // source peer
//   uint32 last_sent_by_id                  = 2;
//   uint32 node_broadcast_interval_secs     = 3;
//   repeated Neighbor neighbors             = 4;
// Neighbor:
//   uint32 node_id                          = 1;
//   float  snr                              = 2;  // dB, IEEE-754 fixed32
//   fixed32 last_rx_time                    = 3;  // deprecated
//   uint32 node_broadcast_interval_secs     = 4;
//
// The outer NeighborInfo is the decoded.payload of a MeshPacket whose
// decoded.portnum == 71. We only care about the `neighbors` list — the
// outer node_id duplicates MeshPacket.from anyway.

static int8_t saturate_snr_x4_from_float(float snr_db)
{
    float v = snr_db * 4.0f;
    if (v >  127.0f) return  127;
    if (v < -128.0f) return -128;
    return (int8_t)v;
}

static bool decode_neighbor_inner(const uint8_t *buf, uint16_t len,
                                  phoneapi_neighbor_entry_t *out)
{
    out->node_num = 0u;
    out->snr_x4   = INT8_MIN;
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->node_num = (uint32_t)v;
        } else if (f == 2u && w == WT_I32) {
            uint32_t u; if (!read_fixed32(buf, len, &pos, &u)) return false;
            union { uint32_t u; float f; } pun;
            pun.u = u;
            out->snr_x4 = saturate_snr_x4_from_float(pun.f);
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

static bool decode_neighborinfo_inner(const uint8_t *buf, uint16_t len,
                                      phoneapi_neighbors_t *out)
{
    /* Caller pre-zeroed *out. Append into entries[] capping at
     * PHONEAPI_NEIGHBORS_MAX. Other fields (last_sent_by_id, intervals)
     * are skipped — F-3 doesn't surface them. */
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 4u && w == WT_LEN) {
            uint64_t plen;
            if (!read_varint(buf, len, &pos, &plen)) return false;
            if (plen > (uint64_t)(len - pos)) return false;
            phoneapi_neighbor_entry_t e;
            if (!decode_neighbor_inner(buf + pos, (uint16_t)plen, &e)) {
                return false;
            }
            pos += (uint16_t)plen;
            if (out->count < PHONEAPI_NEIGHBORS_MAX && e.node_num != 0u) {
                out->entries[out->count++] = e;
            }
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

bool phoneapi_decode_neighborinfo_packet(const uint8_t *buf, uint16_t len,
                                          uint32_t *out_from_node,
                                          phoneapi_neighbors_t *out_nb)
{
    if (out_from_node == NULL || out_nb == NULL) return false;
    *out_from_node = 0u;
    memset(out_nb, 0, sizeof(*out_nb));

    data_subview_t data = { 0, NULL, 0 };
    bool have_data = false;
    uint32_t from = 0u;
    uint32_t rx_time = 0u;

    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_I32) {
            uint32_t v; if (!read_fixed32(buf, len, &pos, &v)) return false;
            from = v;
        } else if (f == 4u && w == WT_LEN) {
            if (!dispatch_sub(buf, len, &pos, decode_data, &data)) return false;
            have_data = true;
        } else if (f == 8u && w == WT_I32) {
            /* MeshPacket.rx_time — useful as freshness epoch for F-3
             * "heard N s ago" column when the host clock is sync'd. */
            uint32_t v; if (!read_fixed32(buf, len, &pos, &v)) return false;
            rx_time = v;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    if (!have_data || data.portnum != PHONEAPI_PORTNUM_NEIGHBORINFO_APP) {
        return false;
    }
    *out_from_node = from;
    out_nb->epoch = (rx_time != 0u) ? rx_time : 1u;
    if (data.payload != NULL && data.payload_len > 0u) {
        if (!decode_neighborinfo_inner(data.payload, data.payload_len,
                                       out_nb)) {
            return false;
        }
    }
    return true;
}

// ── Config sub-oneof decoders (B3-P1 / Cut B) ───────────────────────
//
// Each input buffer is the raw DeviceConfig / LoRaConfig / PositionConfig
// / DisplayConfig sub-message — i.e. the bytes inside the outer
// Config oneof's LD wrapper. The caller (phoneapi_session.on_frame
// FR_TAG_CONFIG handler) is responsible for stripping the FromRadio
// frame wrapper *and* the Config oneof tag before invoking these.
//
// Field numbers below mirror config.proto verbatim — see
// firmware/core0/meshtastic/protobufs/meshtastic/config.proto. Keep
// in sync with ipc_config_handler.cpp on Core 0; both ends consume
// the same wire facts.

// DeviceConfig — config.proto:202–264
//   role=1, serial_enabled=2 (deprecated), button_gpio=4, buzzer_gpio=5,
//   rebroadcast_mode=6, node_info_broadcast_secs=7,
//   double_tap_as_button_press=8, is_managed=9 (deprecated),
//   disable_triple_click=10, tzdef=11, led_heartbeat_disabled=12,
//   buzzer_mode=13. B3-P1 surface: 1, 6, 7, 8, 10, 11, 12.
bool phoneapi_decode_config_device(const uint8_t *buf, uint16_t len,
                                   phoneapi_config_device_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->role = (uint8_t)v;
        } else if (f == 6u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->rebroadcast_mode = (uint8_t)v;
        } else if (f == 7u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->node_info_broadcast_secs = (uint32_t)v;
        } else if (f == 8u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->double_tap_as_button_press = (v != 0u);
        } else if (f == 10u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->disable_triple_click = (v != 0u);
        } else if (f == 11u && w == WT_LEN) {
            if (!read_string_into(buf, len, &pos,
                                  out->tzdef, PHONEAPI_TZDEF_MAX)) return false;
        } else if (f == 12u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->led_heartbeat_disabled = (v != 0u);
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// LoRaConfig — config.proto:1012–1135
//   use_preset=1, modem_preset=2, bandwidth=3, spread_factor=4,
//   coding_rate=5, frequency_offset=6 (float, skipped), region=7,
//   hop_limit=8, tx_enabled=9, tx_power=10 (int32), channel_num=11,
//   override_duty_cycle=12, sx126x_rx_boosted_gain=13,
//   override_frequency=14 (float, skipped), pa_fan_disabled=15,
//   ignore_incoming=103 (repeated, skipped), ignore_mqtt=104,
//   config_ok_to_mqtt=105, fem_lna_mode=106.
bool phoneapi_decode_config_lora(const uint8_t *buf, uint16_t len,
                                 phoneapi_config_lora_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->use_preset = (v != 0u);
        } else if (f == 2u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->modem_preset = (uint8_t)v;
        } else if (f == 3u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->bandwidth = (uint32_t)v;
        } else if (f == 4u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->spread_factor = (uint32_t)v;
        } else if (f == 5u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->coding_rate = (uint32_t)v;
        } else if (f == 7u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->region = (uint8_t)v;
        } else if (f == 8u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->hop_limit = (uint32_t)v;
        } else if (f == 9u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->tx_enabled = (v != 0u);
        } else if (f == 10u && w == WT_VARINT) {
            // int32 — proto3 encodes negatives as 10-byte varint of two's complement
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->tx_power = (int32_t)(uint32_t)v;
        } else if (f == 11u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->channel_num = (uint32_t)v;
        } else if (f == 12u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->override_duty_cycle = (v != 0u);
        } else if (f == 13u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->sx126x_rx_boosted_gain = (v != 0u);
        } else if (f == 106u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->fem_lna_mode = (uint8_t)v;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// PositionConfig — config.proto:361–426
//   position_broadcast_secs=1, position_broadcast_smart_enabled=2,
//   fixed_position=3, gps_enabled=4 (deprecated), gps_update_interval=5,
//   gps_attempt_time=6 (deprecated), position_flags=7, rx_gpio=8,
//   tx_gpio=9, broadcast_smart_minimum_distance=10,
//   broadcast_smart_minimum_interval_secs=11, gps_en_gpio=12, gps_mode=13.
bool phoneapi_decode_config_position(const uint8_t *buf, uint16_t len,
                                     phoneapi_config_position_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->position_broadcast_secs = (uint32_t)v;
        } else if (f == 2u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->position_broadcast_smart_enabled = (v != 0u);
        } else if (f == 3u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->fixed_position = (v != 0u);
        } else if (f == 5u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->gps_update_interval = (uint32_t)v;
        } else if (f == 7u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->position_flags = (uint32_t)v;
        } else if (f == 10u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->broadcast_smart_minimum_distance = (uint32_t)v;
        } else if (f == 11u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->broadcast_smart_minimum_interval_secs = (uint32_t)v;
        } else if (f == 13u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->gps_mode = (uint8_t)v;
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// DisplayConfig — config.proto:657–790
//   screen_on_secs=1, gps_format=2 (deprecated),
//   auto_screen_carousel_secs=3, compass_north_top=4 (deprecated),
//   flip_screen=5, units=6, oled=7, displaymode=8, heading_bold=9,
//   wake_on_tap_or_motion=10, compass_orientation=11, use_12h_clock=12,
//   use_long_node_name=13, enable_message_bubbles=14.
bool phoneapi_decode_config_display(const uint8_t *buf, uint16_t len,
                                    phoneapi_config_display_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->screen_on_secs = (uint32_t)v;
        } else if (f == 3u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->auto_screen_carousel_secs = (uint32_t)v;
        } else if (f == 5u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->flip_screen = (v != 0u);
        } else if (f == 6u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->units = (uint8_t)v;
        } else if (f == 7u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->oled = (uint8_t)v;
        } else if (f == 8u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->displaymode = (uint8_t)v;
        } else if (f == 9u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->heading_bold = (v != 0u);
        } else if (f == 10u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->wake_on_tap_or_motion = (v != 0u);
        } else if (f == 11u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->compass_orientation = (uint8_t)v;
        } else if (f == 12u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->use_12h_clock = (v != 0u);
        } else if (f == 13u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->use_long_node_name = (v != 0u);
        } else if (f == 14u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->enable_message_bubbles = (v != 0u);
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// PowerConfig — config.proto:433–490
//   is_power_saving=1, on_battery_shutdown_after_secs=2,
//   adc_multiplier_override=3 (float, skipped), wait_bluetooth_secs=4,
//   sds_secs=6, ls_secs=7, min_wake_secs=8,
//   device_battery_ina_address=9, powermon_enables=32 (uint64).
bool phoneapi_decode_config_power(const uint8_t *buf, uint16_t len,
                                  phoneapi_config_power_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->is_power_saving = (v != 0u);
        } else if (f == 2u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->on_battery_shutdown_after_secs = (uint32_t)v;
        } else if (f == 4u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->wait_bluetooth_secs = (uint32_t)v;
        } else if (f == 6u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->sds_secs = (uint32_t)v;
        } else if (f == 7u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->ls_secs = (uint32_t)v;
        } else if (f == 8u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->min_wake_secs = (uint32_t)v;
        } else if (f == 9u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->device_battery_ina_address = (uint32_t)v;
        } else if (f == 32u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            /* uint64 — surface only low 32 bits to match IPC scope. */
            out->powermon_enables_lo = (uint32_t)(v & 0xFFFFFFFFu);
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// SecurityConfig — config.proto:1172–1211
//   public_key=1 (bytes), private_key=2 (bytes — NOT decoded into cache),
//   admin_key=3 (repeated bytes — NOT decoded), is_managed=4,
//   serial_enabled=5, debug_log_api_enabled=6, admin_channel_enabled=8.
bool phoneapi_decode_config_security(const uint8_t *buf, uint16_t len,
                                     phoneapi_config_security_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t f = (uint32_t)(tag_word >> 3);
        uint8_t  w = (uint8_t)(tag_word & 7u);

        if (f == 1u && w == WT_LEN) {
            if (!read_bytes_into(buf, len, &pos, out->public_key,
                                 (uint8_t)sizeof(out->public_key),
                                 &out->public_key_len)) return false;
        } else if (f == 4u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->is_managed = (v != 0u);
        } else if (f == 5u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->serial_enabled = (v != 0u);
        } else if (f == 6u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->debug_log_api_enabled = (v != 0u);
        } else if (f == 8u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->admin_channel_enabled = (v != 0u);
        } else {
            if (!skip_field(buf, len, &pos, w)) return false;
        }
    }
    return true;
}

// ── ModuleConfig sub-oneof decoders (B3 cascade walk-down) ──────────
// Field numbers from module_config.proto. Only fields exposed through
// the IpcConfigKey 0x10xx — 0x16xx namespace are decoded; everything
// else is skip_field'd. See ipc_config_handler.cpp for the wire-side
// authoritative table of which fields land in flash.

// TelemetryConfig (proto:625)
//   1 device_update_interval, 2 environment_update_interval,
//   3 environment_measurement_enabled, 4 environment_screen_enabled,
//   5 environment_display_fahrenheit, 8 power_measurement_enabled,
//   9 power_update_interval, 10 power_screen_enabled,
//  14 device_telemetry_enabled
bool phoneapi_decode_module_telemetry(const uint8_t *buf, uint16_t len,
                                      phoneapi_module_telemetry_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tw; if (!read_varint(buf, len, &pos, &tw)) return false;
        uint32_t f = (uint32_t)(tw >> 3);
        uint8_t  w = (uint8_t)(tw & 7u);
        uint64_t v;
        if (w == WT_VARINT) {
            if (!read_varint(buf, len, &pos, &v)) return false;
            switch (f) {
            case 1u:  out->device_update_interval           = (uint32_t)v; break;
            case 2u:  out->environment_update_interval      = (uint32_t)v; break;
            case 3u:  out->environment_measurement_enabled  = (v != 0u);   break;
            case 4u:  out->environment_screen_enabled       = (v != 0u);   break;
            case 5u:  out->environment_display_fahrenheit   = (v != 0u);   break;
            case 8u:  out->power_measurement_enabled        = (v != 0u);   break;
            case 9u:  out->power_update_interval            = (uint32_t)v; break;
            case 10u: out->power_screen_enabled             = (v != 0u);   break;
            case 14u: out->device_telemetry_enabled         = (v != 0u);   break;
            default:  break;  /* skip unknown varint fields silently */
            }
        } else if (!skip_field(buf, len, &pos, w)) {
            return false;
        }
    }
    return true;
}

// NeighborInfoConfig (proto:130)
//   1 enabled, 2 update_interval, 3 transmit_over_lora
bool phoneapi_decode_module_neighbor(const uint8_t *buf, uint16_t len,
                                     phoneapi_module_neighbor_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tw; if (!read_varint(buf, len, &pos, &tw)) return false;
        uint32_t f = (uint32_t)(tw >> 3);
        uint8_t  w = (uint8_t)(tw & 7u);
        if (w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            switch (f) {
            case 1u: out->enabled            = (v != 0u);   break;
            case 2u: out->update_interval    = (uint32_t)v; break;
            case 3u: out->transmit_over_lora = (v != 0u);   break;
            default: break;
            }
        } else if (!skip_field(buf, len, &pos, w)) {
            return false;
        }
    }
    return true;
}

// RangeTestConfig (proto:598) — 1 enabled, 2 sender. Fields 3 (save)
// and 4 (clear_on_reboot) are ESP32-only, skipped.
bool phoneapi_decode_module_range_test(const uint8_t *buf, uint16_t len,
                                       phoneapi_module_range_test_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tw; if (!read_varint(buf, len, &pos, &tw)) return false;
        uint32_t f = (uint32_t)(tw >> 3);
        uint8_t  w = (uint8_t)(tw & 7u);
        if (w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            if      (f == 1u) out->enabled = (v != 0u);
            else if (f == 2u) out->sender  = (uint32_t)v;
        } else if (!skip_field(buf, len, &pos, w)) {
            return false;
        }
    }
    return true;
}

// DetectionSensorConfig (proto:152)
//   1 enabled, 2 minimum_broadcast_secs, 3 state_broadcast_secs,
//   5 name (string ≤19+NUL), 7 detection_trigger_type, 8 use_pullup.
//   Field 4 send_bell, 6 monitor_pin not exposed.
bool phoneapi_decode_module_detect(const uint8_t *buf, uint16_t len,
                                   phoneapi_module_detect_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tw; if (!read_varint(buf, len, &pos, &tw)) return false;
        uint32_t f = (uint32_t)(tw >> 3);
        uint8_t  w = (uint8_t)(tw & 7u);
        if (w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            switch (f) {
            case 1u: out->enabled                 = (v != 0u);   break;
            case 2u: out->minimum_broadcast_secs  = (uint32_t)v; break;
            case 3u: out->state_broadcast_secs    = (uint32_t)v; break;
            case 7u: out->detection_trigger_type  = (uint8_t)(v & 0xFFu); break;
            case 8u: out->use_pullup              = (v != 0u);   break;
            default: break;
            }
        } else if (f == 5u && w == WT_LEN) {
            if (!read_string_into(buf, len, &pos, out->name, sizeof(out->name)))
                return false;
        } else if (!skip_field(buf, len, &pos, w)) {
            return false;
        }
    }
    return true;
}

// CannedMessageConfig (proto:715) — 8 updown1_enabled, 11 send_bell.
// Rotary fields and deprecated fields skipped.
bool phoneapi_decode_module_canned_msg(const uint8_t *buf, uint16_t len,
                                       phoneapi_module_canned_msg_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tw; if (!read_varint(buf, len, &pos, &tw)) return false;
        uint32_t f = (uint32_t)(tw >> 3);
        uint8_t  w = (uint8_t)(tw & 7u);
        if (w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            if      (f ==  8u) out->updown1_enabled = (v != 0u);
            else if (f == 11u) out->send_bell       = (v != 0u);
        } else if (!skip_field(buf, len, &pos, w)) {
            return false;
        }
    }
    return true;
}

// AmbientLightingConfig (proto:823)
//   1 led_state (bool), 2 current, 3 red, 4 green, 5 blue (all uint32 on
//   the wire, clamped to u8 0..255 by the IPC handler — proto declares
//   uint32 even though nanopb stores u8).
bool phoneapi_decode_module_ambient(const uint8_t *buf, uint16_t len,
                                    phoneapi_module_ambient_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tw; if (!read_varint(buf, len, &pos, &tw)) return false;
        uint32_t f = (uint32_t)(tw >> 3);
        uint8_t  w = (uint8_t)(tw & 7u);
        if (w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            uint8_t  vu8 = (uint8_t)((v > 255ull) ? 255u : v);
            switch (f) {
            case 1u: out->led_state = (v != 0u); break;
            case 2u: out->current   = vu8;       break;
            case 3u: out->red       = vu8;       break;
            case 4u: out->green     = vu8;       break;
            case 5u: out->blue      = vu8;       break;
            default: break;
            }
        } else if (!skip_field(buf, len, &pos, w)) {
            return false;
        }
    }
    return true;
}

// PaxcounterConfig (proto:276) — 1 enabled, 2 paxcounter_update_interval.
// Fields 3, 4 (RSSI thresholds) skipped.
bool phoneapi_decode_module_paxcounter(const uint8_t *buf, uint16_t len,
                                       phoneapi_module_paxcounter_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tw; if (!read_varint(buf, len, &pos, &tw)) return false;
        uint32_t f = (uint32_t)(tw >> 3);
        uint8_t  w = (uint8_t)(tw & 7u);
        if (w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            if      (f == 1u) out->enabled                    = (v != 0u);
            else if (f == 2u) out->paxcounter_update_interval = (uint32_t)v;
        } else if (!skip_field(buf, len, &pos, w)) {
            return false;
        }
    }
    return true;
}

// StoreForwardConfig (proto:563) — 6 fields, all bool/uint32. T2.4.1.
//   1 enabled, 2 heartbeat, 3 records, 4 history_return_max,
//   5 history_return_window, 6 is_server
bool phoneapi_decode_module_store_forward(const uint8_t *buf, uint16_t len,
                                          phoneapi_module_store_forward_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tw; if (!read_varint(buf, len, &pos, &tw)) return false;
        uint32_t f = (uint32_t)(tw >> 3);
        uint8_t  w = (uint8_t)(tw & 7u);
        if (w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            switch (f) {
            case 1u: out->enabled               = (v != 0u);   break;
            case 2u: out->heartbeat             = (v != 0u);   break;
            case 3u: out->records               = (uint32_t)v; break;
            case 4u: out->history_return_max    = (uint32_t)v; break;
            case 5u: out->history_return_window = (uint32_t)v; break;
            case 6u: out->is_server             = (v != 0u);   break;
            default: break;
            }
        } else if (!skip_field(buf, len, &pos, w)) {
            return false;
        }
    }
    return true;
}

// SerialConfig (proto:379) — 8 fields. T2.4.2.
//   1 enabled, 2 echo, 3 rxd, 4 txd, 5 baud (enum), 6 timeout,
//   7 mode (enum), 8 override_console_serial_port
bool phoneapi_decode_module_serial(const uint8_t *buf, uint16_t len,
                                   phoneapi_module_serial_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tw; if (!read_varint(buf, len, &pos, &tw)) return false;
        uint32_t f = (uint32_t)(tw >> 3);
        uint8_t  w = (uint8_t)(tw & 7u);
        if (w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            switch (f) {
            case 1u: out->enabled                       = (v != 0u);   break;
            case 2u: out->echo                          = (v != 0u);   break;
            case 3u: out->rxd                           = (uint32_t)v; break;
            case 4u: out->txd                           = (uint32_t)v; break;
            case 5u: out->baud                          = (uint8_t)v;  break;
            case 6u: out->timeout                       = (uint32_t)v; break;
            case 7u: out->mode                          = (uint8_t)v;  break;
            case 8u: out->override_console_serial_port  = (v != 0u);   break;
            default: break;
            }
        } else if (!skip_field(buf, len, &pos, w)) {
            return false;
        }
    }
    return true;
}

// ExternalNotificationConfig (proto:472) — 15 fields.  T2.4.3.
bool phoneapi_decode_module_ext_notif(const uint8_t *buf, uint16_t len,
                                      phoneapi_module_ext_notif_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tw; if (!read_varint(buf, len, &pos, &tw)) return false;
        uint32_t f = (uint32_t)(tw >> 3);
        uint8_t  w = (uint8_t)(tw & 7u);
        if (w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            switch (f) {
            case 1u:  out->enabled              = (v != 0u);   break;
            case 2u:  out->output_ms            = (uint32_t)v; break;
            case 3u:  out->output               = (uint32_t)v; break;
            case 4u:  out->active               = (v != 0u);   break;
            case 5u:  out->alert_message        = (v != 0u);   break;
            case 6u:  out->alert_bell           = (v != 0u);   break;
            case 7u:  out->use_pwm              = (v != 0u);   break;
            case 8u:  out->output_vibra         = (uint32_t)v; break;
            case 9u:  out->output_buzzer        = (uint32_t)v; break;
            case 10u: out->alert_message_vibra  = (v != 0u);   break;
            case 11u: out->alert_message_buzzer = (v != 0u);   break;
            case 12u: out->alert_bell_vibra     = (v != 0u);   break;
            case 13u: out->alert_bell_buzzer    = (v != 0u);   break;
            case 14u: out->nag_timeout          = (uint32_t)v; break;
            case 15u: out->use_i2s_as_buzzer    = (v != 0u);   break;
            default: break;
            }
        } else if (!skip_field(buf, len, &pos, w)) {
            return false;
        }
    }
    return true;
}

// RemoteHardwareConfig (proto:110) — 1 enabled, 2 allow_undefined_pin_access.
// Field 3 (available_pins) = repeated nested message; skipped in v1.
bool phoneapi_decode_module_remote_hw(const uint8_t *buf, uint16_t len,
                                      phoneapi_module_remote_hw_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tw; if (!read_varint(buf, len, &pos, &tw)) return false;
        uint32_t f = (uint32_t)(tw >> 3);
        uint8_t  w = (uint8_t)(tw & 7u);
        if (w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            if      (f == 1u) out->enabled                    = (v != 0u);
            else if (f == 2u) out->allow_undefined_pin_access = (v != 0u);
        } else if (!skip_field(buf, len, &pos, w)) {
            return false;
        }
    }
    return true;
}

bool phoneapi_walk_config_oneof(const uint8_t *buf, uint16_t len,
                                phoneapi_config_field_cb cb, void *ctx)
{
    if (buf == NULL || cb == NULL) return false;
    uint16_t pos = 0;
    while (pos < len) {
        uint64_t tag_word;
        if (!read_varint(buf, len, &pos, &tag_word)) return false;
        uint32_t field_num = (uint32_t)(tag_word >> 3);
        uint8_t  wt        = (uint8_t)(tag_word & 7u);

        if (wt == WT_LEN) {
            uint64_t sub_len;
            if (!read_varint(buf, len, &pos, &sub_len)) return false;
            if (sub_len > (uint64_t)(len - pos)) return false;
            cb(field_num, &buf[pos], (uint16_t)sub_len, ctx);
            pos += (uint16_t)sub_len;
        } else if (!skip_field(buf, len, &pos, wt)) {
            return false;
        }
    }
    return true;
}

const char *phoneapi_from_radio_tag_name(from_radio_tag_t tag)
{
    switch (tag) {
    case FR_TAG_NONE:                return "none";
    case FR_TAG_PACKET:              return "packet";
    case FR_TAG_MY_INFO:             return "my_info";
    case FR_TAG_NODE_INFO:           return "node_info";
    case FR_TAG_CONFIG:              return "config";
    case FR_TAG_LOG_RECORD:          return "log_record";
    case FR_TAG_CONFIG_COMPLETE_ID:  return "config_complete";
    case FR_TAG_REBOOTED:            return "rebooted";
    case FR_TAG_MODULE_CONFIG:       return "module_config";
    case FR_TAG_CHANNEL:             return "channel";
    case FR_TAG_QUEUE_STATUS:        return "queue_status";
    case FR_TAG_XMODEM:              return "xmodem";
    case FR_TAG_METADATA:            return "metadata";
    case FR_TAG_MQTT_CLIENT_PROXY:   return "mqtt_proxy";
    case FR_TAG_FILE_INFO:           return "file_info";
    case FR_TAG_CLIENT_NOTIFICATION: return "notify";
    case FR_TAG_DEVICEUI_CONFIG:     return "deviceui";
    default:                         return "?";
    }
}
