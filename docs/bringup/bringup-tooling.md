# Bringup Tooling Reference

Reference document for the MokyaLora Rev A bringup shell: build/flash scripts, source
layout, and automated test runner usage. Intended to be loaded as context at the start of
each bringup or debug session.

---

## Hardware Setup

- **Debugger:** SEGGER J-Link (supports 1.8 V VTREF — required for RP2350B)
- **Serial port:** auto-detected by USB VID `0x2E8A` (Raspberry Pi), 115200 baud, USB CDC (RP2350B USB). COM number varies per host and enumeration order — all scripts call `scripts/_mokya-port.ps1::Resolve-MokyaPort`; pass `-PortName COMxx` to override.
- **SWD connection:** SWCLK / SWDIO / GND on board test points
- **Power:** USB-C 5 V input (VSYS via BQ25622)

> Pi Debug Probe is **incompatible** — requires 3.3 V VTREF; RP2350B logic is 1.8 V.

---

## Build & Flash Scripts

All build, flash, and serial helper scripts live in `scripts/`. Run from project root.

| Script | Location | Purpose |
|--------|----------|---------|
| `configure_bringup.sh` | `scripts/` | CMake configure only (run once or after CMakeLists change) |
| `build_bringup.sh` | `scripts/` | Incremental build only (no flash) |
| `build_and_flash_bringup.sh` | `scripts/` | Full build + J-Link flash + reset |
| `flash_bringup.sh` | `scripts/` | Flash pre-built ELF only (no build step) |
| `serial_monitor.ps1` | `scripts/` | Interactive serial terminal |
| `bringup_run.ps1` | `scripts/` | Automated command runner (send commands, capture output) |
| `firmware/tools/recv_pcm_dump.py` | `firmware/tools/` | Binary PCM receiver for `mic_dump` command |

### Build & Flash

```bash
# First time: configure CMake (run from project root in Git Bash)
bash scripts/configure_bringup.sh

# Subsequent builds: incremental build only
bash scripts/build_bringup.sh

# Full build + flash via J-Link
bash scripts/build_and_flash_bringup.sh

# Flash only (ELF already built)
bash scripts/flash_bringup.sh
```

### Interactive Terminal

```powershell
# Open interactive shell (COM4, 115200)
.\scripts\serial_monitor.ps1

# Open shell and send initial command
.\scripts\serial_monitor.ps1 help
```

### Automated Command Runner (`scripts/bringup_run.ps1`)

Sends one or more commands to the bringup shell, waits for each to complete, and prints
output. Handles COM port retry, boot banner detection, and per-command timeouts.

```powershell
# Single command
.\scripts\bringup_run.ps1 psram

# Multiple commands in sequence
.\scripts\bringup_run.ps1 scan_a scan_b status lora_dump

# Build + flash first, then run commands
.\scripts\bringup_run.ps1 -Flash scan_a lora_dump psram

# Custom port
.\scripts\bringup_run.ps1 -PortName COM5 mic_dump
```

Per-command timeouts (seconds): `lora_rx` = 35, `lora_dump` = 8, `psram` = 5, others = 2.

### PCM Dump Receiver

```bash
# Record 1 s of mic audio and save to mic_dump.wav (run from project root)
python firmware/tools/recv_pcm_dump.py
```

Sends `mic_dump` to the bringup shell, receives binary int16 PCM over serial, and saves
`mic_dump.wav`. Open in Audacity: File → Import → Audio, or drag-and-drop.

---

## Bringup Shell Commands

```
imu         — LSM6DSV16X accel+gyro+temp one-shot read
baro        — LPS22HH pressure+temp one-shot read
mag         — LIS2MDL magnetometer one-shot read
gnss_info   — Teseo-LIV3FL: $PSTMGETSWVER + 300-byte NMEA stream
dump_a      — dump Bus A device registers (IMU/Mag/Baro/GNSS)
dump_b      — dump Bus B device registers (Charger/LED)
scan_a      — I2C scan Bus A (sensors, GPIO 34/35)
scan_b      — I2C scan Bus B (power, GPIO 6/7)
status      — BQ25622 charger status
led         — LED cycle (keyboard/red/green)
motor       — vibration motor breathe ×5
amp_test    — NAU8315 constant tone 5 s at 80%
amp         — NAU8315 speaker breathe tone ×5 (~444 Hz)
bee         — Xiao Mi Feng melody at 40% amp
mic         — IM69D130 PDM: capture 32768 bits, check 1-density
mic_raw     — PDM density monitor 10 s (speak to see shift)
mic_loop    — mic → speaker real-time loopback 10 s
mic_rec     — record 3 s to SRAM then play back
mic_dump    — record 1 s, send binary PCM over serial (use recv_pcm_dump.py)
lora        — SX1262 reset + GetStatus + SyncWord check
lora_rx     — SX1262 RX sniff 30 s (923.125 MHz, SF11, BW250k, AS923)
lora_dump   — SX1262 full status: errors, SyncWord, OCP, RxGain, RSSI, stats
sram        — RP2350B internal SRAM 16 KB pattern test (5 patterns)
flash       — W25Q128JW JEDEC ID + unique ID
psram       — APS6404L init + SPI/QPI write+readback probe
tft         — ST7789VI: solid fills + dynamic sub-tests + backlight fade
key         — keyboard monitor (prints key name on press; Enter to exit)
charge_on   — enable BQ25622 charging
charge_off  — disable BQ25622 charging
```

---

## Source Layout

The bringup shell was originally a single `i2c_custom_scan.c`. Split into focused units:

| File | Contents |
|------|----------|
| `bringup.h` | Shared header: all `#include`, pin `#define`, register `#define`, public declarations |
| `i2c_custom_scan.c` | Main REPL loop, keypad monitor, command dispatch |
| `bringup_power.c` | LM27965, BQ25622, motor PWM, Bus B init/deinit |
| `bringup_sensors.c` | LSM6DSV16X, LIS2MDL, LPS22HH, Teseo-LIV3FL, Bus A scan |
| `bringup_audio.c` | PIO I2S driver, NAU8315 tone/breathe/melody, PDM mic test/raw/loopback/rec/dump |
| `bringup_flash.c` | Internal SRAM test, W25Q128JW JEDEC/UID, APS6404L QMI probe |
| `bringup_lora.c` | SX1262 SPI helpers, `lora_test`, `lora_rx`, `lora_dump` |
| `bringup_tft.c` | PIO 8080 driver, ST7789VI init, colour fills, dynamic sub-tests |
| `i2s_out.pio` | PIO I2S output: 48828 Hz, 16-bit stereo, gpio_base=16 |
| `pdm_mic.pio` | PIO PDM clock + data capture: 3.125 MHz, autopush 32 bits |
| `tft_8080.pio` | PIO 8080 write: side-set nWR, out D[7:0], autopull 8-bit, clkdiv=4 |

All source files are in `firmware/tools/bringup/`.

---

## Known Firmware Bugs Fixed During Bringup

| Bug | Fix |
|-----|-----|
| `lora_dump`: GetDeviceErrors called before calibration → all-fail POR default | Moved to after `Calibrate(0x7F)` + `CalibrateImage({0xE1,0xE9})` |
| `lora_dump`: RxGain WriteRegister used wrong address 0x06B8 | Corrected to **0x08AC** (readback confirmed 0x96) |
| `lora_dump` / `lora_rx`: GetStats used 7-byte SPI transfer; `rx[7]` out-of-bounds | Corrected to **8 bytes**: opcode + status + RxOk×2 + CrcErr×2 + HdrErr×2 |
| `bq25622_print_status`: VBUS_STAT decode string wrong at index 4 | Unified into shared `bq_vbus_str[]` array |
| PDM mic PIO: designed for L-channel (SELECT=GND); actual SELECT=VDD → R-channel | PIO changed to `nop side 0` / `in pins,1 side 1`; `gpio_disable_pulls` replaces `gpio_pull_up` |
| `mic_loop`: `printf` stats caused CPU stall → PDM CLK freeze → mic lost lock | Added `#define MIC_LOOP_STATS 0` compile-time switch; disabled by default |
| `mic_loop`: PDM `get_blocking` and I2S `put_blocking` contend for CPU → timing drift | Added `mic_rec`: Phase 1 PDM-only capture, Phase 2 I2S-only playback |
| PSRAM `psram_test`: SPI probe read `psram[0]` without writing first → false 0xFFFFFFFF on cold boot | Now writes pattern first, then reads back; also adds QPI Quad I/O probe via bootrom rfmt |
| PSRAM reset sequence: tRST busy-wait was 50 µs; APS6404L spec requires 100 µs | Busy-wait loop doubled to 12500 NOPs (~100 µs at 125 MHz) |
