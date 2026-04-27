/* settings_keys.c — see settings_keys.h. */

#include "settings_keys.h"

#include <stddef.h>
#include <string.h>

#include "ipc_protocol.h"

/* ── Enum string tables ──────────────────────────────────────────────── *
 *
 * Only the names need to match Meshtastic enum *values*; the user sees
 * these strings, the IPC SET writes the index. If Meshtastic reorders
 * an enum we silently get wrong labels — which is why these mirror the
 * .proto enum order exactly (do not sort alphabetically).
 */

static const char *const k_lora_region[] = {
    "UNSET", "US", "EU_433", "EU_868", "CN", "JP", "ANZ", "KR",
    "TW", "RU", "IN", "NZ_865", "TH", "LORA_24", "UA_433", "UA_868",
    "MY_433", "MY_919", "SG_923", "PH_433", "PH_868", "PH_915",
    "ANZ_433", "KZ_433_870", "KZ_863_868", "NP_865", "BR_902",
};

static const char *const k_lora_modem[] = {
    "LONG_FAST", "LONG_SLOW", "VERY_LONG_SLOW", "MEDIUM_SLOW",
    "MEDIUM_FAST", "SHORT_SLOW", "SHORT_FAST", "LONG_MODERATE",
    "SHORT_TURBO",
};

static const char *const k_device_role[] = {
    "CLIENT", "CLIENT_MUTE", "ROUTER", "ROUTER_CLIENT",
    "REPEATER", "TRACKER", "SENSOR", "TAK", "CLIENT_HIDDEN",
    "LOST_AND_FOUND", "TAK_TRACKER", "ROUTER_LATE",
};

static const char *const k_gps_mode[] = {
    "DISABLED", "ENABLED", "NOT_PRESENT",
};

/* ── Master key table ────────────────────────────────────────────────── *
 *
 * Order matters: settings_keys_in_group() returns a contiguous slice
 * keyed by the `group` field, so all entries with the same group must
 * be consecutive. Keep groups in the same order as settings_group_t.
 */

static const settings_key_def_t k_keys[] = {
    /* ── Device ─────────────────────────────────────────────────────── */
    { IPC_CFG_DEVICE_ROLE, SG_DEVICE, SK_KIND_ENUM_U8,
      0, 11, /*reboot=*/1, "role",
      k_device_role, sizeof(k_device_role)/sizeof(k_device_role[0]) },

    /* ── LoRa ───────────────────────────────────────────────────────── */
    { IPC_CFG_LORA_REGION, SG_LORA, SK_KIND_ENUM_U8,
      0, 26, /*reboot=*/0, "region",
      k_lora_region, sizeof(k_lora_region)/sizeof(k_lora_region[0]) },
    { IPC_CFG_LORA_MODEM_PRESET, SG_LORA, SK_KIND_ENUM_U8,
      0, 8, /*reboot=*/0, "preset",
      k_lora_modem, sizeof(k_lora_modem)/sizeof(k_lora_modem[0]) },
    { IPC_CFG_LORA_TX_POWER, SG_LORA, SK_KIND_I8,
      0, 30, /*reboot=*/0, "tx_pwr_dbm",
      NULL, 0 },
    { IPC_CFG_LORA_HOP_LIMIT, SG_LORA, SK_KIND_U8,
      1, 7, /*reboot=*/0, "hop_limit",
      NULL, 0 },
    { IPC_CFG_LORA_CHANNEL_NUM, SG_LORA, SK_KIND_U8,
      0, 250, /*reboot=*/0, "channel",
      NULL, 0 },

    /* ── Position ───────────────────────────────────────────────────── */
    { IPC_CFG_GPS_MODE, SG_POSITION, SK_KIND_ENUM_U8,
      0, 2, /*reboot=*/1, "gps_mode",
      k_gps_mode, sizeof(k_gps_mode)/sizeof(k_gps_mode[0]) },
    { IPC_CFG_GPS_UPDATE_INTERVAL, SG_POSITION, SK_KIND_U32,
      0, 86400, /*reboot=*/1, "gps_int_s",
      NULL, 0 },
    { IPC_CFG_POSITION_BCAST_SECS, SG_POSITION, SK_KIND_U32,
      0, 86400, /*reboot=*/1, "pos_bcast_s",
      NULL, 0 },

    /* ── Power ──────────────────────────────────────────────────────── */
    { IPC_CFG_POWER_SAVING, SG_POWER, SK_KIND_BOOL,
      0, 1, /*reboot=*/1, "save_pwr",
      NULL, 0 },
    { IPC_CFG_SHUTDOWN_AFTER_SECS, SG_POWER, SK_KIND_U32,
      0, 86400, /*reboot=*/1, "shut_after_s",
      NULL, 0 },

    /* ── Display ────────────────────────────────────────────────────── */
    { IPC_CFG_SCREEN_ON_SECS, SG_DISPLAY, SK_KIND_U32,
      0, 86400, /*reboot=*/0, "screen_on_s",
      NULL, 0 },
    { IPC_CFG_UNITS_METRIC, SG_DISPLAY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "metric",
      NULL, 0 },

    /* ── Owner ──────────────────────────────────────────────────────── */
    { IPC_CFG_OWNER_LONG_NAME, SG_OWNER, SK_KIND_STR,
      0, 39, /*reboot=*/0, "long_name",
      NULL, 0 },
    { IPC_CFG_OWNER_SHORT_NAME, SG_OWNER, SK_KIND_STR,
      0, 4, /*reboot=*/0, "short_name",
      NULL, 0 },
};

#define KEY_COUNT  ((uint8_t)(sizeof(k_keys) / sizeof(k_keys[0])))

static const char *const k_group_names[SG_GROUP_COUNT] = {
    [SG_DEVICE]   = "Device",
    [SG_LORA]     = "LoRa",
    [SG_POSITION] = "Position",
    [SG_POWER]    = "Power",
    [SG_DISPLAY]  = "Display",
    [SG_OWNER]    = "Owner",
};

const char *settings_group_name(settings_group_t g)
{
    if ((unsigned)g >= SG_GROUP_COUNT) return "?";
    return k_group_names[g];
}

const settings_key_def_t *settings_keys_in_group(settings_group_t group,
                                                  uint8_t *out_count)
{
    uint8_t first = KEY_COUNT;
    uint8_t count = 0;
    for (uint8_t i = 0; i < KEY_COUNT; ++i) {
        if (k_keys[i].group == (uint8_t)group) {
            if (first == KEY_COUNT) first = i;
            count++;
        }
    }
    if (out_count) *out_count = count;
    if (count == 0) return NULL;
    return &k_keys[first];
}

const settings_key_def_t *settings_key_find(uint16_t ipc_key)
{
    for (uint8_t i = 0; i < KEY_COUNT; ++i) {
        if (k_keys[i].ipc_key == ipc_key) return &k_keys[i];
    }
    return NULL;
}

uint8_t settings_keys_total_count(void)
{
    return KEY_COUNT;
}
