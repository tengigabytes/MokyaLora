#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// SX1262 opcode constants (DS.SX1261-2 S13)
// ---------------------------------------------------------------------------
#define SX1262_OP_GET_STATUS            0xC0u
#define SX1262_OP_READ_REGISTER         0x1Du
#define SX1262_REG_SYNC_WORD_MSB        0x0740u  // default 0x14 (private network)
#define SX1262_REG_SYNC_WORD_LSB        0x0741u  // default 0x24
#define SX1262_OP_SET_STANDBY           0x80u
#define SX1262_OP_SET_PACKET_TYPE       0x8Au
#define SX1262_OP_SET_RF_FREQUENCY      0x86u
#define SX1262_OP_CALIBRATE             0x89u   // CalibParam bitmask (bits 0-6)
#define SX1262_OP_CALIBRATE_IMAGE       0x98u   // 2-byte freq band pair
#define SX1262_OP_SET_MODULATION_PARAMS 0x8Bu
#define SX1262_OP_SET_PACKET_PARAMS     0x8Cu
#define SX1262_OP_SET_BUFFER_BASE_ADDR  0x8Fu
#define SX1262_OP_SET_DIO2_RF_SWITCH    0x9Du
#define SX1262_OP_SET_RX                0x82u
#define SX1262_OP_GET_IRQ_STATUS        0x12u
#define SX1262_OP_CLEAR_IRQ_STATUS      0x02u
#define SX1262_OP_GET_RX_BUFFER_STATUS  0x13u
#define SX1262_OP_READ_BUFFER           0x1Eu
#define SX1262_OP_GET_RSSI_INST         0x15u   // GetRssiInst: NOP + RSSI byte (-value/2 dBm)

// Public API
void lora_test(void);
void lora_rx(uint32_t freq_hz, uint8_t sf, uint8_t bw_code,
             uint8_t cr_code, uint32_t timeout_s);
void lora_dump(void);
void lora_tx(void);
