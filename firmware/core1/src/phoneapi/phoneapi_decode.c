// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors

#include "phoneapi_decode.h"

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
} chan_settings_ctx_t;

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
                .name       = out->name,
                .name_max   = PHONEAPI_CHANNEL_NAME_MAX,
                .psk_len    = &out->psk_len,
                .channel_id = &out->channel_id,
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
        } else if (f == 7u && w == WT_VARINT) {
            uint64_t v; if (!read_varint(buf, len, &pos, &v)) return false;
            out->role = (uint8_t)v;
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
