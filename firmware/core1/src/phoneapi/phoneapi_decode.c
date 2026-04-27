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
