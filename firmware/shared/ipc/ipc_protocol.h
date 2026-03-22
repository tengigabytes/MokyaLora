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

#ifdef __cplusplus
}
#endif
