/* settings_keys.h — Metadata table for IPC_CFG_* keys (Stage 2).
 *
 * Per-key metadata: IPC code, display label, value kind, range / enum
 * strings, group, and whether mutating it requires a Core 0 reboot to
 * fully take effect (Meshtastic AdminModule per-segment rules — see
 * plan b2-mossy-brooks.md).
 *
 * The settings_view consults this table to render labels, range hints,
 * enum strings, and to decide whether Apply sends IPC_CMD_COMMIT_CONFIG
 * (soft) or IPC_CMD_COMMIT_REBOOT (hard).
 *
 * This is a Core-1-side mirror; Core 0's ipc_config_handler.cpp has its
 * own validation table. The two only share the `key` IDs from
 * ipc_protocol.h — they are independently maintained.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SK_KIND_U8,         ///< uint8_t in [min, max]
    SK_KIND_I8,         ///< int8_t in [min, max]
    SK_KIND_U32,        ///< uint32_t in [min, max]
    SK_KIND_BOOL,       ///< uint8_t 0/1
    SK_KIND_ENUM_U8,    ///< uint8_t mapped through enum_values[]
    SK_KIND_STR,        ///< UTF-8 string up to `max` bytes (Stage 3 — placeholder UI)
    SK_KIND_U32_FLAGS,  ///< uint32_t bitmask, displayed as 0x%08lX, ±step toggles bits
    SK_KIND_BYTES_RO,   ///< read-only opaque bytes, displayed as base64; edit blocked
} settings_kind_t;

typedef enum {
    SG_DEVICE = 0,
    SG_LORA,
    SG_POSITION,
    SG_POWER,
    SG_DISPLAY,
    SG_CHANNEL,
    SG_OWNER,
    SG_SECURITY,
    SG_GROUP_COUNT,
} settings_group_t;

typedef struct {
    uint16_t            ipc_key;        ///< IPC_CFG_* code from ipc_protocol.h
    uint8_t             group;          ///< settings_group_t
    uint8_t             kind;           ///< settings_kind_t
    int32_t             min;            ///< for numeric: lower bound; for STR: 0
    int32_t             max;            ///< for numeric: upper bound; for STR: max bytes
    uint8_t             needs_reboot;   ///< 1 = mutating this key forces COMMIT_REBOOT
    const char         *label;          ///< short human-readable label
    const char *const  *enum_values;    ///< for SK_KIND_ENUM_U8 only; indexed by raw u8
    uint8_t             enum_count;     ///< number of valid enum values (0 .. enum_count-1)
} settings_key_def_t;

const char *settings_group_name(settings_group_t g);

/** Returns pointer to the first key in `group` and writes its count. */
const settings_key_def_t *settings_keys_in_group(settings_group_t group,
                                                  uint8_t *out_count);

/** Look up a key by its IPC_CFG_* code; NULL if unknown. */
const settings_key_def_t *settings_key_find(uint16_t ipc_key);

/** Total number of keys in the table (sum across groups). */
uint8_t settings_keys_total_count(void);

#ifdef __cplusplus
}
#endif
