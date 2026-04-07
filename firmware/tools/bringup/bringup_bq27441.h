#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// BQ27441-G1 fuel gauge (Bus B, 0x55)
// Reference: SLUSBH1C datasheet + SLUUAC9A Technical Reference Manual
// ---------------------------------------------------------------------------
#define BQ27441_ADDR         0x55

// Standard command registers — 16-bit little-endian (addr = LSB, addr+1 = MSB)
#define BQ27441_REG_CTRL     0x00  // Control() — subcommand interface
#define BQ27441_REG_TEMP     0x02  // Temperature() — 0.1 K; C = (raw*0.1) - 273.15
#define BQ27441_REG_VOLT     0x04  // Voltage() — mV
#define BQ27441_REG_FLAGS    0x06  // Flags() — status bits
#define BQ27441_REG_REMCAP   0x0C  // RemainingCapacity() — mAh (temperature-compensated)
#define BQ27441_REG_FULLCAP  0x0E  // FullChargeCapacity() — mAh (temperature-compensated)
#define BQ27441_REG_AVGCUR   0x10  // AverageCurrent() — signed mA (+charge, -discharge)
#define BQ27441_REG_SOC      0x1C  // StateOfCharge() — %
#define BQ27441_REG_SOH      0x20  // StateOfHealth() — hi byte=%, lo byte=status(0-6)

// CONTROL() sub-commands (write 3 bytes to 0x00: [0x00, subcmd_lo, subcmd_hi])
// Read result: write 0x00 pointer, read 2 bytes; wait >= 66 us between write and read
#define BQ27441_CTRL_STATUS  0x0000  // Returns CONTROL_STATUS word
#define BQ27441_CTRL_DEVTYPE 0x0001  // Returns 0x0421 for BQ27441-G1
#define BQ27441_CTRL_CLR_HIB 0x0012  // CLEAR_HIBERNATE — force exit from HIBERNATE

// Flags() bit positions (16-bit little-endian)
// Low byte (0x06): [7]=OCVTAKEN [5]=ITPOR [4]=CFGUPMODE [3]=BAT_DET [2]=SOC1 [1]=SOCF [0]=DSG
// High byte (0x07): [1]=FC [0]=CHG
#define BQ27441_FLAG_DSG        (1u << 0)   // Discharging / relaxation mode
#define BQ27441_FLAG_SOCF       (1u << 1)   // SOC final threshold (default 2%)
#define BQ27441_FLAG_SOC1       (1u << 2)   // SOC low threshold  (default 10%)
#define BQ27441_FLAG_BAT_DET    (1u << 3)   // Battery detected
#define BQ27441_FLAG_ITPOR      (1u << 5)   // RAM reset to ROM defaults; host must reconfigure
#define BQ27441_FLAG_OCVTAKEN   (1u << 7)   // OCV measurement taken in relax mode
#define BQ27441_FLAG_CHG        (1u << 8)   // Fast charge allowed
#define BQ27441_FLAG_FC         (1u << 9)   // Fully charged

// CONTROL_STATUS bit positions (16-bit little-endian)
// Low byte (0x00):  [7]=INITCOMP [6]=HIBERNATE [4]=SLEEP
// High byte (0x01): [5]=SS (sealed)
#define BQ27441_CS_SLEEP        (1u << 4)
#define BQ27441_CS_HIBERNATE    (1u << 6)
#define BQ27441_CS_INITCOMP     (1u << 7)
#define BQ27441_CS_SEALED       (1u << 13)

// Public API
int  fg_read16(uint8_t reg, uint16_t *val);
int  fg_ctrl_write(uint16_t subcmd);
int  fg_ctrl_read(uint16_t subcmd, uint16_t *result);
void bq27441_read(void);
void bq27441_dump_regs(void);
void gauge_diag(void);
