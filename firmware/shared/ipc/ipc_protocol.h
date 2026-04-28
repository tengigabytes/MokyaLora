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
    /* 0x01xx — Device */
    IPC_CFG_DEVICE_NAME         = 0x0100,  ///< string, max 40 B
    IPC_CFG_DEVICE_ROLE         = 0x0101,  ///< uint8 (CLIENT, ROUTER, etc.)

    /* 0x02xx — LoRa */
    IPC_CFG_LORA_REGION         = 0x0200,  ///< uint8
    IPC_CFG_LORA_MODEM_PRESET   = 0x0201,  ///< uint8 (LONG_FAST, SHORT_TURBO, etc.)
    IPC_CFG_LORA_TX_POWER       = 0x0202,  ///< int8 (dBm)
    IPC_CFG_LORA_HOP_LIMIT      = 0x0203,  ///< uint8 (1–7)
    IPC_CFG_LORA_CHANNEL_NUM    = 0x0204,  ///< uint8

    /* 0x03xx — Position / GPS */
    IPC_CFG_GPS_MODE            = 0x0300,  ///< uint8 (DISABLED/ENABLED/NOT_PRESENT)
    IPC_CFG_GPS_UPDATE_INTERVAL = 0x0301,  ///< uint32 (seconds)
    IPC_CFG_POSITION_BCAST_SECS = 0x0302,  ///< uint32

    /* 0x04xx — Power */
    IPC_CFG_POWER_SAVING        = 0x0400,  ///< uint8 (bool)
    IPC_CFG_SHUTDOWN_AFTER_SECS = 0x0401,  ///< uint32

    /* 0x05xx — Display */
    IPC_CFG_SCREEN_ON_SECS      = 0x0500,  ///< uint32 (0 = default 60 s)
    IPC_CFG_UNITS_METRIC        = 0x0501,  ///< uint8 (bool)

    /* 0x06xx — Channel */
    IPC_CFG_CHANNEL_NAME        = 0x0600,  ///< string, max 12 B
    IPC_CFG_CHANNEL_PSK         = 0x0601,  ///< bytes, max 32 B

    /* 0x07xx — Owner */
    IPC_CFG_OWNER_LONG_NAME     = 0x0700,  ///< string, max 40 B
    IPC_CFG_OWNER_SHORT_NAME    = 0x0701,  ///< string, max 5 B
} IpcConfigKey;

/** IPC_CMD_GET_CONFIG — request a config value by key */
typedef struct {
    uint16_t key;                ///< IpcConfigKey
} IpcPayloadGetConfig;           /* 2 B */

/** IPC_MSG_CONFIG_VALUE / IPC_CMD_SET_CONFIG — config value envelope */
typedef struct {
    uint16_t key;                ///< IpcConfigKey
    uint16_t value_len;          ///< Byte length of value[]
    uint8_t  value[];            ///< Type-dependent: uint8/int8/uint32/string/bytes
} IpcPayloadConfigValue;         /* 4 B + variable */

/** IPC_MSG_CONFIG_RESULT — set/commit acknowledgement */
typedef struct {
    uint16_t key;                ///< Which key this result is for
    uint8_t  result;             ///< 0=OK, 1=UNKNOWN_KEY, 2=INVALID_VALUE, 3=BUSY
} IpcPayloadConfigResult;        /* 3 B */

#ifdef __cplusplus
}
#endif
