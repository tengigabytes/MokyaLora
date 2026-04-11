/*
 * tusb_config.h — MokyaLora Phase 2 M1.1-B Core 1 m1_bridge
 *
 * Device-only TinyUSB configuration: single CDC interface, no MSC/HID/other
 * classes. CDC transfer buffers sized generously (1024 B each direction) to
 * absorb burst traffic from Meshtastic's serial console while the host
 * drains it at full-speed USB rates.
 *
 * Pico SDK's tinyusb_device target automatically sets CFG_TUSB_MCU to
 * OPT_MCU_RP2040 (which covers RP2350 as well — same dcd_rp2040 port).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ── Board / RHPort ─────────────────────────────────────────────────────── */
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_DEFAULT_SPEED
#endif

/* ── Common ─────────────────────────────────────────────────────────────── */
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

/* CFG_TUSB_OS is set by the Pico SDK (tinyusb_device target) to OPT_OS_PICO,
 * which wires TinyUSB's internal sync primitives to hardware_mutex. That is
 * compatible with calling tud_task() from both a busy-poll loop (before the
 * scheduler starts) and a FreeRTOS task (after it starts), because hardware
 * mutexes don't depend on the scheduler. We intentionally do NOT override it
 * here — the SDK's -D from the command line wins. */
#define CFG_TUSB_DEBUG        0

#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

/* Legacy tusb_init(void) expects CFG_TUSB_RHPORT0_MODE to pick the mode+speed
 * of roothub port 0. OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED matches our single
 * CDC-device configuration running off RP2350's full-speed USB controller. */
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN    __attribute__((aligned(4)))

/* ── Device configuration ───────────────────────────────────────────────── */
#define CFG_TUD_ENDPOINT0_SIZE    64

/* Exactly one CDC interface, nothing else. */
#define CFG_TUD_CDC              1
#define CFG_TUD_MSC              0
#define CFG_TUD_HID              0
#define CFG_TUD_MIDI             0
#define CFG_TUD_VENDOR           0

/* CDC FIFOs — the TX fifo absorbs Meshtastic boot log bursts, RX fifo is
 * only touched by user input from the host (meshtastic CLI commands). */
#define CFG_TUD_CDC_RX_BUFSIZE   1024
#define CFG_TUD_CDC_TX_BUFSIZE   1024

/* USB packet buffer. 64 B is full-speed bulk max. */
#define CFG_TUD_CDC_EP_BUFSIZE   64

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
