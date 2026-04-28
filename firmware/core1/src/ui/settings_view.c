/* settings_view.c — see settings_view.h. */

#include "settings_view.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ipc_protocol.h"
#include "mie_font.h"
#include "mie/keycode.h"
#include "settings_client.h"
#include "settings_keys.h"
#include "ime_task.h"
#include "mie/utf8.h"
#include "phoneapi_cache.h"

/* Layout (landscape 320×240):
 *   y   0 .. 23   header  — "[Group X/Y] group_name [pending: N]"
 *   y  24 .. 215  body    — key list (cursor "> " prefix on selected row)
 *   y 216 .. 239  footer  — hint / status
 */

static lv_obj_t *s_panel;
static lv_obj_t *s_header;
static lv_obj_t *s_body;
static lv_obj_t *s_footer;

/* ── Per-key cached state ────────────────────────────────────────────── *
 *
 * Index space is the row-order in settings_keys.c (settings_key_find
 * doesn't expose an index, so we track our own parallel array via
 * pointer compare).
 *
 * value/value_len = last value seen from a CONFIG_VALUE reply or our
 * own SET (so the UI shows the new value immediately even before
 * Apply round-trips).
 *
 * dirty = caller pressed OK in edit mode; pending until next Apply.
 * The Apply success clears dirty for all keys in the group.
 */
#define KEY_CACHE_MAX  96u  /* must be ≥ settings_keys_total_count() */
/* Per-key cached value buffer. Owner long_name (39 B) is stored
 * truncated; full bytes flow through ime_request_text without
 * touching this cache. 12 B keeps total BSS bounded while still
 * showing the leading 3-4 CJK characters or 12 ASCII chars. */
#define VAL_BUF_MAX    12u

typedef struct {
    bool      have_value;
    bool      dirty;
    uint8_t   value_len;
    uint8_t   value[VAL_BUF_MAX];
} key_cache_t;

static key_cache_t s_cache[KEY_CACHE_MAX];

/* ── UI state ────────────────────────────────────────────────────────── */

typedef enum {
    UI_BROWSE = 0,
    UI_EDIT,
} ui_mode_t;

static ui_mode_t s_mode;
static uint8_t   s_cur_group;     /* settings_group_t */
static uint8_t   s_cur_row;       /* index in current group's row list (0 .. n_keys = Apply) */
static uint8_t   s_cur_channel_index;  /* 0..PHONEAPI_CHANNEL_COUNT-1, only meaningful in SG_CHANNEL */
static bool      s_dirty_load;    /* trigger GET burst on next refresh after activation */
static uint32_t  s_last_render_seq;
static uint32_t  s_render_seq;
static uint32_t  s_last_phoneapi_seq;  /* phoneapi_cache_change_seq() last seen */

/* Edit overlay working copy (raw u8/u32 representation in little-endian
 * bytes — same wire format we send via SET). */
static uint8_t  s_edit_buf[VAL_BUF_MAX];
static uint16_t s_edit_len;
static uint16_t s_edit_key;       /* IPC_CFG_* of the key being edited */

static char s_footer_msg[40];     /* sticky footer override (msg ≤ ~36 chars) */

/* Forward decl for the ime_request_text callback (defined near
 * confirm_edit; referenced earlier from enter_edit_for_row). */
static void str_edit_done(bool committed, const char *utf8,
                          uint16_t byte_len, void *ctx);

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int find_key_index(uint16_t ipc_key)
{
    uint8_t idx = 0;
    for (uint8_t g = 0; g < SG_GROUP_COUNT; ++g) {
        uint8_t cnt = 0;
        const settings_key_def_t *defs = settings_keys_in_group(g, &cnt);
        for (uint8_t i = 0; i < cnt; ++i) {
            if (defs[i].ipc_key == ipc_key) return (int)(idx + i);
        }
        idx += cnt;
    }
    return -1;
}

static int row_to_global_index(uint8_t group, uint8_t row, uint8_t group_n_keys)
{
    if (row >= group_n_keys) return -1;  /* "Apply" row */
    uint8_t idx = 0;
    for (uint8_t pg = 0; pg < group; ++pg) {
        uint8_t pc = 0;
        (void)settings_keys_in_group(pg, &pc);
        idx += pc;
    }
    return (int)(idx + row);
}

static uint32_t value_to_u32(const uint8_t *buf, uint8_t len)
{
    uint32_t v = 0;
    if (len > 4) len = 4;
    for (uint8_t i = 0; i < len; ++i) v |= ((uint32_t)buf[i]) << (8u * i);
    return v;
}

static void value_from_u32(uint8_t *buf, uint8_t len, uint32_t v)
{
    if (len > 4) len = 4;
    for (uint8_t i = 0; i < len; ++i) buf[i] = (uint8_t)(v >> (8u * i));
}

static int32_t value_to_i32(const uint8_t *buf, uint8_t len)
{
    /* Sign-extend from the top byte of the field. */
    uint32_t u = value_to_u32(buf, len);
    if (len == 1 && (u & 0x80u)) u |= 0xFFFFFF00u;
    else if (len == 2 && (u & 0x8000u)) u |= 0xFFFF0000u;
    return (int32_t)u;
}

static void format_value(const settings_key_def_t *def,
                         const uint8_t *buf, uint8_t len,
                         char *out, size_t out_sz)
{
    if (len == 0) { snprintf(out, out_sz, "(?)"); return; }
    switch (def->kind) {
    case SK_KIND_BOOL:
        snprintf(out, out_sz, "%s", buf[0] ? "true" : "false");
        break;
    case SK_KIND_ENUM_U8: {
        uint8_t v = buf[0];
        if (def->enum_values && v < def->enum_count && def->enum_values[v]) {
            snprintf(out, out_sz, "%s", def->enum_values[v]);
        } else {
            snprintf(out, out_sz, "%u", (unsigned)v);
        }
        break;
    }
    case SK_KIND_U8:
        snprintf(out, out_sz, "%u", (unsigned)buf[0]);
        break;
    case SK_KIND_I8:
        snprintf(out, out_sz, "%d", (int)(int8_t)buf[0]);
        break;
    case SK_KIND_U32:
        snprintf(out, out_sz, "%lu", (unsigned long)value_to_u32(buf, len));
        break;
    case SK_KIND_U32_FLAGS:
        snprintf(out, out_sz, "0x%08lX", (unsigned long)value_to_u32(buf, len));
        break;
    case SK_KIND_BYTES_RO: {
        /* First 6 bytes as hex, ellipsis for the rest. The full key is
         * not on screen anywhere — UI is preview-only. */
        size_t shown = (len > 6u) ? 6u : len;
        size_t pos = 0;
        for (size_t i = 0; i < shown && pos + 3 < out_sz; ++i) {
            pos += (size_t)snprintf(&out[pos], out_sz - pos, "%02X", buf[i]);
        }
        if (len > shown && pos + 3 < out_sz) {
            snprintf(&out[pos], out_sz - pos, "...");
        }
        if (len == 0u) snprintf(out, out_sz, "(empty)");
        break;
    }
    case SK_KIND_STR: {
        size_t n = (len < out_sz - 1) ? len : (out_sz - 1);
        memcpy(out, buf, n);
        out[n] = '\0';
        break;
    }
    default:
        snprintf(out, out_sz, "?");
        break;
    }
}

static uint8_t pending_count_in_group(uint8_t group)
{
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(group, &n_keys);
    if (!defs) return 0;
    uint8_t pending = 0;
    int base = row_to_global_index(group, 0, n_keys + 1);
    if (base < 0) return 0;
    for (uint8_t i = 0; i < n_keys; ++i) {
        if (s_cache[base + i].dirty) pending++;
    }
    return pending;
}

static bool group_needs_reboot(uint8_t group)
{
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(group, &n_keys);
    if (!defs) return false;
    int base = row_to_global_index(group, 0, n_keys + 1);
    if (base < 0) return false;
    for (uint8_t i = 0; i < n_keys; ++i) {
        if (s_cache[base + i].dirty && defs[i].needs_reboot) return true;
    }
    return false;
}

static void send_get_burst(uint8_t group)
{
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(group, &n_keys);
    if (!defs) return;
    uint8_t ch = (group == SG_CHANNEL) ? s_cur_channel_index : 0u;
    for (uint8_t i = 0; i < n_keys; ++i) {
        (void)settings_client_send_get(defs[i].ipc_key, ch);
    }
}

/* ── M5F.3: cache-driven row population ──────────────────────────────── *
 *
 * Cascade FR_TAG_CONFIG handler (phoneapi_session.c) decodes the four
 * Config sub-oneofs (Device / LoRa / Position / Display) into
 * phoneapi_cache. settings_view pulls from the cache so opening a
 * group renders immediately, instead of waiting for the GET-burst
 * round-trips. Keys not in the cached groups (Power / Channel /
 * Owner) still go through send_get_burst (B3-P2 / B3-P3 will move
 * those into the cache too).
 *
 * Dirty rows (user-edited but not yet Apply'd) are skipped so a
 * cascade refresh doesn't clobber pending edits. */

static void cache_set_row_u8(int gidx, uint8_t v)
{
    if (gidx < 0 || gidx >= (int)KEY_CACHE_MAX) return;
    if (s_cache[gidx].dirty) return;
    s_cache[gidx].value[0]   = v;
    s_cache[gidx].value_len  = 1;
    s_cache[gidx].have_value = true;
}

static void cache_set_row_u32(int gidx, uint32_t v)
{
    if (gidx < 0 || gidx >= (int)KEY_CACHE_MAX) return;
    if (s_cache[gidx].dirty) return;
    value_from_u32(s_cache[gidx].value, 4, v);
    s_cache[gidx].value_len  = 4;
    s_cache[gidx].have_value = true;
}

static void cache_set_row_str(int gidx, const char *s, size_t maxlen)
{
    if (gidx < 0 || gidx >= (int)KEY_CACHE_MAX) return;
    if (s_cache[gidx].dirty) return;
    size_t n = strnlen(s, maxlen);
    if (n > VAL_BUF_MAX) n = VAL_BUF_MAX;
    memcpy(s_cache[gidx].value, s, n);
    s_cache[gidx].value_len  = (uint8_t)n;
    s_cache[gidx].have_value = true;
}

static void cache_set_row_bytes(int gidx, const uint8_t *src, uint8_t src_len)
{
    if (gidx < 0 || gidx >= (int)KEY_CACHE_MAX) return;
    if (s_cache[gidx].dirty) return;
    /* VAL_BUF_MAX (12) is smaller than typical bytes fields (32B
     * pubkey). Truncate to first 12 bytes — render layer shows them
     * as hex prefix; full key is not needed for the UI. */
    uint8_t n = (src_len < VAL_BUF_MAX) ? src_len : VAL_BUF_MAX;
    memcpy(s_cache[gidx].value, src, n);
    s_cache[gidx].value_len  = n;
    s_cache[gidx].have_value = true;
}

static bool populate_group_from_cache(uint8_t group)
{
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(group, &n_keys);
    if (defs == NULL) return false;
    int base = row_to_global_index(group, 0, n_keys + 1);
    if (base < 0) return false;

    switch (group) {
    case SG_DEVICE: {
        phoneapi_config_device_t d;
        if (!phoneapi_cache_get_config_device(&d)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_DEVICE_ROLE:
                cache_set_row_u8(idx, d.role); break;
            case IPC_CFG_DEVICE_REBROADCAST_MODE:
                cache_set_row_u8(idx, d.rebroadcast_mode); break;
            case IPC_CFG_DEVICE_NODE_INFO_BCAST_SECS:
                cache_set_row_u32(idx, d.node_info_broadcast_secs); break;
            case IPC_CFG_DEVICE_DOUBLE_TAP_BTN:
                cache_set_row_u8(idx, d.double_tap_as_button_press ? 1u : 0u); break;
            case IPC_CFG_DEVICE_DISABLE_TRIPLE_CLICK:
                cache_set_row_u8(idx, d.disable_triple_click ? 1u : 0u); break;
            case IPC_CFG_DEVICE_TZDEF:
                cache_set_row_str(idx, d.tzdef, PHONEAPI_TZDEF_MAX); break;
            case IPC_CFG_DEVICE_LED_HEARTBEAT_DISABLED:
                cache_set_row_u8(idx, d.led_heartbeat_disabled ? 1u : 0u); break;
            default: break;
            }
        }
        return true;
    }
    case SG_LORA: {
        phoneapi_config_lora_t d;
        if (!phoneapi_cache_get_config_lora(&d)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_LORA_REGION:
                cache_set_row_u8(idx, d.region); break;
            case IPC_CFG_LORA_MODEM_PRESET:
                cache_set_row_u8(idx, d.modem_preset); break;
            case IPC_CFG_LORA_TX_POWER:
                cache_set_row_u8(idx, (uint8_t)(int8_t)d.tx_power); break;
            case IPC_CFG_LORA_HOP_LIMIT:
                cache_set_row_u8(idx, (uint8_t)d.hop_limit); break;
            case IPC_CFG_LORA_CHANNEL_NUM:
                cache_set_row_u8(idx, (uint8_t)d.channel_num); break;
            case IPC_CFG_LORA_USE_PRESET:
                cache_set_row_u8(idx, d.use_preset ? 1u : 0u); break;
            case IPC_CFG_LORA_BANDWIDTH:
                cache_set_row_u32(idx, d.bandwidth); break;
            case IPC_CFG_LORA_SPREAD_FACTOR:
                cache_set_row_u32(idx, d.spread_factor); break;
            case IPC_CFG_LORA_CODING_RATE:
                cache_set_row_u32(idx, d.coding_rate); break;
            case IPC_CFG_LORA_TX_ENABLED:
                cache_set_row_u8(idx, d.tx_enabled ? 1u : 0u); break;
            case IPC_CFG_LORA_OVERRIDE_DUTY_CYCLE:
                cache_set_row_u8(idx, d.override_duty_cycle ? 1u : 0u); break;
            case IPC_CFG_LORA_SX126X_RX_BOOSTED_GAIN:
                cache_set_row_u8(idx, d.sx126x_rx_boosted_gain ? 1u : 0u); break;
            case IPC_CFG_LORA_FEM_LNA_MODE:
                cache_set_row_u8(idx, d.fem_lna_mode); break;
            default: break;
            }
        }
        return true;
    }
    case SG_POSITION: {
        phoneapi_config_position_t d;
        if (!phoneapi_cache_get_config_position(&d)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_GPS_MODE:
                cache_set_row_u8(idx, d.gps_mode); break;
            case IPC_CFG_GPS_UPDATE_INTERVAL:
                cache_set_row_u32(idx, d.gps_update_interval); break;
            case IPC_CFG_POSITION_BCAST_SECS:
                cache_set_row_u32(idx, d.position_broadcast_secs); break;
            case IPC_CFG_POSITION_BCAST_SMART_ENABLED:
                cache_set_row_u8(idx, d.position_broadcast_smart_enabled ? 1u : 0u); break;
            case IPC_CFG_POSITION_FIXED_POSITION:
                cache_set_row_u8(idx, d.fixed_position ? 1u : 0u); break;
            case IPC_CFG_POSITION_FLAGS:
                cache_set_row_u32(idx, d.position_flags); break;
            case IPC_CFG_POSITION_BCAST_SMART_MIN_DIST:
                cache_set_row_u32(idx, d.broadcast_smart_minimum_distance); break;
            case IPC_CFG_POSITION_BCAST_SMART_MIN_INT_SECS:
                cache_set_row_u32(idx, d.broadcast_smart_minimum_interval_secs); break;
            default: break;
            }
        }
        return true;
    }
    case SG_POWER: {
        phoneapi_config_power_t d;
        if (!phoneapi_cache_get_config_power(&d)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_POWER_SAVING:
                cache_set_row_u8(idx, d.is_power_saving ? 1u : 0u); break;
            case IPC_CFG_SHUTDOWN_AFTER_SECS:
                cache_set_row_u32(idx, d.on_battery_shutdown_after_secs); break;
            case IPC_CFG_POWER_SDS_SECS:
                cache_set_row_u32(idx, d.sds_secs); break;
            case IPC_CFG_POWER_LS_SECS:
                cache_set_row_u32(idx, d.ls_secs); break;
            case IPC_CFG_POWER_MIN_WAKE_SECS:
                cache_set_row_u32(idx, d.min_wake_secs); break;
            case IPC_CFG_POWER_BATTERY_INA_ADDRESS:
                cache_set_row_u32(idx, d.device_battery_ina_address); break;
            case IPC_CFG_POWER_POWERMON_ENABLES:
                cache_set_row_u32(idx, d.powermon_enables_lo); break;
            default: break;
            }
        }
        return true;
    }
    case SG_SECURITY: {
        phoneapi_config_security_t d;
        if (!phoneapi_cache_get_config_security(&d)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_SECURITY_PUBLIC_KEY:
                cache_set_row_bytes(idx, d.public_key, d.public_key_len); break;
            case IPC_CFG_SECURITY_IS_MANAGED:
                cache_set_row_u8(idx, d.is_managed ? 1u : 0u); break;
            case IPC_CFG_SECURITY_SERIAL_ENABLED:
                cache_set_row_u8(idx, d.serial_enabled ? 1u : 0u); break;
            case IPC_CFG_SECURITY_DEBUG_LOG_API_ENABLED:
                cache_set_row_u8(idx, d.debug_log_api_enabled ? 1u : 0u); break;
            case IPC_CFG_SECURITY_ADMIN_CHANNEL_ENABLED:
                cache_set_row_u8(idx, d.admin_channel_enabled ? 1u : 0u); break;
            default: break;
            }
        }
        return true;
    }
    case SG_OWNER: {
        /* Owner = User record for our own node_num. Pull from
         * phoneapi_cache_get_node_by_id(my_node_num). */
        phoneapi_my_info_t mi;
        if (!phoneapi_cache_get_my_info(&mi)) return false;
        phoneapi_node_t self;
        if (!phoneapi_cache_get_node_by_id(mi.my_node_num, &self)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_OWNER_LONG_NAME:
                cache_set_row_str(idx, self.long_name, PHONEAPI_LONG_NAME_MAX); break;
            case IPC_CFG_OWNER_SHORT_NAME:
                cache_set_row_str(idx, self.short_name, PHONEAPI_SHORT_NAME_MAX); break;
            case IPC_CFG_OWNER_IS_LICENSED:
                cache_set_row_u8(idx, self.is_licensed ? 1u : 0u); break;
            case IPC_CFG_OWNER_PUBLIC_KEY:
                cache_set_row_bytes(idx, self.public_key, self.public_key_len); break;
            default: break;
            }
        }
        return true;
    }
    case SG_CHANNEL: {
        /* B3-P3: addressed by s_cur_channel_index (0..7). Cascade decoder
         * fills all 8 channel cache slots from FromRadio.channel frames. */
        phoneapi_channel_t ch;
        if (!phoneapi_cache_get_channel(s_cur_channel_index, &ch)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_CHANNEL_NAME:
                cache_set_row_str(idx, ch.name, PHONEAPI_CHANNEL_NAME_MAX); break;
            case IPC_CFG_CHANNEL_MODULE_POSITION_PRECISION:
                cache_set_row_u32(idx, ch.module_position_precision); break;
            case IPC_CFG_CHANNEL_MODULE_IS_MUTED:
                cache_set_row_u8(idx, ch.module_is_muted ? 1u : 0u); break;
            default: break;
            }
        }
        return true;
    }
    case SG_TELEMETRY: {
        phoneapi_module_telemetry_t m;
        if (!phoneapi_cache_get_module_telemetry(&m)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_TELEM_DEVICE_UPDATE_INTERVAL:
                cache_set_row_u32(idx, m.device_update_interval); break;
            case IPC_CFG_TELEM_ENV_UPDATE_INTERVAL:
                cache_set_row_u32(idx, m.environment_update_interval); break;
            case IPC_CFG_TELEM_ENV_MEASUREMENT_ENABLED:
                cache_set_row_u8(idx, m.environment_measurement_enabled ? 1u : 0u); break;
            case IPC_CFG_TELEM_ENV_SCREEN_ENABLED:
                cache_set_row_u8(idx, m.environment_screen_enabled ? 1u : 0u); break;
            case IPC_CFG_TELEM_ENV_DISPLAY_FAHRENHEIT:
                cache_set_row_u8(idx, m.environment_display_fahrenheit ? 1u : 0u); break;
            case IPC_CFG_TELEM_POWER_MEASUREMENT_ENABLED:
                cache_set_row_u8(idx, m.power_measurement_enabled ? 1u : 0u); break;
            case IPC_CFG_TELEM_POWER_UPDATE_INTERVAL:
                cache_set_row_u32(idx, m.power_update_interval); break;
            case IPC_CFG_TELEM_POWER_SCREEN_ENABLED:
                cache_set_row_u8(idx, m.power_screen_enabled ? 1u : 0u); break;
            case IPC_CFG_TELEM_DEVICE_TELEM_ENABLED:
                cache_set_row_u8(idx, m.device_telemetry_enabled ? 1u : 0u); break;
            default: break;
            }
        }
        return true;
    }
    case SG_NEIGHBOR: {
        phoneapi_module_neighbor_t m;
        if (!phoneapi_cache_get_module_neighbor(&m)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_NEIGHBOR_ENABLED:
                cache_set_row_u8(idx, m.enabled ? 1u : 0u); break;
            case IPC_CFG_NEIGHBOR_UPDATE_INTERVAL:
                cache_set_row_u32(idx, m.update_interval); break;
            case IPC_CFG_NEIGHBOR_TRANSMIT_OVER_LORA:
                cache_set_row_u8(idx, m.transmit_over_lora ? 1u : 0u); break;
            default: break;
            }
        }
        return true;
    }
    case SG_RANGE_TEST: {
        phoneapi_module_range_test_t m;
        if (!phoneapi_cache_get_module_range_test(&m)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_RANGETEST_ENABLED:
                cache_set_row_u8(idx, m.enabled ? 1u : 0u); break;
            case IPC_CFG_RANGETEST_SENDER:
                cache_set_row_u32(idx, m.sender); break;
            default: break;
            }
        }
        return true;
    }
    case SG_DETECT_SENSOR: {
        phoneapi_module_detect_t m;
        if (!phoneapi_cache_get_module_detect(&m)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_DETECT_ENABLED:
                cache_set_row_u8(idx, m.enabled ? 1u : 0u); break;
            case IPC_CFG_DETECT_MIN_BCAST_SECS:
                cache_set_row_u32(idx, m.minimum_broadcast_secs); break;
            case IPC_CFG_DETECT_STATE_BCAST_SECS:
                cache_set_row_u32(idx, m.state_broadcast_secs); break;
            case IPC_CFG_DETECT_NAME:
                cache_set_row_str(idx, m.name, PHONEAPI_DETECT_NAME_MAX); break;
            case IPC_CFG_DETECT_TRIGGER_TYPE:
                cache_set_row_u8(idx, m.detection_trigger_type); break;
            case IPC_CFG_DETECT_USE_PULLUP:
                cache_set_row_u8(idx, m.use_pullup ? 1u : 0u); break;
            default: break;
            }
        }
        return true;
    }
    case SG_CANNED_MSG: {
        phoneapi_module_canned_msg_t m;
        if (!phoneapi_cache_get_module_canned_msg(&m)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_CANNED_UPDOWN1_ENABLED:
                cache_set_row_u8(idx, m.updown1_enabled ? 1u : 0u); break;
            case IPC_CFG_CANNED_SEND_BELL:
                cache_set_row_u8(idx, m.send_bell ? 1u : 0u); break;
            default: break;
            }
        }
        return true;
    }
    case SG_AMBIENT: {
        phoneapi_module_ambient_t m;
        if (!phoneapi_cache_get_module_ambient(&m)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_AMBIENT_LED_STATE:
                cache_set_row_u8(idx, m.led_state ? 1u : 0u); break;
            case IPC_CFG_AMBIENT_CURRENT:
                cache_set_row_u8(idx, m.current); break;
            case IPC_CFG_AMBIENT_RED:
                cache_set_row_u8(idx, m.red); break;
            case IPC_CFG_AMBIENT_GREEN:
                cache_set_row_u8(idx, m.green); break;
            case IPC_CFG_AMBIENT_BLUE:
                cache_set_row_u8(idx, m.blue); break;
            default: break;
            }
        }
        return true;
    }
    case SG_PAXCOUNTER: {
        phoneapi_module_paxcounter_t m;
        if (!phoneapi_cache_get_module_paxcounter(&m)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_PAX_ENABLED:
                cache_set_row_u8(idx, m.enabled ? 1u : 0u); break;
            case IPC_CFG_PAX_UPDATE_INTERVAL:
                cache_set_row_u32(idx, m.paxcounter_update_interval); break;
            default: break;
            }
        }
        return true;
    }
    case SG_DISPLAY: {
        phoneapi_config_display_t d;
        if (!phoneapi_cache_get_config_display(&d)) return false;
        for (uint8_t i = 0; i < n_keys; ++i) {
            int idx = base + i;
            switch (defs[i].ipc_key) {
            case IPC_CFG_SCREEN_ON_SECS:
                cache_set_row_u32(idx, d.screen_on_secs); break;
            case IPC_CFG_UNITS_METRIC:
                /* Meshtastic enum: 0=METRIC, 1=IMPERIAL → bool "metric". */
                cache_set_row_u8(idx, (d.units == 0u) ? 1u : 0u); break;
            case IPC_CFG_DISPLAY_AUTO_CAROUSEL_SECS:
                cache_set_row_u32(idx, d.auto_screen_carousel_secs); break;
            case IPC_CFG_DISPLAY_FLIP_SCREEN:
                cache_set_row_u8(idx, d.flip_screen ? 1u : 0u); break;
            case IPC_CFG_DISPLAY_OLED:
                cache_set_row_u8(idx, d.oled); break;
            case IPC_CFG_DISPLAY_DISPLAYMODE:
                cache_set_row_u8(idx, d.displaymode); break;
            case IPC_CFG_DISPLAY_HEADING_BOLD:
                cache_set_row_u8(idx, d.heading_bold ? 1u : 0u); break;
            case IPC_CFG_DISPLAY_WAKE_ON_TAP_OR_MOTION:
                cache_set_row_u8(idx, d.wake_on_tap_or_motion ? 1u : 0u); break;
            case IPC_CFG_DISPLAY_COMPASS_ORIENTATION:
                cache_set_row_u8(idx, d.compass_orientation); break;
            case IPC_CFG_DISPLAY_USE_12H_CLOCK:
                cache_set_row_u8(idx, d.use_12h_clock ? 1u : 0u); break;
            case IPC_CFG_DISPLAY_USE_LONG_NODE_NAME:
                cache_set_row_u8(idx, d.use_long_node_name ? 1u : 0u); break;
            case IPC_CFG_DISPLAY_ENABLE_MESSAGE_BUBBLES:
                cache_set_row_u8(idx, d.enable_message_bubbles ? 1u : 0u); break;
            default: break;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

/* Reset cache rows for the given group — cleared on channel switch so the
 * next seed shows the new channel's values. (Across channels we keep dirty
 * flags off because the underlying SETs targeted whichever channel was
 * active when the user pressed OK; once switched, those edits are no
 * longer addressable through this row's screen position.) */
static void reset_group_cache(uint8_t group)
{
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(group, &n_keys);
    if (!defs) return;
    int base = row_to_global_index(group, 0, n_keys + 1);
    if (base < 0) return;
    for (uint8_t i = 0; i < n_keys; ++i) {
        s_cache[base + i].have_value = false;
        s_cache[base + i].dirty      = false;
        s_cache[base + i].value_len  = 0;
    }
}

/* Seed group rows: try cache first, fall back to IPC GET burst for
 * any group the cache doesn't cover (Power / Channel / Owner). */
static void seed_group(uint8_t group)
{
    if (!populate_group_from_cache(group)) {
        send_get_burst(group);
    }
}

/* ── Rendering ───────────────────────────────────────────────────────── */

static void render_browse(void)
{
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(s_cur_group, &n_keys);

    char hdr[80];
    uint8_t pending = pending_count_in_group(s_cur_group);
    char ch_tag[16] = { 0 };
    if (s_cur_group == SG_CHANNEL) {
        snprintf(ch_tag, sizeof(ch_tag), " Ch %u/8",
                 (unsigned)(s_cur_channel_index + 1));
    }
    if (pending > 0) {
        snprintf(hdr, sizeof(hdr), "[%u/%u] %s%s  *%u",
                 (unsigned)(s_cur_group + 1),
                 (unsigned)SG_GROUP_COUNT,
                 settings_group_name((settings_group_t)s_cur_group),
                 ch_tag,
                 (unsigned)pending);
    } else {
        snprintf(hdr, sizeof(hdr), "[%u/%u] %s%s",
                 (unsigned)(s_cur_group + 1),
                 (unsigned)SG_GROUP_COUNT,
                 settings_group_name((settings_group_t)s_cur_group),
                 ch_tag);
    }
    lv_label_set_text(s_header, hdr);

    /* Body: fixed-size buffer; ~10 lines × ~40 chars. */
    char body[640];
    size_t pos = 0;
    int base = row_to_global_index(s_cur_group, 0, n_keys + 1);
    for (uint8_t i = 0; i < n_keys && pos < sizeof(body) - 80; ++i) {
        const settings_key_def_t *d = &defs[i];
        char val[48];
        if (base >= 0 && s_cache[base + i].have_value) {
            format_value(d, s_cache[base + i].value,
                         s_cache[base + i].value_len, val, sizeof(val));
        } else {
            snprintf(val, sizeof(val), "...");
        }
        const char *cursor = (i == s_cur_row) ? "> " : "  ";
        const char *flag   = (base >= 0 && s_cache[base + i].dirty) ? " *" : "";
        pos += (size_t)snprintf(&body[pos], sizeof(body) - pos,
                                "%s%s : %s%s\n",
                                cursor, d->label, val, flag);
    }
    /* Trailing Apply row */
    if (pos < sizeof(body) - 40) {
        const char *cursor = (s_cur_row == n_keys) ? "> " : "  ";
        const char *label;
        if (pending == 0)               label = "[no changes]";
        else if (group_needs_reboot(s_cur_group)) label = "[Apply + reboot]";
        else                            label = "[Apply]";
        snprintf(&body[pos], sizeof(body) - pos, "%s%s\n", cursor, label);
    }
    lv_label_set_text(s_body, body);

    if (s_footer_msg[0]) {
        lv_label_set_text(s_footer, s_footer_msg);
    } else if (s_cur_group == SG_CHANNEL) {
        lv_label_set_text(s_footer,
                          "UP/DN row  L/R Ch  TAB group  OK edit");
    } else {
        lv_label_set_text(s_footer,
                          "UP/DN row  L/R group  TAB next  OK edit");
    }
}

static void render_edit(void)
{
    const settings_key_def_t *d = settings_key_find(s_edit_key);
    if (!d) { s_mode = UI_BROWSE; render_browse(); return; }

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "edit: %s.%s",
             settings_group_name((settings_group_t)d->group), d->label);
    lv_label_set_text(s_header, hdr);

    char body[256];
    char val[48];
    format_value(d, s_edit_buf, (uint8_t)s_edit_len, val, sizeof(val));

    if (d->kind == SK_KIND_STR) {
        snprintf(body, sizeof(body),
                 "current : %s\n"
                 "\n"
                 "(string editing arrives in Stage 3 — IME wiring)",
                 val);
    } else if (d->kind == SK_KIND_ENUM_U8 || d->kind == SK_KIND_BOOL) {
        snprintf(body, sizeof(body),
                 "value : %s\n"
                 "\n"
                 "UP / DOWN  cycle\n"
                 "OK         confirm\n"
                 "BACK       cancel",
                 val);
    } else {
        snprintf(body, sizeof(body),
                 "value : %s\n"
                 "range : %ld .. %ld\n"
                 "\n"
                 "UP / DOWN  ±1\n"
                 "L / R      ±10\n"
                 "OK         confirm\n"
                 "BACK       cancel",
                 val, (long)d->min, (long)d->max);
    }
    lv_label_set_text(s_body, body);

    if (d->needs_reboot) {
        lv_label_set_text(s_footer, "* changing this key requires reboot *");
    } else {
        lv_label_set_text(s_footer, "soft-reload (no reboot)");
    }
}

static void render_all(void)
{
    if (s_mode == UI_EDIT) render_edit();
    else                   render_browse();
}

/* ── Edit-mode key handling ──────────────────────────────────────────── */

static void edit_step(const settings_key_def_t *d, int step)
{
    uint32_t cur_u = value_to_u32(s_edit_buf, (uint8_t)s_edit_len);
    int32_t  cur_i = value_to_i32(s_edit_buf, (uint8_t)s_edit_len);

    switch (d->kind) {
    case SK_KIND_BOOL:
        s_edit_buf[0] = !s_edit_buf[0];
        s_edit_len = 1;
        break;
    case SK_KIND_ENUM_U8: {
        int32_t v = (int32_t)s_edit_buf[0];
        int32_t cnt = (int32_t)d->enum_count;
        if (cnt <= 0) cnt = 1;
        v = ((v + step) % cnt + cnt) % cnt;   /* wrap, accept negative step */
        s_edit_buf[0] = (uint8_t)v;
        s_edit_len = 1;
        break;
    }
    case SK_KIND_U8: {
        int32_t v = (int32_t)cur_u + step;
        if (v < d->min) v = d->min;
        if (v > d->max) v = d->max;
        s_edit_buf[0] = (uint8_t)v;
        s_edit_len = 1;
        break;
    }
    case SK_KIND_I8: {
        int32_t v = cur_i + step;
        if (v < d->min) v = d->min;
        if (v > d->max) v = d->max;
        s_edit_buf[0] = (uint8_t)(int8_t)v;
        s_edit_len = 1;
        break;
    }
    case SK_KIND_U32: {
        int64_t v = (int64_t)cur_u + step;
        if (v < d->min) v = d->min;
        if (v > d->max) v = d->max;
        value_from_u32(s_edit_buf, 4, (uint32_t)v);
        s_edit_len = 4;
        break;
    }
    case SK_KIND_U32_FLAGS: {
        /* ±1 nudges the lowest bit; ±10 shifts a single bit up/down so
         * the user can sweep through likely flag values without a real
         * bit-picker. Full editor lands in a follow-up. */
        uint32_t v = cur_u;
        if (step >= 10)        v <<= 1;
        else if (step <= -10)  v >>= 1;
        else if (step > 0)     v ^= 1u;
        else                   v = (v == 0u) ? 0u : (v - 1u);
        value_from_u32(s_edit_buf, 4, v);
        s_edit_len = 4;
        break;
    }
    default:
        break;
    }
}

static void enter_edit_for_row(uint8_t row)
{
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(s_cur_group, &n_keys);
    if (!defs || row >= n_keys) return;
    int gidx = row_to_global_index(s_cur_group, row, n_keys + 1);
    if (gidx < 0) return;

    s_edit_key = defs[row].ipc_key;

    /* Read-only kinds bail out before allocating the edit overlay. */
    if (defs[row].kind == SK_KIND_BYTES_RO) {
        snprintf(s_footer_msg, sizeof(s_footer_msg),
                 "%s is read-only", defs[row].label);
        s_render_seq++;
        return;
    }

    /* Strings skip the numeric edit overlay entirely — hand off to the
     * generic ime_request_text helper (post-Stage 3 MIE-reuse pattern,
     * supersedes the bespoke modal_enter call site). The current cached
     * value (if any) seeds the IME so the user can edit-in-place. */
    if (defs[row].kind == SK_KIND_STR) {
        char initial[VAL_BUF_MAX + 1];
        const char *prefill = NULL;
        if (s_cache[gidx].have_value && s_cache[gidx].value_len > 0) {
            uint8_t n = s_cache[gidx].value_len;
            if (n > VAL_BUF_MAX) n = VAL_BUF_MAX;
            memcpy(initial, s_cache[gidx].value, n);
            initial[n] = '\0';
            prefill = initial;
        }
        ime_text_request_t req = {
            .prompt    = defs[row].label,
            .initial   = prefill,
            .max_bytes = (uint16_t)defs[row].max,
            .mode_hint = IME_TEXT_MODE_DEFAULT,
            .flags     = IME_TEXT_FLAG_NONE,
            .layout    = IME_TEXT_LAYOUT_FULLSCREEN,
            /* draft_id namespace for Settings = (1u << 31) | ipc_key, so
             * drafts can't collide with future caller namespaces (e.g.
             * conversation_view will use the peer node id directly). The
             * existing prefill takes precedence; the draft only restores
             * if no cached value is present. */
            .draft_id  = (uint32_t)(0x80000000u | (uint32_t)defs[row].ipc_key),
        };
        snprintf(s_footer_msg, sizeof(s_footer_msg),
                 "type %s, FUNC = done", defs[row].label);
        s_render_seq++;
        if (!ime_request_text(&req, str_edit_done,
                              (void *)(uintptr_t)defs[row].ipc_key)) {
            snprintf(s_footer_msg, sizeof(s_footer_msg),
                     "ime busy — try again");
        }
        return;
    }

    if (s_cache[gidx].have_value) {
        s_edit_len = s_cache[gidx].value_len;
        memcpy(s_edit_buf, s_cache[gidx].value, s_edit_len);
    } else {
        memset(s_edit_buf, 0, sizeof(s_edit_buf));
        /* Default-size by kind so step math has somewhere to write. */
        s_edit_len = (defs[row].kind == SK_KIND_U32 ||
                      defs[row].kind == SK_KIND_U32_FLAGS) ? 4 : 1;
    }
    s_mode = UI_EDIT;
    s_render_seq++;
}

/* ime_request_text done callback. Receives the UTF-8 string already
 * truncated to the key's max_bytes; we route it into the cache + IPC
 * SET. The IPC_CFG_* key is passed via ctx (encoded as uintptr_t to
 * avoid an out-of-band file-static). The cache is display-only —
 * long_name shows as a leading-prefix while the full byte_len goes
 * out via the SET payload. */
static void str_edit_done(bool committed, const char *utf8,
                          uint16_t byte_len, void *ctx)
{
    uint16_t key = (uint16_t)(uintptr_t)ctx;
    if (!committed || key == 0) { s_render_seq++; return; }
    if (utf8 == NULL || byte_len == 0) {
        snprintf(s_footer_msg, sizeof(s_footer_msg),
                 "(empty — not committed)");
        s_render_seq++;
        return;
    }
    int gidx = find_key_index(key);
    if (gidx >= 0) {
        size_t cache_len = mie_utf8_truncate(utf8, byte_len, VAL_BUF_MAX);
        s_cache[gidx].value_len  = (uint8_t)cache_len;
        memcpy(s_cache[gidx].value, utf8, cache_len);
        s_cache[gidx].have_value = true;
        s_cache[gidx].dirty      = true;
    }
    uint8_t ch = ((key & 0xFF00u) == 0x0600u) ? s_cur_channel_index : 0u;
    (void)settings_client_send_set(key, ch, utf8, byte_len);
    s_render_seq++;
}

static void confirm_edit(void)
{
    const settings_key_def_t *d = settings_key_find(s_edit_key);
    if (!d) { s_mode = UI_BROWSE; return; }
    /* SK_KIND_STR can't reach here — enter_edit_for_row short-circuits
     * strings into ime_request_text without entering UI_EDIT mode. */

    int gidx = find_key_index(s_edit_key);
    if (gidx < 0) { s_mode = UI_BROWSE; return; }

    /* Cache the new value locally so the browse view shows it immediately. */
    s_cache[gidx].value_len  = (uint8_t)s_edit_len;
    memcpy(s_cache[gidx].value, s_edit_buf, s_edit_len);
    s_cache[gidx].have_value = true;
    s_cache[gidx].dirty      = true;

    /* Push SET to Core 0 — handler accumulates pending_segments. The
     * actual flash write happens at Apply (COMMIT). */
    uint8_t ch = ((s_edit_key & 0xFF00u) == 0x0600u) ? s_cur_channel_index : 0u;
    (void)settings_client_send_set(s_edit_key, ch, s_edit_buf, s_edit_len);

    s_mode = UI_BROWSE;
    s_render_seq++;
}

static void apply_changes(void)
{
    if (pending_count_in_group(s_cur_group) == 0) {
        snprintf(s_footer_msg, sizeof(s_footer_msg), "(no pending changes)");
        s_render_seq++;
        return;
    }
    bool needs_reboot = group_needs_reboot(s_cur_group);
    if (!settings_client_send_commit(needs_reboot)) {
        snprintf(s_footer_msg, sizeof(s_footer_msg),
                 "commit push failed (ring full)");
        s_render_seq++;
        return;
    }
    snprintf(s_footer_msg, sizeof(s_footer_msg),
             needs_reboot ? "applying + reboot..." : "applying...");

    /* Clear dirty flags on the assumption Core 0 will accept; if a
     * later CONFIG_RESULT carries an error we'll surface it, but for
     * now an OK clears the asterisks. */
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(s_cur_group, &n_keys);
    int base = (defs != NULL) ? row_to_global_index(s_cur_group, 0, n_keys + 1) : -1;
    if (base >= 0) {
        for (uint8_t i = 0; i < n_keys; ++i) s_cache[base + i].dirty = false;
    }
    s_render_seq++;
}

/* ── Reply ingestion ─────────────────────────────────────────────────── */

static void drain_replies(void)
{
    settings_client_reply_t r;
    while (settings_client_take_reply(&r)) {
        if (r.kind == (uint8_t)SCR_VALUE) {
            int gidx = find_key_index(r.key);
            if (gidx >= 0 && gidx < (int)KEY_CACHE_MAX) {
                uint8_t n = (uint8_t)((r.value_len < VAL_BUF_MAX)
                                      ? r.value_len : VAL_BUF_MAX);
                s_cache[gidx].value_len  = n;
                memcpy(s_cache[gidx].value, r.value, n);
                s_cache[gidx].have_value = true;
            }
            s_render_seq++;
        } else if (r.kind == (uint8_t)SCR_OK) {
            /* commit OK has key=0; SET OK has key= the IPC_CFG_* */
            if (r.key == 0) {
                snprintf(s_footer_msg, sizeof(s_footer_msg), "applied");
                s_render_seq++;
            }
        } else if (r.kind == (uint8_t)SCR_UNKNOWN_KEY) {
            snprintf(s_footer_msg, sizeof(s_footer_msg),
                     "err: unknown key 0x%04x", (unsigned)r.key);
            s_render_seq++;
        } else if (r.kind == (uint8_t)SCR_INVALID_VALUE) {
            snprintf(s_footer_msg, sizeof(s_footer_msg),
                     "err: invalid value for 0x%04x", (unsigned)r.key);
            s_render_seq++;
        } else if (r.kind == (uint8_t)SCR_BUSY) {
            snprintf(s_footer_msg, sizeof(s_footer_msg), "err: core 0 busy");
            s_render_seq++;
        }
    }
}

/* ── Public entry points ─────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    const lv_font_t *f16 = mie_font_unifont_sm_16();
    s_panel = panel;

    /* settings_client_init() is idempotent (creates the reply queue
     * once); safe to call on every recreate. */
    settings_client_init();

    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);

    s_header = lv_label_create(panel);
    if (f16) lv_obj_set_style_text_font(s_header, f16, 0);
    lv_obj_set_style_text_color(s_header, lv_color_hex(0xFFFF80), 0);
    lv_label_set_text(s_header, "[1/6] Device");
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_width(s_header, 312);

    s_body = lv_label_create(panel);
    if (f16) lv_obj_set_style_text_font(s_body, f16, 0);
    lv_obj_set_style_text_color(s_body, lv_color_white(), 0);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_body, 2, 0);
    lv_label_set_text(s_body, "loading...");
    lv_obj_set_pos(s_body, 0, 24);
    lv_obj_set_size(s_body, 312, 240 - 24 - 24);

    s_footer = lv_label_create(panel);
    if (f16) lv_obj_set_style_text_font(s_footer, f16, 0);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(0x808080), 0);
    lv_label_set_text(s_footer, "");
    lv_obj_set_pos(s_footer, 0, 240 - 24);
    lv_obj_set_width(s_footer, 312);

    s_dirty_load = true;
}

static void apply(const key_event_t *ev)
{
    if (!ev || !ev->pressed) return;

    s_footer_msg[0] = '\0';   /* any keypress clears the sticky status */

    if (s_mode == UI_EDIT) {
        const settings_key_def_t *d = settings_key_find(s_edit_key);
        if (!d) { s_mode = UI_BROWSE; s_render_seq++; return; }

        switch (ev->keycode) {
        case MOKYA_KEY_UP:    edit_step(d, +1);  s_render_seq++; break;
        case MOKYA_KEY_DOWN:  edit_step(d, -1);  s_render_seq++; break;
        case MOKYA_KEY_RIGHT: edit_step(d, +10); s_render_seq++; break;
        case MOKYA_KEY_LEFT:  edit_step(d, -10); s_render_seq++; break;
        case MOKYA_KEY_OK:    confirm_edit();              break;
        case MOKYA_KEY_BACK:  s_mode = UI_BROWSE;
                              s_render_seq++;              break;
        default: break;
        }
        return;
    }

    /* UI_BROWSE */
    uint8_t n_keys = 0;
    (void)settings_keys_in_group(s_cur_group, &n_keys);
    uint8_t row_max = n_keys;   /* extra slot = Apply */

    switch (ev->keycode) {
    case MOKYA_KEY_UP:
        if (s_cur_row > 0) s_cur_row--;
        s_render_seq++;
        break;
    case MOKYA_KEY_DOWN:
        if (s_cur_row < row_max) s_cur_row++;
        s_render_seq++;
        break;
    case MOKYA_KEY_LEFT:
        if (s_cur_group == SG_CHANNEL) {
            s_cur_channel_index =
                (uint8_t)((s_cur_channel_index + PHONEAPI_CHANNEL_COUNT - 1u) % PHONEAPI_CHANNEL_COUNT);
            reset_group_cache(s_cur_group);
            seed_group(s_cur_group);
        } else {
            s_cur_group = (uint8_t)((s_cur_group + SG_GROUP_COUNT - 1) % SG_GROUP_COUNT);
            s_cur_row = 0;
            seed_group(s_cur_group);
        }
        s_render_seq++;
        break;
    case MOKYA_KEY_RIGHT:
        if (s_cur_group == SG_CHANNEL) {
            s_cur_channel_index =
                (uint8_t)((s_cur_channel_index + 1u) % PHONEAPI_CHANNEL_COUNT);
            reset_group_cache(s_cur_group);
            seed_group(s_cur_group);
        } else {
            s_cur_group = (uint8_t)((s_cur_group + 1) % SG_GROUP_COUNT);
            s_cur_row = 0;
            seed_group(s_cur_group);
        }
        s_render_seq++;
        break;
    case MOKYA_KEY_TAB:
        s_cur_group = (uint8_t)((s_cur_group + 1) % SG_GROUP_COUNT);
        s_cur_row = 0;
        seed_group(s_cur_group);
        s_render_seq++;
        break;
    case MOKYA_KEY_OK:
        if (s_cur_row == n_keys) apply_changes();
        else                     enter_edit_for_row(s_cur_row);
        break;
    case MOKYA_KEY_BACK: {
        /* Clear pending dirty flags in this group (cancel pending edits). */
        const settings_key_def_t *defs = settings_keys_in_group(s_cur_group, &n_keys);
        if (defs != NULL) {
            int base = row_to_global_index(s_cur_group, 0, n_keys + 1);
            if (base >= 0) {
                for (uint8_t i = 0; i < n_keys; ++i) s_cache[base + i].dirty = false;
            }
        }
        seed_group(s_cur_group);   /* re-read truth from cache or Core 0 */
        s_render_seq++;
        break;
    }
    default: break;
    }
}

static void refresh(void)
{
    if (s_panel == NULL) return;

    /* Active-only refresh: router no longer calls us when hidden, so
     * the inbox-style "drain even when hidden" branch is gone. The
     * cache is still re-seeded on activation via s_dirty_load. */
    drain_replies();

    if (s_dirty_load) {
        seed_group(s_cur_group);
        s_dirty_load = false;
        s_last_phoneapi_seq = phoneapi_cache_change_seq();
        s_render_seq++;
    } else {
        /* When cascade pushes a fresh Config (e.g. host CLI just SET
         * a value via cascade->AdminModule), phoneapi_cache_change_seq
         * bumps. Re-populate the current group so the row reflects
         * the new value within one LVGL tick. */
        uint32_t seq = phoneapi_cache_change_seq();
        if (seq != s_last_phoneapi_seq) {
            s_last_phoneapi_seq = seq;
            (void)populate_group_from_cache(s_cur_group);
            s_render_seq++;
        }
    }

    if (s_render_seq != s_last_render_seq) {
        s_last_render_seq = s_render_seq;
        render_all();
    }
}

static void destroy(void)
{
    s_panel = s_header = s_body = s_footer = NULL;
    /* Force re-render + GET burst on next activation so widgets reflect
     * latest cache. s_cache / s_mode / s_cur_group / s_cur_row /
     * s_cache dirty flags / s_footer_msg / s_edit_buf all persist
     * across destroy via static .bss — preserves the user's edit
     * state through cache eviction. */
    s_last_render_seq = (uint32_t)-1;
    s_dirty_load = true;
}

static const view_descriptor_t SETTINGS_DESC = {
    .id      = VIEW_ID_SETTINGS,
    .name    = "settings",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
};

const view_descriptor_t *settings_view_descriptor(void)
{
    return &SETTINGS_DESC;
}
