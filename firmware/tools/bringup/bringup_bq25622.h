#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// BQ25622 charger (Bus B, 0x6B)
// ---------------------------------------------------------------------------
#define BQ25622_ADDR  0x6B

// BQ25622 register map (SLUSEG2D)
// Charge/voltage/current limit registers: 16-bit little-endian register pairs.
// Each logical register Xh occupies two I2C byte addresses: lo=Xh, hi=(X+1)h.
// ICHG  field: 80 mA/step, range 80-3520 mA (0x01-0x2C), POR 1040 mA (0x0D)
// VREG  field: 10 mV/step, range 3500-4800 mV (0x15E-0x1E0), POR 4200 mV (0x1A4)
// IINDPM field: 20 mA/step, range 100-3200 mA (0x05-0xA0),  POR 3200 mA (0xA0)
#define REG_ICHG_LO     0x02  // ICHG[1:0] in bits[7:6]; bits[5:0] reserved
#define REG_ICHG_HI     0x03  // ICHG[5:2] in bits[3:0]; bits[7:4] reserved
#define REG_VREG_LO     0x04  // VREG[4:0] in bits[7:3]; bits[2:0] reserved
#define REG_VREG_HI     0x05  // VREG[8:5] in bits[3:0]; bits[7:4] reserved
#define REG_IINDPM_LO   0x06  // IINDPM[3:0] in bits[7:4]; bits[3:0] reserved
#define REG_IINDPM_HI   0x07  // IINDPM[7:4] in bits[3:0]; bits[7:4] reserved
// BQ25622 ADC control (SLUSEG2D S8.6.2.28-29) — single-byte registers
// REG0x26: ADC_EN[7] ADC_RATE[6] ADC_SAMPLE[5:4] ADC_AVG[3] ADC_AVG_INIT[2]
//   0x80 = ADC_EN=1, continuous, 12-bit (ADC_SAMPLE=00 = 12-bit)
// REG0x27: per-channel disable bits (POR=0x00, all channels enabled)
#define REG_ADC_CTRL     0x26
#define REG_ADC_FUNC_DIS 0x27

// BQ25622 ADC registers (SLUSEG2D S8.6.2.30-35)
// 16-bit little-endian; raw16 = (hi<<8)|lo; decode per field:
//   IBUS/IBAT : 2s-complement -> (int16_t)raw16 >> 1 (*2mA) / >>2 (*4mA)
//   VBUS/VPMID: unsigned bits[14:2] -> (raw16>>2)&0x1FFF (*3.97mV)
//   VBAT/VSYS : unsigned bits[12:1] -> (raw16>>1)&0x0FFF (*1.99mV)
#define REG_IBUS_ADC_LO  0x28
#define REG_IBUS_ADC_HI  0x29
#define REG_IBAT_ADC_LO  0x2A
#define REG_IBAT_ADC_HI  0x2B
#define REG_VBUS_ADC_LO  0x2C
#define REG_VBUS_ADC_HI  0x2D
#define REG_VPMID_ADC_LO 0x2E
#define REG_VPMID_ADC_HI 0x2F
#define REG_VBAT_ADC_LO  0x30
#define REG_VBAT_ADC_HI  0x31
#define REG_VSYS_ADC_LO  0x32
#define REG_VSYS_ADC_HI  0x33
#define REG_CHARGE_CTRL0    0x14
#define REG_CHARGER_CTRL1   0x16  // EN_AUTO_IBATDIS[7] FORCE_IBATDIS[6] EN_CHG[5] EN_HIZ[4] WD_RST[2] WATCHDOG[1:0]
#define REG_CHARGER_CTRL2   0x17
#define REG_CHARGER_CTRL3   0x18  // EN_OTG[6] BATFET_DLY[2] BATFET_CTRL[1:0]
#define REG_CHARGER_CTRL4   0x19
#define REG_NTC_CTRL0       0x1A  // TS_IGNORE[7]
#define REG_STATUS0         0x1D  // VSYS_STAT[4] WD_STAT[0]
#define REG_STATUS1         0x1E  // CHG_STAT[4:3] VBUS_STAT[2:0]
#define REG_FAULT_STATUS    0x1F  // BAT_FAULT_STAT[6] TS_STAT[2:0]
#define REG_CHG_FLAG0       0x20
#define REG_CHG_FLAG1       0x21
#define REG_FAULT_FLAG0     0x22
#define REG_PART_INFO       0x38  // PN[5:3] DEV_REV[2:0]  0x02 = BQ25622

// Public API
int  bq25622_reg_read(uint8_t reg, uint8_t *val);
int  bq25622_reg_write(uint8_t reg, uint8_t val);
void bq25622_print_status(void);
void bq25622_enable_charge(void);
void bq25622_disable_charge(void);
void bq25622_read_adc(void);
void bq25622_dump_regs(void);
void charger_diag(void);
