/* channel_share_url.c — see channel_share_url.h.
 *
 * Single-pass encoder: builds the ChannelSet protobuf into a stack
 * buffer with single-byte length backpatches (max body ≈ 110 B,
 * comfortably under 128 B), then base64-url-encodes into the output
 * after the prefix.
 *
 * Field numbers from generated proto headers:
 *   apponly.pb.h:    meshtastic_ChannelSet_settings_tag    = 1
 *                    meshtastic_ChannelSet_lora_config_tag = 2
 *   channel.pb.h:    meshtastic_ChannelSettings_psk_tag             = 2
 *                    meshtastic_ChannelSettings_name_tag            = 3
 *                    meshtastic_ChannelSettings_module_settings_tag = 7
 *                    meshtastic_ModuleSettings_position_precision_tag = 1
 *                    meshtastic_ModuleSettings_is_muted_tag         = 2
 *   config.pb.h:     meshtastic_Config_LoRaConfig_use_preset_tag     = 1
 *                    meshtastic_Config_LoRaConfig_modem_preset_tag   = 2
 *                    meshtastic_Config_LoRaConfig_region_tag         = 7
 *                    meshtastic_Config_LoRaConfig_hop_limit_tag      = 8
 *                    meshtastic_Config_LoRaConfig_tx_power_tag       = 10
 *
 * Lesson from B-3 (commit cebef0d): never trust plan-time field-number
 * comments; cross-check against generated headers. The above were
 * verified before writing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "channel_share_url.h"
#include "base64_url.h"

#include <string.h>

#include "phoneapi_cache.h"

/* ── Mini protobuf encoder (subset of phoneapi_encode helpers, kept
 *    local so this file doesn't depend on the cascade encode path). ── */

static bool put_varint(uint8_t *dst, size_t cap, size_t *pos, uint64_t v)
{
    while (v >= 0x80u) {
        if (*pos + 1u > cap) return false;
        dst[(*pos)++] = (uint8_t)(v | 0x80u);
        v >>= 7;
    }
    if (*pos + 1u > cap) return false;
    dst[(*pos)++] = (uint8_t)v;
    return true;
}

static bool put_tag(uint8_t *dst, size_t cap, size_t *pos,
                    uint32_t field, uint8_t wt)
{
    return put_varint(dst, cap, pos, ((uint64_t)field << 3) | wt);
}

static bool put_bytes_field(uint8_t *dst, size_t cap, size_t *pos,
                            uint32_t field, const uint8_t *data, size_t len)
{
    if (!put_tag(dst, cap, pos, field, 2u)) return false;
    if (!put_varint(dst, cap, pos, len)) return false;
    if (*pos + len > cap) return false;
    if (len > 0u && data != NULL) {
        memcpy(dst + *pos, data, len);
    }
    *pos += len;
    return true;
}

static bool put_varint_field(uint8_t *dst, size_t cap, size_t *pos,
                             uint32_t field, uint64_t value)
{
    if (!put_tag(dst, cap, pos, field, 0u)) return false;
    if (!put_varint(dst, cap, pos, value)) return false;
    return true;
}

/* ── Sub-message builders ───────────────────────────────────────────── */

static size_t encode_module_settings(uint8_t *dst, size_t cap,
                                     const phoneapi_channel_t *ch)
{
    /* ModuleSettings {
     *   uint32 position_precision = 1;
     *   bool   is_muted           = 2;
     * }
     * Both proto3 — emit only when set / non-default to keep the
     * encoded size matching what the device actually carries on its
     * configured channel. */
    size_t pos = 0;
    if (ch->module_position_precision != 0u) {
        if (!put_varint_field(dst, cap, &pos, 1u,
                              ch->module_position_precision)) return SIZE_MAX;
    }
    if (ch->module_is_muted) {
        if (!put_varint_field(dst, cap, &pos, 2u, 1u)) return SIZE_MAX;
    }
    return pos;
}

static size_t encode_channel_settings(uint8_t *dst, size_t cap,
                                       const phoneapi_channel_t *ch)
{
    /* ChannelSettings {
     *   bytes  psk             = 2;
     *   string name            = 3;
     *   ModuleSettings module_settings = 7;
     * }
     * Field 1 (channel_num) and 4 (id) are deprecated/unused in
     * B-4 v1; field 5/6 (uplink/downlink) default false — skipped. */
    size_t pos = 0;

    /* psk — always emit if non-empty. */
    if (ch->psk_len > 0u) {
        if (!put_bytes_field(dst, cap, &pos, 2u,
                             ch->psk, ch->psk_len)) return SIZE_MAX;
    }

    /* name — emit if non-empty. */
    size_t name_len = 0u;
    while (name_len < PHONEAPI_CHANNEL_NAME_MAX &&
           ch->name[name_len] != '\0') name_len++;
    if (name_len > 0u) {
        if (!put_bytes_field(dst, cap, &pos, 3u,
                             (const uint8_t *)ch->name, name_len)) return SIZE_MAX;
    }

    /* module_settings — sub-message. Reserve 1 byte for length. */
    if (ch->has_module_settings) {
        if (!put_tag(dst, cap, &pos, 7u, 2u)) return SIZE_MAX;
        if (pos + 1u > cap) return SIZE_MAX;
        size_t len_pos = pos++;
        size_t ms_len = encode_module_settings(dst + pos, cap - pos, ch);
        if (ms_len == SIZE_MAX) return SIZE_MAX;
        if (ms_len >= 128u) return SIZE_MAX;
        dst[len_pos] = (uint8_t)ms_len;
        pos += ms_len;
    }
    return pos;
}

static size_t encode_lora_config(uint8_t *dst, size_t cap,
                                  const phoneapi_config_lora_t *lc)
{
    /* LoRaConfig — emit the subset captured by phoneapi_decode_config_lora:
     *   field 1 use_preset (bool)
     *   field 2 modem_preset (varint enum)
     *   field 7 region (varint enum)
     *   field 8 hop_limit (varint)
     *   field 10 tx_power (sint32 → varint with sign-extension)
     *
     * When use_preset=true, receiver derives bandwidth/spread_factor/
     * coding_rate from modem_preset, so omitting those fields still
     * yields a usable URL. v2 may extend phoneapi_config_lora_t +
     * decoder to capture all 13 LoRaConfig fields for byte-exact
     * match with `meshtastic --info` Primary URL output. */
    size_t pos = 0;
    if (!put_varint_field(dst, cap, &pos, 1u, lc->use_preset ? 1u : 0u))
        return SIZE_MAX;
    if (!put_varint_field(dst, cap, &pos, 2u, lc->modem_preset))
        return SIZE_MAX;
    if (lc->region != 0u) {
        if (!put_varint_field(dst, cap, &pos, 7u, lc->region))
            return SIZE_MAX;
    }
    if (lc->hop_limit != 0u) {
        if (!put_varint_field(dst, cap, &pos, 8u, lc->hop_limit))
            return SIZE_MAX;
    }
    /* tx_power is int32 — proto3 encodes negative as 10-byte varint
     * (sign-extended). For the typical 0..30 dBm range a single byte
     * suffices; the cast to uint64_t handles negatives correctly via
     * sign-extension. */
    if (lc->tx_power != 0) {
        if (!put_varint_field(dst, cap, &pos, 10u,
                              (uint64_t)(int64_t)lc->tx_power))
            return SIZE_MAX;
    }
    return pos;
}

/* ── Top-level builder ──────────────────────────────────────────────── */

static const char URL_PREFIX[] = "https://meshtastic.org/e/#";
#define URL_PREFIX_LEN  ((sizeof(URL_PREFIX)) - 1u)

size_t channel_share_url_build(uint8_t channel_index,
                                char *out, size_t cap)
{
    if (out == NULL || cap < URL_PREFIX_LEN + 1u) return 0u;

    /* Pull cache snapshots — fail if either is missing. */
    phoneapi_channel_t ch;
    if (!phoneapi_cache_get_channel(channel_index, &ch) || !ch.in_use) {
        return 0u;
    }
    phoneapi_config_lora_t lc;
    if (!phoneapi_cache_get_config_lora(&lc)) {
        return 0u;
    }

    /* Build ChannelSet bytes into a scratch buffer. Worst case ~110 B
     * (32 B PSK + 11 B name + tags/lengths + LoRaConfig). 160 B gives
     * margin without bloating the stack. */
    uint8_t buf[160];
    size_t  pos = 0;

    /* ChannelSet.settings (field 1, repeated → emit once) */
    if (!put_tag(buf, sizeof(buf), &pos, 1u, 2u)) return 0u;
    if (pos + 1u > sizeof(buf)) return 0u;
    size_t cs_len_pos = pos++;
    size_t cs_len = encode_channel_settings(buf + pos, sizeof(buf) - pos, &ch);
    if (cs_len == SIZE_MAX || cs_len >= 128u) return 0u;
    buf[cs_len_pos] = (uint8_t)cs_len;
    pos += cs_len;

    /* ChannelSet.lora_config (field 2) */
    if (!put_tag(buf, sizeof(buf), &pos, 2u, 2u)) return 0u;
    if (pos + 1u > sizeof(buf)) return 0u;
    size_t lc_len_pos = pos++;
    size_t lc_len = encode_lora_config(buf + pos, sizeof(buf) - pos, &lc);
    if (lc_len == SIZE_MAX || lc_len >= 128u) return 0u;
    buf[lc_len_pos] = (uint8_t)lc_len;
    pos += lc_len;

    /* Compose URL = prefix + base64_urlsafe(buf[0..pos]) */
    memcpy(out, URL_PREFIX, URL_PREFIX_LEN);
    size_t b64 = base64_urlsafe_encode(buf, pos,
                                        out + URL_PREFIX_LEN,
                                        cap - URL_PREFIX_LEN);
    if (b64 == 0u) return 0u;   /* base64 encoder rejected (cap too small) */
    return URL_PREFIX_LEN + b64;
}
