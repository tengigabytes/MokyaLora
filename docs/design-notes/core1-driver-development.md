# Core 1 Driver Development Guide

**Scope.** This document describes the constraints, init rules, and
debugging playbook for writing new peripheral drivers inside the
Core 1 `m1_bridge` image (Apache-2.0, `firmware/core1/`). Core 0
(Meshtastic, GPL-3.0) follows Arduino-Pico conventions and is out of
scope here.

It distils lessons from M1 (USB bridge), M2 (doorbell + flash
safety), M3.1/M3.2 (display + LVGL), and M3.3 Phase A (keypad). Every
rule here is a rule **because something silently broke once**. If you
are about to add a new driver, read this before writing code.

---

## 1. What Core 1 Inherits vs Does Itself

Core 0 boots the RP2350, runs the stock Pico SDK `runtime_init_*`
hooks, then calls `multicore_launch_core1_raw()` from `initVariant()`.
Core 1's crt0 then runs with several hooks deliberately skipped
(see `firmware/core1/m1_bridge/CMakeLists.txt`):

| Hook                                    | Done by        | Consequence for Core 1                                        |
|-----------------------------------------|----------------|---------------------------------------------------------------|
| `runtime_init_clocks`                   | Core 0         | `clock_get_hz(clk_*)` returns **0** on Core 1                 |
| `runtime_init_early_resets`             | Core 0         | All peripherals already out of reset                          |
| `runtime_init_post_clock_resets`        | Core 0         | IO_BANK0, PADS_BANK0, PIO0/1, DMA, etc. all out of reset      |
| `runtime_init_spin_locks_reset`         | Core 0         | `spin_lock_claim` is safe                                     |
| `runtime_init_bootrom_locking_enable`   | Core 0         | Core 1 does not enter this path                               |
| `runtime_init_usb_power_down`           | Core 0         | USB-CTRL left in whatever state Core 0 gave it; we re-init it |
| `runtime_init_install_ram_vector_table` | Core 1 (kept)  | FreeRTOS needs writable VTOR                                  |
| `runtime_init_mutex`                    | Core 1 (kept)  | C runtime mutex init                                          |
| `runtime_init_per_core_*`               | Core 1 (kept)  | NVIC priorities etc.                                          |

**Implication.** Do not call `unreset_block_wait` or `clock_configure`
from Core 1 code — you will fight Core 0 or no-op harmlessly depending
on the peripheral. Do not call `clock_get_hz()` for timing — Core 1
always sees 0. Use the system-clock constant (150 MHz) or read QMI /
PLL registers directly if you really need runtime values.

---

## 2. Pico SDK Board Header Must Match Silicon

**MokyaLora Rev A uses RP2350B (48 GPIOs).** The Pico SDK gates a
lot of behaviour on `PICO_RP2350A`:

```c
// src/rp2350/hardware_regs/include/hardware/platform_defs.h
#if PICO_RP2350A
#define NUM_BANK0_GPIOS _u(30)
#else
#define NUM_BANK0_GPIOS _u(48)
#endif
```

`check_gpio_param(gpio)` in `hardware_gpio/include/hardware/gpio.h`
is the macro every GPIO entrypoint calls:

```c
static inline void check_gpio_param(uint gpio) {
    invalid_params_if(HARDWARE_GPIO, gpio >= NUM_BANK0_GPIOS);
}
```

In a release build (`PARAM_ASSERTIONS_ENABLED_HARDWARE_GPIO=0`) this
compiles to a no-op and the SDK function **returns without doing
anything** when `gpio >= NUM_BANK0_GPIOS`. There is no error, no
log — the pad register stays at its boot-time default
(`0x00000116`: ISO=1, IE=0, PDE=1, FUNCSEL stays at `0x1F` = NULL).

The stock `pico2` / `pico2_w` board headers both define
`PICO_RP2350A 1`, so any MokyaLora build that inherits from them
silently drops every GPIO write to pins 30–47. **Display works
(GPIO 6–22, in range) but keypad fails (GPIO 36–47, out of range).**

**Fix that shipped in M3.3 Phase A:**
`firmware/core1/m1_bridge/boards/mokya_rev_a.h` defines
`PICO_RP2350A 0` plus MokyaLora-specific flash/variant fields. The
CMakeLists appends the boards/ directory to `PICO_BOARD_HEADER_DIRS`
and sets `PICO_BOARD mokya_rev_a`:

```cmake
list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/boards")
set(PICO_BOARD mokya_rev_a CACHE STRING "MokyaLora Rev A (RP2350B)" FORCE)
```

**Rule.** Never change `PICO_BOARD` to a stock value for the Core 1
image without verifying `PICO_RP2350A 0`. If you bring in another SDK
subsystem that depends on board-specific pins, extend `mokya_rev_a.h`
rather than swap boards.

---

## 3. GPIO Driver Rules

### 3.1 Use the SDK, not raw registers

```c
gpio_init(pin);
gpio_set_dir(pin, GPIO_OUT);
gpio_put(pin, 1);
gpio_pull_up(pin);
// ...
```

The SDK's `gpio_set_function(pin, GPIO_FUNC_SIO)` (called implicitly
by `gpio_init`) does three important things on RP2350:

1. Clears `ISO` in the pad register (so the pad actually connects).
2. Sets `IE=1` (input buffer on) and clears `OD`.
3. Writes `FUNCSEL` in IO_BANK0 ctrl.

If you bypass the SDK with direct pad writes, you **must** replicate
all three steps, including the ISO clear. Previous debug sessions
wrote `IE|PUE` to the pad register and forgot ISO — the pad stayed
isolated, the pin did nothing, and the symptom was identical to
"GPIO not initialised" which made diagnosis circular.

### 3.2 High GPIOs (30–47) are behind `gpio_hi_*`

SIO has separate register pairs for GPIO 0–31 (`gpio_out`, `gpio_oe`,
`gpio_in`) and 32–47 (`gpio_hi_out`, `gpio_hi_oe`, `gpio_hi_in`).
The SDK handles this internally — **if** `NUM_BANK0_GPIOS` is 48. If
it is 30 (wrong board header), every SDK call that uses the high-GPIO
path short-circuits.

SWD reference for GPIO 32–47:
- `SIO_BASE + 0x008` — `gpio_hi_in`
- `SIO_BASE + 0x014` — `gpio_hi_out`
- `SIO_BASE + 0x034` — `gpio_hi_oe`

Bit `N - 32` is the bit for GPIO `N`.

### 3.3 Register cheatsheet for SWD diagnosis

| Register                     | Address (GPIO N)            | Healthy for input with pull-up  | Healthy for output                 | Reset default (unhealthy)          |
|------------------------------|-----------------------------|---------------------------------|------------------------------------|------------------------------------|
| `PADS_BANK0_GPIO{N}`         | `0x40038004 + N*4`          | `0x5A` (IE=1 PUE=1 PDE=0 ISO=0) | `0x56` (IE=1 PUE=0 PDE=1 ISO=0) \* | `0x116` (ISO=1 IE=0 PDE=1)         |
| `IO_BANK0 GPIO{N} ctrl`      | `0x40028004 + N*8`          | `0x05` (FUNCSEL=SIO)            | `0x05`                             | `0x1F` (FUNCSEL=NULL)              |
| `SIO gpio_hi_in`             | `0xD0000008`                | bit N-32 reflects pull          | bit N-32 reflects own output       | (IE=0 so reads undefined)          |
| `SIO gpio_hi_out`            | `0xD0000014`                | any                             | what you wrote                     | 0                                  |
| `SIO gpio_hi_oe`             | `0xD0000034`                | bit clear (input)               | bit set                            | 0                                  |

\* Output pads currently keep the reset-default PDE=1; this is
intentional (the output driver dominates) but you can clear it with
`gpio_disable_pulls()` if you want the cleanest state.

Pad register bit layout (RP2350):

```
bit 8: ISO        (must be 0 to use the pad)
bit 7: OD         (output disable)
bit 6: IE         (input enable)
bits 5..4: DRIVE  (00=2mA, 01=4mA, 10=8mA, 11=12mA)
bit 3: PUE        (pull-up enable)
bit 2: PDE        (pull-down enable)
bit 1: SCHMITT
bit 0: SLEWFAST
```

### 3.4 PIO on high GPIOs — `pio_set_gpio_base(pio, 16)` is mandatory

A PIO instance can address only 32 consecutive GPIOs at a time. On
RP2350B the PIO's "GPIO base" register selects whether the SM pinctrl
fields refer to GPIOs 0–31 (default) or 16–47. Any peripheral that
lives on GPIO 32–47 (keypad columns/rows, sensor-bus pins on some
revisions) **must** call `pio_set_gpio_base(pio, 16)` **before**
`pio_sm_init`. Skip it and `pio_sm_init` returns
`PICO_ERROR_BAD_ALIGNMENT` — a silent failure in release builds where
the assertion is compiled out.

The SDK helpers `sm_config_set_out_pins()`, `sm_config_set_in_pins()`
etc. accept raw GPIO numbers (36–47) and stash the high bits in
`pio_sm_config.pinhi` when `PICO_PIO_USE_GPIO_BASE=1` (automatically
set for RP2350B via our board header). `pio_sm_init()` then validates
the config against the current `gpiobase`; mismatch → error.

Standard keypad init sequence:

```c
pio_set_gpio_base(pio0, 16);        /* required BEFORE sm init */
uint sm = pio_claim_unused_sm(pio0, true);
uint off = pio_add_program(pio0, &keypad_scan_program);
keypad_scan_program_init(pio0, sm, off, ROW_BASE, COL_BASE, clkdiv);
pio_sm_set_enabled(pio0, sm, true);
```

`pio_gpio_init(pio, pin)` itself takes the raw GPIO number — the SDK
adjusts internally.

### 3.5 PIO + DMA continuous-scan pattern

For periodic scanners (keypad, sensor polling) the canonical zero-CPU
pattern is PIO + 2 DMA channels, both in ring mode:

- **TX DMA** streams a fixed mask table from RAM into the PIO TX FIFO.
  `channel_config_set_ring(&cfg, false, N)` makes read-addr wrap at a
  2^N-byte boundary, so the table loops forever without CPU attention.
- **RX DMA** drains PIO RX FIFO into a mirror buffer in RAM.
  `channel_config_set_ring(&cfg, true, N)` wraps write-addr so the
  buffer always holds the latest `2^N` results.
- Both channels use 8-bit transfers even though the PIO FIFO is
  32-bit wide: an 8-bit DMA transfer pops one full FIFO word and keeps
  the low byte, advancing memory by 1 byte. Align the RAM buffers to
  `2^N` so ring mode doesn't partially wrap.
- Use `dma_start_channel_mask((1u << tx) | (1u << rx))` to start both
  simultaneously, then `pio_sm_set_enabled()`.
- `transfer_count = 0xFFFFFFFFu` gives ~5.8 days at our scan rate
  before the channels exhaust. Chain a pair of channels per direction
  (each chained_to the other) if uptime demand exceeds that.

The consumer task just reads the RX mirror buffer — no PIO FIFO
interaction. Single-byte DMA-concurrent reads of individual ring slots
are safe; composite reads across multiple slots may catch one slot
mid-write. For keypad that is fine because debounce absorbs the
single-row staleness. For higher-integrity snapshots (sensor triplets
that must agree), add a double-buffer with a DMA-complete IRQ to
signal "buffer A is stable".

---

## 4. FreeRTOS Rules on Core 1

### 4.1 Heap is 32 KB total — size tasks accordingly

`configTOTAL_HEAP_SIZE = 32 * 1024` (heap_4). All task stacks, TCBs,
queue control blocks, and `pvPortMalloc` allocations come out of this
one pool. Check `xFreeBytesRemaining` via SWD after boot.

Current post-boot allocation (M3.3 Phase A):

| Consumer              | Bytes  |
|-----------------------|--------|
| `usb_device_task` stack  | 4096 |
| `bridge_task` stack      | 4096 |
| `lvgl_task` stack        | 12288 |
| `keypad_probe_task` stack | 2048 |
| Idle task stack          | 2048 |
| Timer service task stack | 2048 |
| TCBs + queues            | ~1.5 KB |
| **Free**                 | ~5 KB |

**Rule.** Add a new task → drop an existing task's stack high-water
margin, or raise `configTOTAL_HEAP_SIZE` in `FreeRTOSConfig.h`. Do
not assume there is headroom.

### 4.2 Always check `xTaskCreate` return code

```c
BaseType_t rc = xTaskCreate(fn, "name", stack_words, NULL, prio, NULL);
if (rc != pdPASS) { /* heap exhausted — task does not exist */ }
```

Silent failure (`(void)rc;`) is how M3.3 Phase A cost a full debug
cycle. The task was never created; there was no symptom other than
"the driver doesn't work". Either assert, or publish `rc` to a
named SWD-readable variable while the driver is still experimental.

### 4.3 Priority starvation is the default, not the exception

Core 1 runs `configUSE_PREEMPTION=1` with time-slicing. The scheduler
always picks the **highest-priority ready task**; round-robin only
rotates within equal priority. Strictly-lower priorities run only
when every higher-priority task blocks.

Several of our tasks **never block** in their outer loop:

- `usb_device_task`: `for(;;) { tud_task(); taskYIELD(); }` — always ready.
- `bridge_task`: blocks on `xTaskNotifyWait(10 ms)` when idle, but spins busy under traffic.
- `lvgl_task`: `vTaskDelay(pdMS_TO_TICKS(next))` — yields properly.

A new task at priority `tskIDLE_PRIORITY + 1` will be starved by
`usb_device_task` at priority `tskIDLE_PRIORITY + 2`.

**Rule for I/O tasks.** Match the priority of the existing round-robin
group (currently `tskIDLE_PRIORITY + 2`). Only put work at a strictly
lower priority if it is genuinely optional and tolerant of arbitrary
delay — there is no such work in the current image.

### 4.4 SysTick override required

Core 1 skips `runtime_init_clocks`, so `configured_freq[clk_sys]`
stays 0 and the default `vPortSetupTimerInterrupt()` loads the
SysTick with `0xFFFFFF`, making tick rate collapse to ~9 Hz. Core 1's
`main_core1_bridge.c` overrides `vPortSetupTimerInterrupt()` with the
known 150 MHz constant. Do not remove this override. If you change
`configTICK_RATE_HZ`, update the override too.

---

## 5. I2C Rules

`i2c_init(i2c_inst, baud)` computes the SCL timing divisor from
`clock_get_hz(clk_peri)`. On Core 1 this returns 0 → garbage baud
→ inconsistent NACKs (P3-3 symptom).

**Pattern that works** (see `display.c::backlight_init`):

```c
i2c_init(BL_I2C, 100 * 1000);                 // brings peripheral out of reset
i2c_set_baudrate_core1(BL_I2C, 100 * 1000);   // fix up timing with known 150 MHz
gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
gpio_pull_up(PIN_SDA);
gpio_pull_up(PIN_SCL);
```

`i2c_set_baudrate_core1` is MokyaLora's replacement — it mirrors the
SDK's baud-rate logic but uses the 150 MHz constant for `clk_peri`.

---

## 6. Flash & Interrupts

Core 1 can read flash (XIP) freely. **Core 0 owns flash writes**, and
any write path on Core 0 is wrapped via `--wrap=flash_range_erase` /
`--wrap=flash_range_program` to trigger the park protocol:

1. Core 0 sets `flash_lock = REQUEST`, fires `IPC_FLASH_DOORBELL`.
2. Core 1's doorbell ISR calls `flash_park_handler()` (RAM-resident):
   - ACKs with `flash_lock = PARKED`, `__sev()` wakes Core 0.
   - Disables all IRQs on Core 1.
   - `__wfe()` spins in RAM until Core 0 clears the lock.
3. Core 0 completes the write, clears `flash_lock`, `__sev()`.
4. Core 1 restores IRQs and returns.

**Rule.** Never add code that writes flash from Core 1. Never disable
the `flash_range_*` wraps on Core 0. If you ever need to flash-write
from Core 1, you must reverse the roles — this has not been designed
and is not worth doing for anything currently on the roadmap.

---

## 7. Debugging Playbook

### 7.1 Symptom decision tree — "my driver does not work"

Work **top-down** through this checklist. Do not start optimising
register values before you have proven the code path is reached.
Every past session that skipped a step ended up chasing the wrong
bug.

1. **Was the task created?**
   - Publish `xTaskCreate` return code to a named `volatile BaseType_t`
     global (`g_rc_<task>_dbg`), read via SWD `mem32 &g_rc_<task>_dbg`.
   - `pdPASS` (= 1) means the task exists; `errCOULD_NOT_ALLOCATE`
     (= -1, two's-complement `0xFFFFFFFF`) means heap exhaustion.
   - If heap exhaustion: read `xFreeBytesRemaining`
     (`nm build/core1_bridge/core1_bridge.elf | grep xFreeBytes`) and
     cut a stack somewhere.
2. **Is the task running?**
   - Add `volatile uint32_t g_<driver>_tick_count` that increments
     on every iteration of the main loop. Zero after 1 s of runtime
     means the task is starved; small and monotonic means running.
3. **Are the peripheral registers configured?**
   - Read the relevant pad / ctrl / OE registers via SWD.
     Compare to §3.3 "healthy" column.
   - Compare to a peripheral that **does** work (display GPIO 10–22
     is the canonical reference for Core 1 GPIO init).
4. **Is the SDK code path actually doing anything?**
   - Pad at reset `0x00000116`, ctrl at reset `0x0000001F` = the SDK
     call silently returned without writing. See §2 — almost always
     `PICO_RP2350A=1` gating.
5. **Only now** start investigating device-specific behaviour
   (timing, protocol, etc.).

### 7.2 Gotcha catalogue

Recorded from M3.1 through M3.3 Phase A. Add to this table when a
new one bites you.

| Symptom                                      | Root cause                                                 | Fix / avoidance                                                          |
|----------------------------------------------|------------------------------------------------------------|--------------------------------------------------------------------------|
| `gpio_init(36)` silently no-op; pad `0x116`, ctrl `0x1F` | `PICO_BOARD=pico2` → `PICO_RP2350A=1` → `NUM_BANK0_GPIOS=30` | `mokya_rev_a.h` (this doc §2)                                            |
| Task never runs, `scan_count=0`              | Heap exhausted, `xTaskCreate` returned `errCOULD_NOT_ALLOCATE` | Check `rc`; size stacks against 32 KB heap (this doc §4.1)               |
| Task created but never scheduled             | Priority starvation: lower-prio task blocked by never-blocking higher-prio | Equal priority in round-robin group (this doc §4.3)                      |
| `clock_get_hz(clk_peri)` returns 0           | Core 1 skips `runtime_init_clocks`                         | Hard-code 150 MHz or read QMI / PLL directly                             |
| I2C NACKs inconsistently                     | `i2c_init` baud wrong because of `clock_get_hz=0`          | Call `i2c_set_baudrate_core1` right after `i2c_init` (this doc §5)       |
| `vTaskDelay(1000ms)` waits 112 s             | SysTick reload `0xFFFFFF` due to `clock_get_hz(clk_sys)=0` | `vPortSetupTimerInterrupt` override in `main_core1_bridge.c` (§4.4)      |
| Pin stays isolated even after `gpio_init`    | Raw pad-register write forgot to clear ISO bit 8           | Use SDK `gpio_set_function` (clears ISO automatically) — no raw writes   |
| `extern void gpio_set_dir(...)` links but does nothing | SDK `gpio_set_dir` is `static inline`; `extern` declaration has no definition to link | `#include "hardware/gpio.h"` directly, never re-declare SDK symbols      |
| Debug breadcrumb corrupted or flickering     | Slot collision — another writer at same `0x2007FFxx`       | See firmware-architecture.md §9.3 and `feedback_breadcrumb_discipline`   |
| Driver "works sometimes"                     | Task runs but read happens between scans (halt caught the sleep) | Hold the stimulus steady; read `scan_count` to confirm multiple iterations |
| `pio_sm_init` returns `PICO_ERROR_BAD_ALIGNMENT`, or SM runs but pins don't move | PIO tries to address GPIO ≥ 32 with default `gpiobase = 0` | Call `pio_set_gpio_base(pio, 16)` before `pio_sm_init` (this doc §3.4) |
| Two tasks both want to "own" the same GPIO   | `gpio_init` (SIO) vs `pio_gpio_init` (PIO0/1) fight over FUNCSEL; last call wins | One driver per pin group; use compile flag or explicit start/stop transitions when A/B testing |

### 7.3 SWD one-liners

```sh
# Dump a named symbol (no hard-coded address)
"C:/Program Files/Arm/GNU Toolchain .../arm-none-eabi-nm.exe" \
  build/core1_bridge/core1_bridge.elf | grep <symbol>
# → gives address; then via J-Link: mem32 <addr> [count]

# Halt + read a block of named variables
cat > /tmp/j.jlink <<'EOF'
connect
h
mem32 <addr1> 1
mem8  <addr2> 6
g
qc
EOF
"C:/Program Files/SEGGER/JLink_V932/JLink.exe" \
  -device RP2350_M33_0 -if SWD -speed 4000 -autoconnect 1 \
  -CommanderScript "$(cygpath -w /tmp/j.jlink)"
```

USB CDC disconnects while the MCU is halted; host COM port returns
after `g` (go). If the port sticks, re-flash via J-Link to force a
clean USB re-enumeration.

### 7.4 Breadcrumb discipline (TL;DR)

Default: **no breadcrumbs**. Prefer `printf` via the bridge or SWD
reads of named variables. If you must add a breadcrumb, register the
slot in `firmware-architecture.md §9.3` in the same commit; remove
the breadcrumb in the commit that closes the fix. Full rationale in
the `feedback_breadcrumb_discipline` memory entry.

---

## 8. New-Driver Checklist

Run through this list when adding a new peripheral driver to Core 1.

- [ ] Peripheral ownership claimed in `firmware-architecture.md §7`.
- [ ] Source lives under `firmware/core1/src/<peripheral>/` and is
      added to `firmware/core1/m1_bridge/CMakeLists.txt`
      (`add_executable` sources + `target_include_directories`).
- [ ] Uses `firmware/core1/m1_bridge/boards/mokya_rev_a.h` (default
      for this image — nothing to change unless you meddled with
      `PICO_BOARD`).
- [ ] No `clock_get_hz()` calls — hard-code 150 MHz or use a
      MokyaLora helper (`i2c_set_baudrate_core1`, ...).
- [ ] No raw PAD / IO_BANK0 register writes. Use SDK `gpio_*` only.
- [ ] Task priority matches the round-robin group
      (`tskIDLE_PRIORITY + 2`) unless the work is genuinely
      optional-and-deferrable.
- [ ] `xTaskCreate` return code checked (at least published to a
      named global while the driver is still experimental).
- [ ] Stack budget fits the 32 KB FreeRTOS heap; verified with
      `xFreeBytesRemaining` after boot.
- [ ] No new breadcrumb slots; if really needed, registered in
      `firmware-architecture.md §9.3` in the same commit.
- [ ] Smoke test via named variable + SWD `mem32` before adding any
      LVGL / queue / IPC plumbing on top.
