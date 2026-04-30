/**
 * ipc_protocol.h — MokyaLora Inter-Core Communication Protocol
 * SPDX-License-Identifier: MIT
 *
 * This file is the ONLY shared header between Core 0 (GPL-3.0, Meshtastic) and
 * Core 1 (Apache-2.0, UI/App). Core 1 must never #include any Meshtastic header.
 *
 * Transport: RP2350 inter-core FIFOs (32-bit words) + shared SRAM message buffer.
 *
 * Protocol:
 *   1. Sender writes a MsgHeader into the shared buffer at a pre-agreed address.
 *   2. Sender pushes the buffer offset (uint32) into the FIFO.
 *   3. Receiver reads the FIFO, then reads the message from the buffer.
 *
 * All multi-byte fields are little-endian.
 */

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Message IDs ──────────────────────────────────────────────────────────── */

typedef enum {
    /* Core 0 → Core 1 (notifications)
     *
     * IDs 0x01 (RX_TEXT), 0x02 (NODE_UPDATE), 0x04 (TX_ACK) and
     * 0x81 (CMD_SEND_TEXT) were retired in M5E.3 (2026-04-28). The
     * Core 1 cascade PhoneAPI client now consumes those events
     * directly from the FromRadio byte stream carried in
     * IPC_MSG_SERIAL_BYTES, and emits ToRadio (text send) on the
     * c1→c0 SERIAL_BYTES ring. The numeric values are reserved —
     * do NOT reassign them. */
    IPC_MSG_DEVICE_STATUS  = 0x03,  ///< Periodic status update (battery, GPS, RSSI)
    IPC_MSG_CHANNEL_UPDATE = 0x05,  ///< Channel configuration changed
    IPC_MSG_SERIAL_BYTES   = 0x06,  ///< Raw serial byte stream (Phase 2 M1 byte bridge, ≤256 B payload)
    IPC_MSG_CONFIG_VALUE   = 0x07,  ///< Config value response or unsolicited push on change
    IPC_MSG_CONFIG_RESULT  = 0x08,  ///< Set/commit result (OK / error code)
    IPC_MSG_REBOOT_NOTIFY  = 0x09,  ///< Core 0 about to reboot — Core 1 should disconnect USB

    /* Core 1 → Core 0 (commands) */
    IPC_CMD_SET_CHANNEL    = 0x82,  ///< Set active channel
    IPC_CMD_SET_TX_POWER   = 0x83,  ///< Set LoRa TX power (dBm)
    IPC_CMD_REQUEST_STATUS = 0x84,  ///< Request an immediate IPC_MSG_DEVICE_STATUS
    IPC_CMD_SET_NODE_ALIAS = 0x85,  ///< Assign a user-defined alias to a node ID
    IPC_CMD_POWER_STATE    = 0x86,  ///< Notify Core 0 of a power state transition
    IPC_CMD_REBOOT         = 0x87,  ///< Request system reboot
    IPC_CMD_FACTORY_RESET  = 0x88,  ///< Request factory reset (wipes persistent config)
    IPC_CMD_GET_CONFIG     = 0x89,  ///< Request config value by key
    IPC_CMD_SET_CONFIG     = 0x8A,  ///< Set config value by key
    IPC_CMD_COMMIT_CONFIG  = 0x8B,  ///< Commit pending changes via soft reload (no MCU reset).
                                    ///<   Core 0 calls service->reloadConfig(saveWhat) for the
                                    ///<   accumulated SEGMENT_* bitmask plus reloadOwner() if any
                                    ///<   owner key was SET. Use for keys whose Meshtastic
                                    ///<   handler accepts requiresReboot=false (LoRa subset,
                                    ///<   display screen-on/flip, owner, channel, etc).
    IPC_CMD_COMMIT_REBOOT  = 0x8C,  ///< Commit pending changes AND graceful reboot.
                                    ///<   Same flash + reload as 0x8B, then triggers the P2-10
                                    ///<   RebootNotifier path: Core 0 pushes IPC_MSG_REBOOT_NOTIFY,
                                    ///<   Core 1 calls tud_disconnect(), watchdog resets the chip.
                                    ///<   Caller (Core 1 settings UI) is responsible for choosing
                                    ///<   this path when any SET-edited key is reboot-required
                                    ///<   (device.role, rebroadcast_mode, position.gps_mode,
                                    ///<   power.is_power_saving, etc).

    /* Bidirectional (debug) */
    IPC_MSG_LOG_LINE       = 0xF0,  ///< Debug log line (either core → other core)
    IPC_MSG_PANIC          = 0xFE,  ///< Cross-core panic notification (reserved, M6)

    /* Boot handshake */
    IPC_BOOT_READY         = 0xFF,  ///< Sent by each core when its init is complete
} IpcMsgId;

/* ── Ring / transport parameters (Phase 2 M1) ─────────────────────────────── */

#define IPC_MSG_PAYLOAD_MAX        256u   ///< Max payload bytes per ring slot
#define IPC_RING_SLOT_COUNT         32u   ///< SPSC ring depth (data + cmd rings)
#define IPC_LOG_RING_SLOT_COUNT     16u   ///< SPSC ring depth (log ring, best-effort)

/* ── Common header (every message starts with this) ───────────────────────── */

typedef struct {
    uint8_t  msg_id;        ///< IpcMsgId
    uint8_t  seq;           ///< Rolling sequence number (for duplicate detection)
    uint16_t payload_len;   ///< Byte length of the payload that follows
} IpcMsgHeader;             /* 4 bytes */

/* ── Payload structures ───────────────────────────────────────────────────── */

/* IpcPayloadText (RX_TEXT / CMD_SEND_TEXT) and IpcPayloadNodeUpdate
 * (NODE_UPDATE) were retired in M5E.3. Cascade PhoneAPI client on
 * Core 1 now reads/writes equivalent state via the FromRadio /
 * ToRadio byte stream. */

/** IPC_MSG_DEVICE_STATUS */
typedef struct {
    uint16_t battery_mv;            ///< Battery voltage in mV
    uint8_t  battery_pct;           ///< State of charge 0–100 (%)
    uint8_t  charging;              ///< 1 = USB charging active
    int16_t  lora_rssi;             ///< Last LoRa RSSI (dBm)
    int8_t   lora_snr_x4;          ///< Last LoRa SNR × 4
    uint8_t  gps_sats;              ///< GPS satellites in use
    int32_t  lat_e7;                ///< Self latitude  × 1e7
    int32_t  lon_e7;                ///< Self longitude × 1e7
    int32_t  alt_mm;                ///< Altitude in mm (above MSL)
    uint32_t uptime_s;              ///< System uptime in seconds
} IpcPayloadDeviceStatus;

/* IpcPayloadTxAck (TX_ACK) retired in M5E.3 — cascade decoder on
 * Core 1 now extracts routing-layer ACK / queue_status from the
 * FromRadio byte stream (see firmware/core1/src/phoneapi/). */

/** IPC_CMD_SET_TX_POWER */
typedef struct {
    int8_t   power_dbm;            ///< TX power in dBm (e.g. 14, 20, 22)
} IpcPayloadSetTxPower;

/** IPC_CMD_SET_NODE_ALIAS */
typedef struct {
    uint32_t node_id;
    uint8_t  alias_len;
    uint8_t  alias[];               ///< UTF-8 string
} IpcPayloadSetNodeAlias;

/** IPC_CMD_POWER_STATE — Core 1 informs Core 0 of a power state change */
typedef enum {
    IPC_POWER_ACTIVE    = 0,        ///< Normal operation
    IPC_POWER_IDLE      = 1,        ///< Screen off, CPU at reduced rate
    IPC_POWER_SLEEP     = 2,        ///< Deep sleep — Core 1 suspending FreeRTOS tasks
    IPC_POWER_SHIPPING  = 3,        ///< Factory shipping mode — all peripherals off
} IpcPowerState;

typedef struct {
    uint8_t state;                  ///< IpcPowerState
} IpcPayloadPowerState;

/** IPC_MSG_LOG_LINE — debug log forwarded across cores (either direction) */
typedef struct {
    uint8_t  level;                 ///< 0=DEBUG 1=INFO 2=WARN 3=ERROR
    uint8_t  core;                  ///< Originating core: 0 or 1
    uint16_t text_len;              ///< Byte length of text[] (no null terminator)
    uint8_t  text[];                ///< UTF-8 log message (flexible array)
} IpcPayloadLogLine;

/* ── GPS bridge (shared SRAM, not FIFO) ──────────────────────────────────── */

/**
 * IpcGpsBuf — double-buffer for NMEA sentences written by Core 1, read by Core 0.
 *
 * Core 1 owns the Teseo-LIV3FL on the sensor bus (i2c1, GPIO 34/35). It writes
 * complete NMEA sentences into buf[write_idx ^ 1] while Core 0 reads buf[write_idx ^ 1]
 * after the flip. No doorbell is sent — the atomic flip of write_idx is itself the
 * signal; Core 0 polls the index on its 1 Hz cadence.
 *
 * Each slot holds one NMEA sentence (max 82 bytes per NMEA spec, padded to 128).
 */
typedef struct {
    uint8_t  buf[2][128];           ///< Double buffer: [0] and [1] slots
    uint8_t  write_idx;             ///< Index Core 1 is currently writing into (0 or 1)
    uint8_t  len[2];                ///< Valid byte count in each slot
    uint8_t  _pad[1];               ///< Pads to 260 B to match shared-SRAM reservation
} IpcGpsBuf;
_Static_assert(sizeof(IpcGpsBuf) == 260, "IpcGpsBuf must be exactly 260 bytes");

/* ── Config IPC (M1.2, for M4+ LVGL settings UI) ────────────────────────── */

/**
 * IpcConfigKey — typed key discriminator for config get/set.
 *
 * Namespace: 0xCCNN where CC = category, NN = field index.
 * Categories: 0x01=Device, 0x02=LoRa, 0x03=Position, 0x04=Power,
 *             0x05=Display, 0x06=Channel, 0x07=Owner.
 *             0x10–0x1F reserved for ModuleConfig.
 *
 * Core 0 GPL adapter translates between IpcConfigKey and Meshtastic's
 * config globals in
 * `firmware/core0/.../variants/rp2350/rp2350b-mokya/ipc_config_handler.cpp`.
 *
 * Two commit paths exist:
 *   IPC_CMD_COMMIT_CONFIG (0x8B) — soft reload via service->reloadConfig(saveWhat)
 *     plus reloadOwner() when needed. No MCU reset.
 *   IPC_CMD_COMMIT_REBOOT (0x8C) — same flash + reload, then graceful reboot via
 *     RebootNotifier (P2-10 path). Required for keys Meshtastic flags as
 *     requiresReboot=true (e.g. device.role, position.gps_mode, power.*).
 * The Core 1 settings UI maintains the per-key needs_reboot table and picks
 * which command to send; Core 0 simply executes the requested commit.
 *
 * Note: IPC_CFG_DEVICE_NAME (0x0100) is an alias for IPC_CFG_OWNER_LONG_NAME
 * (0x0700) — meshtastic_Config_DeviceConfig has no name field; the
 * user-visible node name lives in `owner.long_name`/`owner.short_name`.
 * The alias is kept so the enum value stays stable; new code should use
 * the OWNER_* keys directly.
 */
typedef enum {
    /* 0x01xx — Device (config.proto: meshtastic.Config.DeviceConfig) */
    IPC_CFG_DEVICE_NAME                   = 0x0100,  ///< alias → OWNER_LONG_NAME
    IPC_CFG_DEVICE_ROLE                   = 0x0101,  ///< uint8 enum, reboot=Y
    IPC_CFG_DEVICE_REBROADCAST_MODE       = 0x0102,  ///< uint8 enum 0..5, reboot=Y
    IPC_CFG_DEVICE_NODE_INFO_BCAST_SECS   = 0x0103,  ///< uint32, reboot=N
    IPC_CFG_DEVICE_DOUBLE_TAP_BTN         = 0x0104,  ///< bool, reboot=N
    IPC_CFG_DEVICE_DISABLE_TRIPLE_CLICK   = 0x0105,  ///< bool, reboot=N
    IPC_CFG_DEVICE_TZDEF                  = 0x0106,  ///< string up to 64 B, reboot=N
    IPC_CFG_DEVICE_LED_HEARTBEAT_DISABLED = 0x0107,  ///< bool, reboot=N

    /* 0x02xx — LoRa (config.proto: meshtastic.Config.LoRaConfig) */
    IPC_CFG_LORA_REGION                = 0x0200,  ///< uint8 enum, reboot=N
    IPC_CFG_LORA_MODEM_PRESET          = 0x0201,  ///< uint8 enum, reboot=N
    IPC_CFG_LORA_TX_POWER              = 0x0202,  ///< int8 dBm, reboot=N
    IPC_CFG_LORA_HOP_LIMIT             = 0x0203,  ///< uint8 1..7, reboot=N
    IPC_CFG_LORA_CHANNEL_NUM           = 0x0204,  ///< uint8, reboot=N
    IPC_CFG_LORA_USE_PRESET            = 0x0205,  ///< bool, reboot=N
    IPC_CFG_LORA_BANDWIDTH             = 0x0206,  ///< uint32 (kHz), reboot=N
    IPC_CFG_LORA_SPREAD_FACTOR         = 0x0207,  ///< uint32 7..12, reboot=N
    IPC_CFG_LORA_CODING_RATE           = 0x0208,  ///< uint32 5..8, reboot=N
    IPC_CFG_LORA_TX_ENABLED            = 0x0209,  ///< bool, reboot=N
    IPC_CFG_LORA_OVERRIDE_DUTY_CYCLE   = 0x020A,  ///< bool, reboot=N
    IPC_CFG_LORA_SX126X_RX_BOOSTED_GAIN= 0x020B,  ///< bool, reboot=N
    IPC_CFG_LORA_FEM_LNA_MODE          = 0x020C,  ///< uint8 enum 0..2, reboot=N

    /* 0x03xx — Position / GPS (config.proto: meshtastic.Config.PositionConfig) */
    IPC_CFG_GPS_MODE                          = 0x0300,  ///< uint8 enum, reboot=Y
    IPC_CFG_GPS_UPDATE_INTERVAL               = 0x0301,  ///< uint32 s, reboot=Y
    IPC_CFG_POSITION_BCAST_SECS               = 0x0302,  ///< uint32, reboot=Y
    IPC_CFG_POSITION_BCAST_SMART_ENABLED      = 0x0303,  ///< bool, reboot=Y
    IPC_CFG_POSITION_FIXED_POSITION           = 0x0304,  ///< bool flag only, reboot=Y
    IPC_CFG_POSITION_FLAGS                    = 0x0305,  ///< uint32 bitmask, reboot=Y
    IPC_CFG_POSITION_BCAST_SMART_MIN_DIST     = 0x0306,  ///< uint32 m, reboot=Y
    IPC_CFG_POSITION_BCAST_SMART_MIN_INT_SECS = 0x0307,  ///< uint32 s, reboot=Y

    /* 0x04xx — Power (config.proto: meshtastic.Config.PowerConfig) */
    IPC_CFG_POWER_SAVING                  = 0x0400,  ///< bool, reboot=Y
    IPC_CFG_SHUTDOWN_AFTER_SECS           = 0x0401,  ///< uint32, reboot=Y
    IPC_CFG_POWER_SDS_SECS                = 0x0402,  ///< uint32 super-deep-sleep, reboot=Y
    IPC_CFG_POWER_LS_SECS                 = 0x0403,  ///< uint32 light-sleep, reboot=Y
    IPC_CFG_POWER_MIN_WAKE_SECS           = 0x0404,  ///< uint32, reboot=Y
    IPC_CFG_POWER_BATTERY_INA_ADDRESS     = 0x0405,  ///< uint32 (I2C addr), reboot=Y
    IPC_CFG_POWER_POWERMON_ENABLES        = 0x0406,  ///< uint32 (low 32 of u64), reboot=N

    /* 0x05xx — Display (config.proto: meshtastic.Config.DisplayConfig) */
    IPC_CFG_SCREEN_ON_SECS                  = 0x0500,  ///< uint32 (NOTE: reboot per
                                                       ///  AdminModule but legacy entry
                                                       ///  marks reboot=N — known bug,
                                                       ///  not fixed in B3-P1)
    IPC_CFG_UNITS_METRIC                    = 0x0501,  ///< bool (mapped to enum)
    IPC_CFG_DISPLAY_AUTO_CAROUSEL_SECS      = 0x0502,  ///< uint32, reboot=N
    IPC_CFG_DISPLAY_FLIP_SCREEN              = 0x0503,  ///< bool, reboot=Y
    IPC_CFG_DISPLAY_OLED                     = 0x0504,  ///< uint8 enum 0..4, reboot=Y
    IPC_CFG_DISPLAY_DISPLAYMODE              = 0x0505,  ///< uint8 enum 0..3, reboot=Y
    IPC_CFG_DISPLAY_HEADING_BOLD             = 0x0506,  ///< bool, reboot=N
    IPC_CFG_DISPLAY_WAKE_ON_TAP_OR_MOTION    = 0x0507,  ///< bool, reboot=N
    IPC_CFG_DISPLAY_COMPASS_ORIENTATION      = 0x0508,  ///< uint8 enum 0..7, reboot=N
    IPC_CFG_DISPLAY_USE_12H_CLOCK            = 0x0509,  ///< bool, reboot=N
    IPC_CFG_DISPLAY_USE_LONG_NODE_NAME       = 0x050A,  ///< bool, reboot=N
    IPC_CFG_DISPLAY_ENABLE_MESSAGE_BUBBLES   = 0x050B,  ///< bool, reboot=N

    /* 0x06xx — Channel (channel.proto: meshtastic.ChannelSettings).
     * IpcPayloadGetConfig.channel_index addresses 0..7 (B3-P3 enables
     * non-primary; B3-P2 still primary-only via index 0). */
    IPC_CFG_CHANNEL_NAME                      = 0x0600,  ///< string max 12 B, reboot=N
    IPC_CFG_CHANNEL_PSK                       = 0x0601,  ///< bytes max 32 B, reboot=N
    IPC_CFG_CHANNEL_MODULE_POSITION_PRECISION = 0x0602,  ///< uint32 (bits), reboot=N
    IPC_CFG_CHANNEL_MODULE_IS_MUTED           = 0x0603,  ///< bool, reboot=N

    /* 0x07xx — Owner (mesh.proto: meshtastic.User) */
    IPC_CFG_OWNER_LONG_NAME     = 0x0700,  ///< string, max 40 B, reboot=N
    IPC_CFG_OWNER_SHORT_NAME    = 0x0701,  ///< string, max 5 B, reboot=N
    IPC_CFG_OWNER_IS_LICENSED   = 0x0702,  ///< bool (HAM mode), reboot=N
    IPC_CFG_OWNER_PUBLIC_KEY    = 0x0703,  ///< bytes max 32 B, RO

    /* 0x08xx — Security (config.proto: meshtastic.Config.SecurityConfig).
     * private_key + admin_key[] are intentionally NOT exposed —
     * editing them on the device is unsafe (no confirmation flow). */
    IPC_CFG_SECURITY_PUBLIC_KEY            = 0x0800,  ///< bytes max 32 B, RO
    IPC_CFG_SECURITY_IS_MANAGED            = 0x0801,  ///< bool, reboot=N
    IPC_CFG_SECURITY_SERIAL_ENABLED        = 0x0802,  ///< bool, reboot=Y
    IPC_CFG_SECURITY_DEBUG_LOG_API_ENABLED = 0x0803,  ///< bool, reboot=Y
    IPC_CFG_SECURITY_ADMIN_CHANNEL_ENABLED = 0x0804,  ///< bool, reboot=N

    /* ── 0x10xx — ModuleConfig.Telemetry (module_config.proto:625) ──────
     * AdminModule.cpp:933 handleSetModuleConfig defaults shouldReboot=true
     * but routes through saveChanges(SEGMENT_MODULECONFIG, ...).  Our IPC
     * soft-reload calls service->reloadConfig(SEGMENT_MODULECONFIG) which
     * fires the configChanged observer; TelemetryModule re-reads its
     * config from there so reboot is not required. */
    IPC_CFG_TELEM_DEVICE_UPDATE_INTERVAL    = 0x1000,  ///< uint32 s
    IPC_CFG_TELEM_ENV_UPDATE_INTERVAL       = 0x1001,  ///< uint32 s
    IPC_CFG_TELEM_ENV_MEASUREMENT_ENABLED   = 0x1002,  ///< bool
    IPC_CFG_TELEM_ENV_SCREEN_ENABLED        = 0x1003,  ///< bool
    IPC_CFG_TELEM_ENV_DISPLAY_FAHRENHEIT    = 0x1004,  ///< bool
    IPC_CFG_TELEM_POWER_MEASUREMENT_ENABLED = 0x1005,  ///< bool
    IPC_CFG_TELEM_POWER_UPDATE_INTERVAL     = 0x1006,  ///< uint32 s
    IPC_CFG_TELEM_POWER_SCREEN_ENABLED      = 0x1007,  ///< bool
    IPC_CFG_TELEM_DEVICE_TELEM_ENABLED      = 0x1008,  ///< bool

    /* ── 0x11xx — ModuleConfig.NeighborInfo (module_config.proto:130) ─── */
    IPC_CFG_NEIGHBOR_ENABLED            = 0x1100,  ///< bool
    IPC_CFG_NEIGHBOR_UPDATE_INTERVAL    = 0x1101,  ///< uint32 s, min 14400
    IPC_CFG_NEIGHBOR_TRANSMIT_OVER_LORA = 0x1102,  ///< bool

    /* ── 0x12xx — ModuleConfig.RangeTest (module_config.proto:598) ─────
     * Fields 3 (save) and 4 (clear_on_reboot) are ESP32-only; not
     * exposed. */
    IPC_CFG_RANGETEST_ENABLED = 0x1200,  ///< bool
    IPC_CFG_RANGETEST_SENDER  = 0x1201,  ///< uint32 (0 = receiver-only)

    /* ── 0x13xx — ModuleConfig.DetectionSensor (module_config.proto:152) ─
     * Field 4 (send_bell) and 6 (monitor_pin) intentionally not
     * exposed: send_bell is a niche bell-character flag for ext.
     * notification, monitor_pin would let an unprivileged settings UI
     * point the trigger at any GPIO (footgun without a board-level
     * allowlist). */
    IPC_CFG_DETECT_ENABLED          = 0x1300,  ///< bool
    IPC_CFG_DETECT_MIN_BCAST_SECS   = 0x1301,  ///< uint32 s
    IPC_CFG_DETECT_STATE_BCAST_SECS = 0x1302,  ///< uint32 s (0 = trigger-only)
    IPC_CFG_DETECT_NAME             = 0x1303,  ///< string max 19 B
    IPC_CFG_DETECT_TRIGGER_TYPE     = 0x1304,  ///< uint8 enum 0..5
    IPC_CFG_DETECT_USE_PULLUP       = 0x1305,  ///< bool

    /* ── 0x14xx — ModuleConfig.CannedMessage (module_config.proto:715) ──
     * Rotary-encoder fields skipped — MokyaLora has a 36-key keypad,
     * no rotary I/O. allow_input_source + enabled are deprecated. */
    IPC_CFG_CANNED_UPDOWN1_ENABLED = 0x1400,  ///< bool (field 8)
    IPC_CFG_CANNED_SEND_BELL       = 0x1401,  ///< bool (field 11)

    /* ── 0x15xx — ModuleConfig.AmbientLighting (module_config.proto:823) ──
     * current/red/green/blue stored as uint8 in nanopb but the proto
     * declares uint32 (clamped 0..255 in our IPC handler). */
    IPC_CFG_AMBIENT_LED_STATE = 0x1500,  ///< bool
    IPC_CFG_AMBIENT_CURRENT   = 0x1501,  ///< uint8 0..255
    IPC_CFG_AMBIENT_RED       = 0x1502,  ///< uint8 0..255
    IPC_CFG_AMBIENT_GREEN     = 0x1503,  ///< uint8 0..255
    IPC_CFG_AMBIENT_BLUE      = 0x1504,  ///< uint8 0..255

    /* ── 0x16xx — ModuleConfig.Paxcounter (module_config.proto:276) ─────
     * wifi_threshold + ble_threshold (int32 RSSI) skipped — defaulted
     * at -80 server-side, no useful UI affordance. */
    IPC_CFG_PAX_ENABLED         = 0x1600,  ///< bool
    IPC_CFG_PAX_UPDATE_INTERVAL = 0x1601,  ///< uint32 s

    /* ── 0x17xx — ModuleConfig.StoreForward (module_config.proto:563) ─── *
     * S-7.4. All 6 fields exposed; reboot=N (TelemetryModule analog —
     * service->reloadConfig(SEGMENT_MODULECONFIG) re-reads). */
    IPC_CFG_SF_ENABLED               = 0x1700,  ///< bool
    IPC_CFG_SF_HEARTBEAT             = 0x1701,  ///< bool
    IPC_CFG_SF_RECORDS               = 0x1702,  ///< uint32
    IPC_CFG_SF_HISTORY_RETURN_MAX    = 0x1703,  ///< uint32
    IPC_CFG_SF_HISTORY_RETURN_WINDOW = 0x1704,  ///< uint32 s
    IPC_CFG_SF_IS_SERVER             = 0x1705,  ///< bool

    /* ── 0x18xx — ModuleConfig.Serial (module_config.proto:379) ───────── *
     * S-7.9. RX/TX GPIO + baud + mode + flags. baud uses Serial_Baud
     * enum (0..15), mode uses Serial_Mode (0..10). reboot=Y on the
     * Meshtastic side (port re-init), but the IPC commit goes through
     * the standard reloadConfig path. */
    IPC_CFG_SERIAL_ENABLED          = 0x1800,  ///< bool
    IPC_CFG_SERIAL_ECHO             = 0x1801,  ///< bool
    IPC_CFG_SERIAL_RXD              = 0x1802,  ///< uint32 (GPIO)
    IPC_CFG_SERIAL_TXD              = 0x1803,  ///< uint32 (GPIO)
    IPC_CFG_SERIAL_BAUD             = 0x1804,  ///< uint8 enum 0..15
    IPC_CFG_SERIAL_TIMEOUT          = 0x1805,  ///< uint32 s
    IPC_CFG_SERIAL_MODE             = 0x1806,  ///< uint8 enum 0..10
    IPC_CFG_SERIAL_OVERRIDE_CONSOLE = 0x1807,  ///< bool

    /* ── 0x19xx — ModuleConfig.ExternalNotification (proto:472) ─────────
     * S-7.2. 15 fields; all bool / uint32. reboot=N. */
    IPC_CFG_EXTNOT_ENABLED              = 0x1900,  ///< bool
    IPC_CFG_EXTNOT_OUTPUT_MS            = 0x1901,  ///< uint32 ms
    IPC_CFG_EXTNOT_OUTPUT               = 0x1902,  ///< uint32 (GPIO)
    IPC_CFG_EXTNOT_OUTPUT_VIBRA         = 0x1903,  ///< uint32 (GPIO)
    IPC_CFG_EXTNOT_OUTPUT_BUZZER        = 0x1904,  ///< uint32 (GPIO)
    IPC_CFG_EXTNOT_ACTIVE               = 0x1905,  ///< bool (active-high vs low)
    IPC_CFG_EXTNOT_ALERT_MESSAGE        = 0x1906,  ///< bool
    IPC_CFG_EXTNOT_ALERT_MESSAGE_VIBRA  = 0x1907,  ///< bool
    IPC_CFG_EXTNOT_ALERT_MESSAGE_BUZZER = 0x1908,  ///< bool
    IPC_CFG_EXTNOT_ALERT_BELL           = 0x1909,  ///< bool
    IPC_CFG_EXTNOT_ALERT_BELL_VIBRA     = 0x190A,  ///< bool
    IPC_CFG_EXTNOT_ALERT_BELL_BUZZER    = 0x190B,  ///< bool
    IPC_CFG_EXTNOT_USE_PWM              = 0x190C,  ///< bool
    IPC_CFG_EXTNOT_NAG_TIMEOUT          = 0x190D,  ///< uint32 s
    IPC_CFG_EXTNOT_USE_I2S_AS_BUZZER    = 0x190E,  ///< bool

    /* ── 0x1Axx — ModuleConfig.RemoteHardware (proto:110) ─────────────── *
     * S-7.10. v1 covers `enabled` and `allow_undefined_pin_access`
     * only.  `available_pins` is a repeated nested message
     * (RemoteHardwarePin{name,gpio,type}); editing it needs a list
     * editor that's tracked separately. */
    IPC_CFG_RHW_ENABLED                    = 0x1A00,  ///< bool
    IPC_CFG_RHW_ALLOW_UNDEFINED_PIN_ACCESS = 0x1A01,  ///< bool
} IpcConfigKey;

/** IPC_CMD_GET_CONFIG — request a config value by key.
 *
 * channel_index is meaningful only for 0x06xx Channel keys (B3-P3).
 * For all other keys it must be 0. Core 0 decoders accept the legacy
 * 2-byte payload (no channel_index) and treat it as channel_index=0
 * for backward compatibility with B2-era SWD test scripts. */
typedef struct {
    uint16_t key;                ///< IpcConfigKey
    uint8_t  channel_index;      ///< 0 for non-channel keys
    uint8_t  _pad;
} IpcPayloadGetConfig;           /* 4 B */

/** IPC_MSG_CONFIG_VALUE / IPC_CMD_SET_CONFIG — config value envelope.
 *
 * Same channel_index semantics as IpcPayloadGetConfig. The legacy
 * 4-byte header (no channel_index, no _pad) is also accepted on
 * decode. New code uses the 8-byte header. */
typedef struct {
    uint16_t key;                ///< IpcConfigKey
    uint16_t value_len;          ///< Byte length of value[]
    uint8_t  channel_index;      ///< 0 for non-channel keys
    uint8_t  _pad[3];
    uint8_t  value[];            ///< Type-dependent: uint8/int8/uint32/string/bytes
} IpcPayloadConfigValue;         /* 8 B + variable */

/** IPC_MSG_CONFIG_RESULT — set/commit acknowledgement */
typedef struct {
    uint16_t key;                ///< Which key this result is for
    uint8_t  result;             ///< 0=OK, 1=UNKNOWN_KEY, 2=INVALID_VALUE, 3=BUSY
} IpcPayloadConfigResult;        /* 3 B */

#ifdef __cplusplus
}
#endif
