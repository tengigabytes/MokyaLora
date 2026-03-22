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
    /* Core 0 → Core 1 (notifications) */
    IPC_MSG_RX_TEXT        = 0x01,  ///< Incoming text message received
    IPC_MSG_NODE_UPDATE    = 0x02,  ///< Node list entry added or updated
    IPC_MSG_DEVICE_STATUS  = 0x03,  ///< Periodic status update (battery, GPS, RSSI)
    IPC_MSG_TX_ACK         = 0x04,  ///< Tx acknowledgement (sent / delivered / failed)
    IPC_MSG_CHANNEL_UPDATE = 0x05,  ///< Channel configuration changed

    /* Core 1 → Core 0 (commands) */
    IPC_CMD_SEND_TEXT      = 0x81,  ///< Request to send a text message
    IPC_CMD_SET_CHANNEL    = 0x82,  ///< Set active channel
    IPC_CMD_SET_TX_POWER   = 0x83,  ///< Set LoRa TX power (dBm)
    IPC_CMD_REQUEST_STATUS = 0x84,  ///< Request an immediate IPC_MSG_DEVICE_STATUS
    IPC_CMD_SET_NODE_ALIAS = 0x85,  ///< Assign a user-defined alias to a node ID
    IPC_CMD_POWER_STATE    = 0x86,  ///< Notify Core 0 of a power state transition
    IPC_CMD_REBOOT         = 0x87,  ///< Request system reboot
    IPC_CMD_FACTORY_RESET  = 0x88,  ///< Request factory reset (wipes persistent config)

    /* Bidirectional (debug) */
    IPC_MSG_LOG_LINE       = 0xF0,  ///< Debug log line (either core → other core)

    /* Boot handshake */
    IPC_BOOT_READY         = 0xFF,  ///< Sent by each core when its init is complete
} IpcMsgId;

/* ── Common header (every message starts with this) ───────────────────────── */

typedef struct {
    uint8_t  msg_id;        ///< IpcMsgId
    uint8_t  seq;           ///< Rolling sequence number (for duplicate detection)
    uint16_t payload_len;   ///< Byte length of the payload that follows
} IpcMsgHeader;             /* 4 bytes */

/* ── Payload structures ───────────────────────────────────────────────────── */

/** IPC_MSG_RX_TEXT / IPC_CMD_SEND_TEXT */
typedef struct {
    uint32_t from_node_id;          ///< Sender node ID (0 = self for CMD_SEND_TEXT)
    uint32_t to_node_id;            ///< Destination node ID (0xFFFFFFFF = broadcast)
    uint8_t  channel_index;         ///< Channel index (0 = primary)
    uint8_t  want_ack;              ///< 1 = request delivery ACK
    uint16_t text_len;              ///< Byte length of text[] (UTF-8, no null terminator)
    uint8_t  text[];                ///< Flexible array — variable length UTF-8 payload
} IpcPayloadText;

/** IPC_MSG_NODE_UPDATE */
typedef struct {
    uint32_t node_id;               ///< Meshtastic node ID
    int16_t  rssi;                  ///< Last heard RSSI (dBm), INT16_MIN if unknown
    int8_t   snr_x4;                ///< SNR × 4 (fixed-point), INT8_MIN if unknown
    uint8_t  hops_away;             ///< Hop count, 0xFF if unknown
    int32_t  lat_e7;                ///< Latitude  × 1e7 (degrees), INT32_MIN if no GPS
    int32_t  lon_e7;                ///< Longitude × 1e7 (degrees), INT32_MIN if no GPS
    uint16_t battery_mv;            ///< Battery voltage in mV, 0 if unknown
    uint8_t  alias_len;             ///< Byte length of alias[] (0 = no alias set)
    uint8_t  alias[];               ///< UTF-8 alias string (variable length)
} IpcPayloadNodeUpdate;

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

/** IPC_MSG_TX_ACK */
typedef struct {
    uint8_t  seq;                   ///< Sequence number of the original CMD_SEND_TEXT
    uint8_t  result;                ///< 0 = sending, 1 = delivered, 2 = failed
} IpcPayloadTxAck;

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
 * Core 1 owns the Teseo-LIV3FL over I2C0. It writes complete NMEA sentences into
 * buf[write_idx ^ 0] while Core 0 reads from buf[read_idx]. Ownership swaps are
 * signalled via IPC_CMD_REQUEST_STATUS or a dedicated FIFO word (upper bit set).
 *
 * Each slot holds one NMEA sentence (max 82 bytes per NMEA spec, padded to 128).
 */
typedef struct {
    uint8_t  buf[2][128];           ///< Double buffer: [0] and [1] slots
    uint8_t  write_idx;             ///< Index Core 1 is currently writing into (0 or 1)
    uint8_t  len[2];                ///< Valid byte count in each slot
    uint8_t  _pad[2];
} IpcGpsBuf;                        /* 260 bytes, place in shared SRAM */

#ifdef __cplusplus
}
#endif
