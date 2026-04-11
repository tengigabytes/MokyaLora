# MCU GPIO Allocation

**MCU:** Raspberry Pi RP2350B (QFN-80)
**Logic voltage:** 1.8 V
**Revision:** Rev 5.2

---

## GPIO Pin Map

| GPIO | Net Name     | Function / Description                          | Peripheral  |
|------|--------------|-------------------------------------------------|-------------|
| 0    | PSRAM_nCS    | PSRAM Chip Select                               | QMI CS1n    |
| 1    | USB_VBUS_DET | USB VBUS Voltage Detection                      | SIO / ADC   |
| 2    | DBG_TX       | Debug UART TX                                   | UART        |
| 3    | DBG_RX       | Debug UART RX                                   | UART        |
| 4    | MIC_CLK      | PDM Microphone Clock                            | PIO         |
| 5    | MIC_DATA     | PDM Microphone Data                             | PIO         |
| 6    | PWR_SDA      | I2C1 SDA — Power + Backlight bus                | I2C1        |
| 7    | PWR_SCL      | I2C1 SCL — Power + Backlight bus                | I2C1        |
| 8    | PWR_INT      | Power Management Interrupt (open-drain, 1.8 V pull-up) | SIO  |
| 9    | MTR_PWM      | Vibration Motor PWM (drives low-side MOSFET)    | PWM4_B      |
| 10   | TFT_nCS      | LCD Chip Select                                 | SIO         |
| 11   | TFT_DCX      | LCD Data / Command Select                       | SIO         |
| 12   | TFT_nWR      | LCD Write Strobe                                | SIO         |
| 13   | TFT_D8       | LCD Data Bit 8                                  | PIO         |
| 14   | TFT_D9       | LCD Data Bit 9                                  | PIO         |
| 15   | TFT_D10      | LCD Data Bit 10                                 | PIO         |
| 16   | TFT_D11      | LCD Data Bit 11                                 | PIO         |
| 17   | TFT_D12      | LCD Data Bit 12                                 | PIO         |
| 18   | TFT_D13      | LCD Data Bit 13                                 | PIO         |
| 19   | TFT_D14      | LCD Data Bit 14                                 | PIO         |
| 20   | TFT_D15      | LCD Data Bit 15                                 | PIO         |
| 21   | TFT_nRST     | LCD Reset                                       | SIO         |
| 22   | TFT_TE       | LCD Tearing Effect                              | SIO         |
| 23   | LORA_nRST    | SX1262 Reset                                    | SIO         |
| 24   | LORA_MISO    | SX1262 SPI MISO                                 | SPI1        |
| 25   | LORA_nCS     | SX1262 SPI Chip Select                          | SPI1        |
| 26   | LORA_SCK     | SX1262 SPI Clock                                | SPI1        |
| 27   | LORA_MOSI    | SX1262 SPI MOSI                                 | SPI1        |
| 28   | LORA_BUSY    | SX1262 Busy Status                              | SIO         |
| 29   | LORA_DIO1    | SX1262 Interrupt / Wakeup Source                | SIO         |
| 30   | AMP_BCLK     | Audio I2S Bit Clock                             | PIO         |
| 31   | AMP_FSR      | Audio I2S Frame Sync                            | PIO         |
| 32   | AMP_DAC      | Audio I2S Data Out                              | PIO         |
| 33   | PWR_BTN      | Power / Wakeup Button — wakeup source from DORMANT | SIO      |
| 34   | IMU_SDA      | I2C1 SDA — Sensor bus (SDK: `i2c1`)            | I2C1        |
| 35   | IMU_SCL      | I2C1 SCL — Sensor bus (SDK: `i2c1`)            | I2C1        |
| 36   | KEY_C0       | Keypad Column 0                                 | PIO         |
| 37   | KEY_C1       | Keypad Column 1                                 | PIO         |
| 38   | KEY_C2       | Keypad Column 2                                 | PIO         |
| 39   | KEY_C3       | Keypad Column 3                                 | PIO         |
| 40   | KEY_C4       | Keypad Column 4                                 | PIO         |
| 41   | KEY_C5       | Keypad Column 5                                 | PIO         |
| 42   | KEY_R0       | Keypad Row 0                                    | PIO         |
| 43   | KEY_R1       | Keypad Row 1                                    | PIO         |
| 44   | KEY_R2       | Keypad Row 2                                    | PIO         |
| 45   | KEY_R3       | Keypad Row 3                                    | PIO         |
| 46   | KEY_R4       | Keypad Row 4                                    | PIO         |
| 47   | KEY_R5       | Keypad Row 5                                    | PIO         |

### QSPI Dedicated Pins (Flash & PSRAM)

`QSPI_SS`, `QSPI_SCLK`, `QSPI_SD0`–`QSPI_SD3` — connected to W25Q128JW Flash and APS6404L PSRAM.

---

## I2C Bus Allocation

### Sensor bus — GPIO 34 / 35 (`i2c1` in Pico SDK)

> **Note:** Despite being labelled "I2C0" in schematic net names and earlier design docs,
> GPIO 34 / 35 map to the **`i2c1`** peripheral in the RP2350 SDK
> (I2C1_SDA / I2C1_SCL — pattern: I2C1_SDA on GPIO 2, 6, 10 … 34, 38).
> Use `i2c1` in firmware. The KiCad net names (`IMU_SDA` / `IMU_SCL`) are unaffected.

| Device        | Part           | 7-bit Address | Note                                    |
|---------------|----------------|---------------|-----------------------------------------|
| IMU           | LSM6DSV16X     | 0x6A          | SA0 tied to GND (default 0x6B conflicts with charger) |
| Magnetometer  | LIS2MDL        | 0x1E          | Fixed address                           |
| Barometer     | LPS22HH        | 0x5D          | SA0 tied to 3.3 V (Rev A confirmed; design docs previously stated 0x5C / SA0=GND) |
| GPS           | Teseo-LIV3FL   | 0x3A          | Fixed address. **I2C only** — `nRST` not routed to MCU; no GPIO enable. Software reset via `$PSTMSRR` / `$PSTMCOLDSTART`. |

### Power + Backlight bus — GPIO 6 / 7 (`i2c1` in Pico SDK)

> **Note:** GPIO 6 / 7 also map to **`i2c1`** (I2C1_SDA / I2C1_SCL).
> Both buses share the same peripheral; they cannot be active simultaneously.
> Bring-up firmware switches between them via `i2c_deinit` / `i2c_init` with different GPIO pairs.

| Device        | Part           | 7-bit Address | Note                                    |
|---------------|----------------|---------------|-----------------------------------------|
| Charger       | BQ25622        | 0x6B          | Fixed address                           |
| Fuel Gauge    | BQ27441-G1A    | 0x55          | Fixed address                           |
| LED Driver    | LM27965        | 0x36          | Controls LCD BL (Bank A) + KB BL (Bank B); nets BKLT_SCL / BKLT_SDA |

**Pull-up:** Both buses require 2.2 kΩ–4.7 kΩ to 1.8 V on SDA and SCL.

---

## Design Notes

1. All GPIO are 1.8 V. Do **not** drive LEDs or high-Vth MOSFETs directly. Use SSM3K56ACT (low-Vth) as the switching element.
2. LCD data bus (GPIO 13–20) is driven by a dedicated PIO state machine for 8-bit parallel 8080 write cycles.
3. Keypad scan uses PIO + DMA: PIO drives columns, samples rows, DMA writes state to RAM — zero CPU overhead.
4. LORA_DIO1 (GPIO 29) doubles as a DORMANT wakeup source for background LoRa Rx.
5. **I2C peripheral naming:** Both I2C buses map to the `i2c1` SDK peripheral. GPIO 6/7 and GPIO 34/35 are both I2C1_SDA/SCL pin options on RP2350. Design docs use the logical labels "Sensor bus" (GPIO 34/35) and "Power bus" (GPIO 6/7); firmware must use `i2c1` for both. They cannot be active simultaneously — switch by calling `i2c_deinit` / `i2c_init` with different GPIO pairs.
