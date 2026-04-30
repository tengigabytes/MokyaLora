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

static const char *const k_rebroadcast_mode[] = {
    "ALL", "ALL_SKIP_DEC", "LOCAL_ONLY", "KNOWN_ONLY",
    "NONE", "CORE_PORTNUMS",
};

static const char *const k_oled_type[] = {
    "AUTO", "SSD1306", "SH1106", "SH1107", "SH1107_128",
};

static const char *const k_displaymode[] = {
    "DEFAULT", "TWOCOLOR", "INVERTED", "COLOR",
};

static const char *const k_compass_orientation[] = {
    "0", "90", "180", "270",
    "0_INV", "90_INV", "180_INV", "270_INV",
};

static const char *const k_fem_lna_mode[] = {
    "DISABLED", "ENABLED", "NOT_PRESENT",
};

static const char *const k_detect_trigger_type[] = {
    "LOGIC_LOW", "LOGIC_HIGH", "FALL_EDGE", "RISE_EDGE",
    "EITHER_LOW", "EITHER_HIGH",
};

/* T2.4.2 SerialConfig.Serial_Baud — 16 entries (proto:383). */
static const char *const k_serial_baud[] = {
    "DEFAULT", "110", "300", "600", "1200", "2400", "4800", "9600",
    "19200", "38400", "57600", "115200", "230400", "460800", "576000",
    "921600",
};

/* T2.4.2 SerialConfig.Serial_Mode — 11 entries (proto:405). */
static const char *const k_serial_mode[] = {
    "DEFAULT", "SIMPLE", "PROTO", "TEXTMSG", "NMEA", "CALTOPO",
    "WS85", "VE_DIRECT", "MS_CONFIG", "LOG", "LOGTEXT",
};

/* S-7.10 RemoteHardwarePinType — 3 entries (module_config.proto:982). */
static const char *const k_rhw_pin_type[] = {
    "UNKNOWN", "DIGITAL_READ", "DIGITAL_WRITE",
};

/* ── Master key table ────────────────────────────────────────────────── *
 *
 * Order matters: settings_keys_in_group() returns a contiguous slice
 * keyed by the `group` field, so all entries with the same group must
 * be consecutive. Keep groups in the same order as settings_group_t.
 */

static const settings_key_def_t k_keys[] = {
    /* ── Device ─────────────────────────────────────────────────────── *
     * Reboot semantics from AdminModule.cpp:675–679 — only role,
     * rebroadcast_mode, button_gpio, buzzer_gpio differences trigger
     * reboot. Everything else stays live. */
    { IPC_CFG_DEVICE_ROLE, SG_DEVICE, SK_KIND_ENUM_U8,
      0, 11, /*reboot=*/1, "role",
      k_device_role, sizeof(k_device_role)/sizeof(k_device_role[0]) },
    { IPC_CFG_DEVICE_REBROADCAST_MODE, SG_DEVICE, SK_KIND_ENUM_U8,
      0, 5, /*reboot=*/1, "rebcast",
      k_rebroadcast_mode, sizeof(k_rebroadcast_mode)/sizeof(k_rebroadcast_mode[0]) },
    { IPC_CFG_DEVICE_NODE_INFO_BCAST_SECS, SG_DEVICE, SK_KIND_U32,
      0, 86400, /*reboot=*/0, "ni_bcast_s",
      NULL, 0 },
    { IPC_CFG_DEVICE_DOUBLE_TAP_BTN, SG_DEVICE, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "dbl_tap_btn",
      NULL, 0 },
    { IPC_CFG_DEVICE_DISABLE_TRIPLE_CLICK, SG_DEVICE, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "tri_click_off",
      NULL, 0 },
    { IPC_CFG_DEVICE_TZDEF, SG_DEVICE, SK_KIND_STR,
      0, 64, /*reboot=*/0, "tzdef",
      NULL, 0 },
    { IPC_CFG_DEVICE_LED_HEARTBEAT_DISABLED, SG_DEVICE, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "led_hb_off",
      NULL, 0 },

    /* ── LoRa ───────────────────────────────────────────────────────── *
     * AdminModule.cpp:847 — requiresReboot = false unconditionally for
     * every LoRa key. configChanged observer reconfigures the radio
     * live (standby → reprogram → restart receive). */
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
    { IPC_CFG_LORA_USE_PRESET, SG_LORA, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "use_preset",
      NULL, 0 },
    { IPC_CFG_LORA_BANDWIDTH, SG_LORA, SK_KIND_U32,
      0, 500, /*reboot=*/0, "bw",
      NULL, 0 },
    { IPC_CFG_LORA_SPREAD_FACTOR, SG_LORA, SK_KIND_U32,
      7, 12, /*reboot=*/0, "sf",
      NULL, 0 },
    { IPC_CFG_LORA_CODING_RATE, SG_LORA, SK_KIND_U32,
      5, 8, /*reboot=*/0, "cr",
      NULL, 0 },
    { IPC_CFG_LORA_TX_ENABLED, SG_LORA, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "tx_enable",
      NULL, 0 },
    { IPC_CFG_LORA_OVERRIDE_DUTY_CYCLE, SG_LORA, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "ovr_duty",
      NULL, 0 },
    { IPC_CFG_LORA_SX126X_RX_BOOSTED_GAIN, SG_LORA, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "rx_boost",
      NULL, 0 },
    { IPC_CFG_LORA_FEM_LNA_MODE, SG_LORA, SK_KIND_ENUM_U8,
      0, 2, /*reboot=*/0, "fem_lna",
      k_fem_lna_mode, sizeof(k_fem_lna_mode)/sizeof(k_fem_lna_mode[0]) },

    /* ── Position ───────────────────────────────────────────────────── *
     * AdminModule.cpp:717–731 — no short-circuit, all PositionConfig
     * keys remain reboot=true. (Mesh radio re-init is implicit on
     * GPS / position-broadcast changes.) */
    { IPC_CFG_GPS_MODE, SG_POSITION, SK_KIND_ENUM_U8,
      0, 2, /*reboot=*/1, "gps_mode",
      k_gps_mode, sizeof(k_gps_mode)/sizeof(k_gps_mode[0]) },
    { IPC_CFG_GPS_UPDATE_INTERVAL, SG_POSITION, SK_KIND_U32,
      0, 86400, /*reboot=*/1, "gps_int_s",
      NULL, 0 },
    { IPC_CFG_POSITION_BCAST_SECS, SG_POSITION, SK_KIND_U32,
      0, 86400, /*reboot=*/1, "pos_bcast_s",
      NULL, 0 },
    { IPC_CFG_POSITION_BCAST_SMART_ENABLED, SG_POSITION, SK_KIND_BOOL,
      0, 1, /*reboot=*/1, "smart_pos",
      NULL, 0 },
    { IPC_CFG_POSITION_FIXED_POSITION, SG_POSITION, SK_KIND_BOOL,
      0, 1, /*reboot=*/1, "fixed_pos",
      NULL, 0 },
    { IPC_CFG_POSITION_FLAGS, SG_POSITION, SK_KIND_U32_FLAGS,
      0, (int32_t)0xFFFFFFFF, /*reboot=*/1, "pos_flags",
      NULL, 0 },
    { IPC_CFG_POSITION_BCAST_SMART_MIN_DIST, SG_POSITION, SK_KIND_U32,
      0, 100000, /*reboot=*/1, "smart_min_d",
      NULL, 0 },
    { IPC_CFG_POSITION_BCAST_SMART_MIN_INT_SECS, SG_POSITION, SK_KIND_U32,
      0, 86400, /*reboot=*/1, "smart_min_s",
      NULL, 0 },

    /* ── Power ──────────────────────────────────────────────────────── *
     * AdminModule.cpp:736–744 — short-circuit checks 7 fields; any
     * change among {device_battery_ina_address, is_power_saving,
     * ls_secs, min_wake_secs, on_battery_shutdown_after_secs, sds_secs,
     * wait_bluetooth_secs} triggers reboot. powermon_enables is NOT
     * in the comparison so it stays live. */
    { IPC_CFG_POWER_SAVING, SG_POWER, SK_KIND_BOOL,
      0, 1, /*reboot=*/1, "save_pwr",
      NULL, 0 },
    { IPC_CFG_SHUTDOWN_AFTER_SECS, SG_POWER, SK_KIND_U32,
      0, 86400, /*reboot=*/1, "shut_after_s",
      NULL, 0 },
    { IPC_CFG_POWER_SDS_SECS, SG_POWER, SK_KIND_U32,
      0, 86400, /*reboot=*/1, "sds_s",
      NULL, 0 },
    { IPC_CFG_POWER_LS_SECS, SG_POWER, SK_KIND_U32,
      0, 86400, /*reboot=*/1, "ls_s",
      NULL, 0 },
    { IPC_CFG_POWER_MIN_WAKE_SECS, SG_POWER, SK_KIND_U32,
      0, 86400, /*reboot=*/1, "min_wake_s",
      NULL, 0 },
    { IPC_CFG_POWER_BATTERY_INA_ADDRESS, SG_POWER, SK_KIND_U32,
      0, 0x7F, /*reboot=*/1, "ina_i2c",
      NULL, 0 },
    { IPC_CFG_POWER_POWERMON_ENABLES, SG_POWER, SK_KIND_U32_FLAGS,
      0, (int32_t)0xFFFFFFFF, /*reboot=*/0, "pwrmon",
      NULL, 0 },

    /* ── Display ────────────────────────────────────────────────────── *
     * AdminModule.cpp:760–764 — only screen_on_secs / flip_screen /
     * oled / displaymode require reboot when changed. Other keys (units,
     * carousel, heading_bold, wake_on_tap_or_motion, compass_orientation,
     * use_12h_clock, use_long_node_name, enable_message_bubbles) take
     * effect live.  Note: SCREEN_ON_SECS legacy entry below still says
     * reboot=0 — known mismatch with AdminModule, NOT fixed in B3-P1
     * to avoid scope creep. */
    { IPC_CFG_SCREEN_ON_SECS, SG_DISPLAY, SK_KIND_U32,
      0, 86400, /*reboot=*/0, "screen_on_s",
      NULL, 0 },
    { IPC_CFG_UNITS_METRIC, SG_DISPLAY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "metric",
      NULL, 0 },
    { IPC_CFG_DISPLAY_AUTO_CAROUSEL_SECS, SG_DISPLAY, SK_KIND_U32,
      0, 86400, /*reboot=*/0, "carousel_s",
      NULL, 0 },
    { IPC_CFG_DISPLAY_FLIP_SCREEN, SG_DISPLAY, SK_KIND_BOOL,
      0, 1, /*reboot=*/1, "flip",
      NULL, 0 },
    { IPC_CFG_DISPLAY_OLED, SG_DISPLAY, SK_KIND_ENUM_U8,
      0, 4, /*reboot=*/1, "oled",
      k_oled_type, sizeof(k_oled_type)/sizeof(k_oled_type[0]) },
    { IPC_CFG_DISPLAY_DISPLAYMODE, SG_DISPLAY, SK_KIND_ENUM_U8,
      0, 3, /*reboot=*/1, "mode",
      k_displaymode, sizeof(k_displaymode)/sizeof(k_displaymode[0]) },
    { IPC_CFG_DISPLAY_HEADING_BOLD, SG_DISPLAY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "bold",
      NULL, 0 },
    { IPC_CFG_DISPLAY_WAKE_ON_TAP_OR_MOTION, SG_DISPLAY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "wake_motion",
      NULL, 0 },
    { IPC_CFG_DISPLAY_COMPASS_ORIENTATION, SG_DISPLAY, SK_KIND_ENUM_U8,
      0, 7, /*reboot=*/0, "compass",
      k_compass_orientation, sizeof(k_compass_orientation)/sizeof(k_compass_orientation[0]) },
    { IPC_CFG_DISPLAY_USE_12H_CLOCK, SG_DISPLAY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "12h_clock",
      NULL, 0 },
    { IPC_CFG_DISPLAY_USE_LONG_NODE_NAME, SG_DISPLAY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "long_name",
      NULL, 0 },
    { IPC_CFG_DISPLAY_ENABLE_MESSAGE_BUBBLES, SG_DISPLAY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "msg_bubbles",
      NULL, 0 },

    /* ── Channel ────────────────────────────────────────────────────── *
     *
     * Primary channel only — IPC protocol does not yet carry a channel
     * index field, so SETs land on `channelFile.channels[0]`. Editing
     * non-primary channels needs a protocol extension (open follow-up
     * in b2-mossy-brooks.md). PSK editing is deferred — bytes payload
     * needs a hex / base64 picker the current SK_KIND_STR can't do. */
    { IPC_CFG_CHANNEL_NAME, SG_CHANNEL, SK_KIND_STR,
      0, 11, /*reboot=*/0, "name",
      NULL, 0 },
    { IPC_CFG_CHANNEL_MODULE_POSITION_PRECISION, SG_CHANNEL, SK_KIND_U32,
      0, 32, /*reboot=*/0, "pos_prec",
      NULL, 0 },
    { IPC_CFG_CHANNEL_MODULE_IS_MUTED, SG_CHANNEL, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "muted",
      NULL, 0 },

    /* ── Owner ──────────────────────────────────────────────────────── */
    { IPC_CFG_OWNER_LONG_NAME, SG_OWNER, SK_KIND_STR,
      0, 39, /*reboot=*/0, "long_name",
      NULL, 0 },
    { IPC_CFG_OWNER_SHORT_NAME, SG_OWNER, SK_KIND_STR,
      0, 4, /*reboot=*/0, "short_name",
      NULL, 0 },
    { IPC_CFG_OWNER_IS_LICENSED, SG_OWNER, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "licensed",
      NULL, 0 },
    { IPC_CFG_OWNER_PUBLIC_KEY, SG_OWNER, SK_KIND_BYTES_RO,
      0, 32, /*reboot=*/0, "pubkey",
      NULL, 0 },

    /* ── Security ───────────────────────────────────────────────────── *
     * AdminModule.cpp:915–917 — only debug_log_api_enabled +
     * serial_enabled gate the reboot=false short-circuit; changing
     * either pushes reboot=true. is_managed / admin_channel_enabled
     * stay live. private_key + admin_key[] intentionally NOT exposed. */
    { IPC_CFG_SECURITY_PUBLIC_KEY, SG_SECURITY, SK_KIND_BYTES_RO,
      0, 32, /*reboot=*/0, "pubkey",
      NULL, 0 },
    { IPC_CFG_SECURITY_IS_MANAGED, SG_SECURITY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "managed",
      NULL, 0 },
    { IPC_CFG_SECURITY_SERIAL_ENABLED, SG_SECURITY, SK_KIND_BOOL,
      0, 1, /*reboot=*/1, "serial",
      NULL, 0 },
    { IPC_CFG_SECURITY_DEBUG_LOG_API_ENABLED, SG_SECURITY, SK_KIND_BOOL,
      0, 1, /*reboot=*/1, "dbg_log",
      NULL, 0 },
    { IPC_CFG_SECURITY_ADMIN_CHANNEL_ENABLED, SG_SECURITY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "admin_ch",
      NULL, 0 },

    /* ── Telemetry (B3-P3) ───────────────────────────────────────── *
     * AdminModule.cpp:933 saveChanges(SEGMENT_MODULECONFIG, true) by
     * default, but our IPC soft-reload via reloadConfig fires the
     * configChanged observer; TelemetryModule re-reads its config so
     * reboot is not required. */
    { IPC_CFG_TELEM_DEVICE_UPDATE_INTERVAL, SG_TELEMETRY, SK_KIND_U32,
      0, 86400, /*reboot=*/0, "dev_int_s",
      NULL, 0 },
    { IPC_CFG_TELEM_ENV_UPDATE_INTERVAL, SG_TELEMETRY, SK_KIND_U32,
      0, 86400, /*reboot=*/0, "env_int_s",
      NULL, 0 },
    { IPC_CFG_TELEM_ENV_MEASUREMENT_ENABLED, SG_TELEMETRY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "env_meas",
      NULL, 0 },
    { IPC_CFG_TELEM_ENV_SCREEN_ENABLED, SG_TELEMETRY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "env_screen",
      NULL, 0 },
    { IPC_CFG_TELEM_ENV_DISPLAY_FAHRENHEIT, SG_TELEMETRY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "env_fahr",
      NULL, 0 },
    { IPC_CFG_TELEM_POWER_MEASUREMENT_ENABLED, SG_TELEMETRY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "pwr_meas",
      NULL, 0 },
    { IPC_CFG_TELEM_POWER_UPDATE_INTERVAL, SG_TELEMETRY, SK_KIND_U32,
      0, 86400, /*reboot=*/0, "pwr_int_s",
      NULL, 0 },
    { IPC_CFG_TELEM_POWER_SCREEN_ENABLED, SG_TELEMETRY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "pwr_screen",
      NULL, 0 },
    { IPC_CFG_TELEM_DEVICE_TELEM_ENABLED, SG_TELEMETRY, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "dev_telem",
      NULL, 0 },

    /* ── NeighborInfo (B3-P3) ────────────────────────────────────── *
     * update_interval has a Meshtastic-side floor of 14400 s
     * (4 h) per AdminModule.cpp:1008. */
    { IPC_CFG_NEIGHBOR_ENABLED, SG_NEIGHBOR, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "enabled",
      NULL, 0 },
    { IPC_CFG_NEIGHBOR_UPDATE_INTERVAL, SG_NEIGHBOR, SK_KIND_U32,
      14400, 86400, /*reboot=*/0, "int_s",
      NULL, 0 },
    { IPC_CFG_NEIGHBOR_TRANSMIT_OVER_LORA, SG_NEIGHBOR, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "tx_lora",
      NULL, 0 },

    /* ── RangeTest (B3-P3) ───────────────────────────────────────── */
    { IPC_CFG_RANGETEST_ENABLED, SG_RANGE_TEST, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "enabled",
      NULL, 0 },
    { IPC_CFG_RANGETEST_SENDER, SG_RANGE_TEST, SK_KIND_U32,
      0, 86400, /*reboot=*/0, "sender_s",
      NULL, 0 },

    /* ── DetectionSensor (B3-P4) ─────────────────────────────────── */
    { IPC_CFG_DETECT_ENABLED, SG_DETECT_SENSOR, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "enabled",
      NULL, 0 },
    { IPC_CFG_DETECT_MIN_BCAST_SECS, SG_DETECT_SENSOR, SK_KIND_U32,
      0, 86400, /*reboot=*/0, "min_bc_s",
      NULL, 0 },
    { IPC_CFG_DETECT_STATE_BCAST_SECS, SG_DETECT_SENSOR, SK_KIND_U32,
      0, 86400, /*reboot=*/0, "state_bc_s",
      NULL, 0 },
    { IPC_CFG_DETECT_NAME, SG_DETECT_SENSOR, SK_KIND_STR,
      0, 19, /*reboot=*/0, "name",
      NULL, 0 },
    { IPC_CFG_DETECT_TRIGGER_TYPE, SG_DETECT_SENSOR, SK_KIND_ENUM_U8,
      0, 5, /*reboot=*/0, "trigger",
      k_detect_trigger_type,
      sizeof(k_detect_trigger_type)/sizeof(k_detect_trigger_type[0]) },
    { IPC_CFG_DETECT_USE_PULLUP, SG_DETECT_SENSOR, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "pullup",
      NULL, 0 },

    /* ── CannedMessage (B3-P4) ───────────────────────────────────── */
    { IPC_CFG_CANNED_UPDOWN1_ENABLED, SG_CANNED_MSG, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "updown1_en",
      NULL, 0 },
    { IPC_CFG_CANNED_SEND_BELL, SG_CANNED_MSG, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "send_bell",
      NULL, 0 },

    /* ── AmbientLighting (B3-P4) ─────────────────────────────────── */
    { IPC_CFG_AMBIENT_LED_STATE, SG_AMBIENT, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "led_on",
      NULL, 0 },
    { IPC_CFG_AMBIENT_CURRENT, SG_AMBIENT, SK_KIND_U8,
      0, 255, /*reboot=*/0, "current",
      NULL, 0 },
    { IPC_CFG_AMBIENT_RED, SG_AMBIENT, SK_KIND_U8,
      0, 255, /*reboot=*/0, "red",
      NULL, 0 },
    { IPC_CFG_AMBIENT_GREEN, SG_AMBIENT, SK_KIND_U8,
      0, 255, /*reboot=*/0, "green",
      NULL, 0 },
    { IPC_CFG_AMBIENT_BLUE, SG_AMBIENT, SK_KIND_U8,
      0, 255, /*reboot=*/0, "blue",
      NULL, 0 },

    /* ── Paxcounter (B3-P4) ──────────────────────────────────────── */
    { IPC_CFG_PAX_ENABLED, SG_PAXCOUNTER, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "enabled",
      NULL, 0 },
    { IPC_CFG_PAX_UPDATE_INTERVAL, SG_PAXCOUNTER, SK_KIND_U32,
      0, 86400, /*reboot=*/0, "int_s",
      NULL, 0 },

    /* ── StoreForward (T2.4.1) ───────────────────────────────────── *
     * `records` and `history_return_*` are uint32 but practical values
     * stay under 1k entries / 24h windows; cap at 100000 so the number
     * template clamps cleanly without overflow risk on tiny screens. */
    { IPC_CFG_SF_ENABLED, SG_STORE_FORWARD, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "enabled",
      NULL, 0 },
    { IPC_CFG_SF_HEARTBEAT, SG_STORE_FORWARD, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "heartbeat",
      NULL, 0 },
    { IPC_CFG_SF_RECORDS, SG_STORE_FORWARD, SK_KIND_U32,
      0, 100000, /*reboot=*/0, "records",
      NULL, 0 },
    { IPC_CFG_SF_HISTORY_RETURN_MAX, SG_STORE_FORWARD, SK_KIND_U32,
      0, 100000, /*reboot=*/0, "hist_max",
      NULL, 0 },
    { IPC_CFG_SF_HISTORY_RETURN_WINDOW, SG_STORE_FORWARD, SK_KIND_U32,
      0, 86400, /*reboot=*/0, "hist_win_s",
      NULL, 0 },
    { IPC_CFG_SF_IS_SERVER, SG_STORE_FORWARD, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "is_server",
      NULL, 0 },

    /* ── Serial (T2.4.2) ─────────────────────────────────────────── */
    { IPC_CFG_SERIAL_ENABLED, SG_SERIAL, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "enabled",
      NULL, 0 },
    { IPC_CFG_SERIAL_ECHO, SG_SERIAL, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "echo",
      NULL, 0 },
    { IPC_CFG_SERIAL_RXD, SG_SERIAL, SK_KIND_U32,
      0, 47, /*reboot=*/0, "rxd_gpio",
      NULL, 0 },
    { IPC_CFG_SERIAL_TXD, SG_SERIAL, SK_KIND_U32,
      0, 47, /*reboot=*/0, "txd_gpio",
      NULL, 0 },
    { IPC_CFG_SERIAL_BAUD, SG_SERIAL, SK_KIND_ENUM_U8,
      0, 15, /*reboot=*/0, "baud",
      k_serial_baud, sizeof(k_serial_baud)/sizeof(k_serial_baud[0]) },
    { IPC_CFG_SERIAL_TIMEOUT, SG_SERIAL, SK_KIND_U32,
      0, 60000, /*reboot=*/0, "timeout_ms",
      NULL, 0 },
    { IPC_CFG_SERIAL_MODE, SG_SERIAL, SK_KIND_ENUM_U8,
      0, 10, /*reboot=*/0, "mode",
      k_serial_mode, sizeof(k_serial_mode)/sizeof(k_serial_mode[0]) },
    { IPC_CFG_SERIAL_OVERRIDE_CONSOLE, SG_SERIAL, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "override_con",
      NULL, 0 },

    /* ── ExternalNotification (T2.4.3) ───────────────────────────── */
    { IPC_CFG_EXTNOT_ENABLED, SG_EXT_NOTIF, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "enabled",
      NULL, 0 },
    { IPC_CFG_EXTNOT_OUTPUT_MS, SG_EXT_NOTIF, SK_KIND_U32,
      0, 60000, /*reboot=*/0, "output_ms",
      NULL, 0 },
    { IPC_CFG_EXTNOT_OUTPUT, SG_EXT_NOTIF, SK_KIND_U32,
      0, 47, /*reboot=*/0, "output_gpio",
      NULL, 0 },
    { IPC_CFG_EXTNOT_OUTPUT_VIBRA, SG_EXT_NOTIF, SK_KIND_U32,
      0, 47, /*reboot=*/0, "vibra_gpio",
      NULL, 0 },
    { IPC_CFG_EXTNOT_OUTPUT_BUZZER, SG_EXT_NOTIF, SK_KIND_U32,
      0, 47, /*reboot=*/0, "buzzer_gpio",
      NULL, 0 },
    { IPC_CFG_EXTNOT_ACTIVE, SG_EXT_NOTIF, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "active_hi",
      NULL, 0 },
    { IPC_CFG_EXTNOT_ALERT_MESSAGE, SG_EXT_NOTIF, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "alert_msg",
      NULL, 0 },
    { IPC_CFG_EXTNOT_ALERT_MESSAGE_VIBRA, SG_EXT_NOTIF, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "alert_msg_v",
      NULL, 0 },
    { IPC_CFG_EXTNOT_ALERT_MESSAGE_BUZZER, SG_EXT_NOTIF, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "alert_msg_b",
      NULL, 0 },
    { IPC_CFG_EXTNOT_ALERT_BELL, SG_EXT_NOTIF, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "alert_bell",
      NULL, 0 },
    { IPC_CFG_EXTNOT_ALERT_BELL_VIBRA, SG_EXT_NOTIF, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "alert_bell_v",
      NULL, 0 },
    { IPC_CFG_EXTNOT_ALERT_BELL_BUZZER, SG_EXT_NOTIF, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "alert_bell_b",
      NULL, 0 },
    { IPC_CFG_EXTNOT_USE_PWM, SG_EXT_NOTIF, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "use_pwm",
      NULL, 0 },
    { IPC_CFG_EXTNOT_NAG_TIMEOUT, SG_EXT_NOTIF, SK_KIND_U32,
      0, 3600, /*reboot=*/0, "nag_s",
      NULL, 0 },
    { IPC_CFG_EXTNOT_USE_I2S_AS_BUZZER, SG_EXT_NOTIF, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "i2s_as_buz",
      NULL, 0 },

    /* ── RemoteHardware (T2.4.4 + S-7.10 available_pins[]) ───────── */
    { IPC_CFG_RHW_ENABLED, SG_REMOTE_HW, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "enabled",
      NULL, 0 },
    { IPC_CFG_RHW_ALLOW_UNDEFINED_PIN_ACCESS, SG_REMOTE_HW, SK_KIND_BOOL,
      0, 1, /*reboot=*/0, "allow_undef",
      NULL, 0 },
    /* available_pins[] — S-7.10. Slot 0..3 carried in IPC channel_index
     * field; this metadata describes the per-slot value semantics. */
    { IPC_CFG_RHW_PIN_COUNT, SG_REMOTE_HW, SK_KIND_U8,
      0, 4, /*reboot=*/0, "pin_count",
      NULL, 0 },
    { IPC_CFG_RHW_PIN_GPIO, SG_REMOTE_HW, SK_KIND_U8,
      0, 255, /*reboot=*/0, "pin_gpio",
      NULL, 0 },
    { IPC_CFG_RHW_PIN_NAME, SG_REMOTE_HW, SK_KIND_STR,
      0, 14, /*reboot=*/0, "pin_name",
      NULL, 0 },
    { IPC_CFG_RHW_PIN_TYPE, SG_REMOTE_HW, SK_KIND_ENUM_U8,
      0, 2, /*reboot=*/0, "pin_type",
      k_rhw_pin_type, sizeof(k_rhw_pin_type)/sizeof(k_rhw_pin_type[0]) },
};

#define KEY_COUNT  ((uint8_t)(sizeof(k_keys) / sizeof(k_keys[0])))

static const char *const k_group_names[SG_GROUP_COUNT] = {
    [SG_DEVICE]   = "Device",
    [SG_LORA]     = "LoRa",
    [SG_POSITION] = "Position",
    [SG_POWER]    = "Power",
    [SG_DISPLAY]  = "Display",
    [SG_CHANNEL]  = "Channel",
    [SG_OWNER]    = "Owner",
    [SG_SECURITY] = "Security",
    [SG_TELEMETRY]    = "Telemetry",
    [SG_NEIGHBOR]     = "Neighbor",
    [SG_RANGE_TEST]   = "RangeTest",
    [SG_DETECT_SENSOR] = "DetectSnsr",
    [SG_CANNED_MSG]   = "CannedMsg",
    [SG_AMBIENT]      = "Ambient",
    [SG_PAXCOUNTER]   = "Paxcounter",
    [SG_STORE_FORWARD]= "StoreForward",
    [SG_SERIAL]       = "Serial",
    [SG_EXT_NOTIF]    = "ExtNotif",
    [SG_REMOTE_HW]    = "RemoteHW",
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
