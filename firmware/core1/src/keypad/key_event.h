/* key_event.h — Multi-producer KeyEvent queue for Core 1.
 *
 * Normative reference: firmware-architecture.md §4.4, usb-control-protocol.md §9.
 *
 * The queue is fed by two producers:
 *   - KeypadScan task   → physical matrix, source = KEY_SOURCE_HW
 *   - UsbCtrlTask (M9)  → CDC#1 Control Protocol, source = KEY_SOURCE_INJECT
 *
 * and drained by one consumer (IMETask / UITask in the final design; in
 * Phase B there is no consumer yet — the queue fills and this module's
 * counters are the primary observability channel).
 *
 * Arbitration rule (HW wins within the 20 ms debounce window):
 *   An INJECT push is rejected with KEY_EVENT_ERR_BUSY if KeypadScan's
 *   current debounced state reports the same keycode as already
 *   physically pressed. This is tracked in a 64-bit bitmap inside
 *   key_event.c, updated from keypad_scan_task via key_event_mark_hw().
 *
 * Thread safety: all entry points below are safe to call concurrently
 * from FreeRTOS tasks (but NOT from ISRs — scan task calls the push
 * path from a regular task context). FreeRTOS xQueueSend / xQueueReceive
 * take the queue's internal mutex; the HW-pressed bitmap is protected
 * by taskENTER_CRITICAL().
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mie/keycode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Event source identifier — matches usb-control-protocol.md §9 wire. */
typedef enum {
    KEY_SOURCE_HW     = 0,
    KEY_SOURCE_INJECT = 1,
} key_source_t;

/* 2-byte packed event. Alignment matches the FreeRTOS queue item size
 * declared in key_event_init(). The bitfield layout is stable across
 * the three producers/consumers because we never serialise this struct
 * over the wire — only through FreeRTOS queues between in-process
 * tasks. */
typedef struct {
    uint8_t keycode;            /* mie/keycode.h constants, 0x01..0x3F   */
    uint8_t pressed : 1;        /* 1 = press, 0 = release                */
    uint8_t source  : 1;        /* key_source_t                           */
    uint8_t flags   : 6;        /* reserved — long-press hint, M9+        */
} key_event_t;

/* Result codes returned by key_event_push_* helpers. */
typedef enum {
    KEY_EVENT_OK            = 0,
    KEY_EVENT_ERR_NOT_READY = 1,  /* init() not called yet                */
    KEY_EVENT_ERR_QUEUE_FULL = 2, /* consumer is too slow / stuck         */
    KEY_EVENT_ERR_BUSY      = 3,  /* INJECT lost arbitration to HW        */
    KEY_EVENT_ERR_BAD_KEY   = 4,  /* keycode outside 0x01..0x3F            */
} key_event_result_t;

/* Create the FreeRTOS queue and zero internal state.
 * Must be called before the scheduler starts consuming KeyEvent producers. */
void key_event_init(void);

/* Push from KeypadScan (HW source).  Non-blocking: if the queue is
 * full this returns KEY_EVENT_ERR_QUEUE_FULL immediately and increments
 * g_key_event_dropped — dropping on a stuck consumer is strictly
 * preferable to stalling the scan task.
 *
 * Also updates the internal "HW currently pressed" bitmap so that a
 * subsequent INJECT for the same keycode is rejected per §9.1. */
key_event_result_t key_event_push_hw(mokya_keycode_t keycode, bool pressed);

/* Like key_event_push_hw but also carries the producer-side flag bits
 * (bit 0 = long-press, bit 1 = long-long, etc — see mie/hal_port.h
 * KEY_FLAG_*). Used by KeypadScan's deferred-press path so the IME can
 * tell which phoneme of a half-key the user intended. The pressed/release
 * arbitration semantics are identical to key_event_push_hw. */
key_event_result_t key_event_push_hw_flags(mokya_keycode_t keycode,
                                           bool pressed, uint8_t flags);

/* Push from UsbCtrlTask (INJECT source).  Returns KEY_EVENT_ERR_BUSY
 * without touching the queue if KeypadScan currently holds `keycode`
 * physically pressed — the human user wins. Reserved for M9; provided
 * now so the queue's final shape is in place. */
key_event_result_t key_event_push_inject(mokya_keycode_t keycode, bool pressed);

/* Inject variant carrying flags (Phase 1.4 — lets SWD tooling and the
 * USB Control Protocol simulate long-press intent). Same arbitration
 * rules as key_event_push_inject. */
key_event_result_t key_event_push_inject_flags(mokya_keycode_t keycode,
                                                bool pressed, uint8_t flags);

/* Consumer drain — waits up to `timeout_ticks` for an event to arrive.
 * Returns true if *out was populated.
 *
 * Primary consumer is the IME task. Every successful push_* call also
 * fans the event out into an independent "view" queue so the LVGL view
 * router can observe keystrokes (FUNC for panel switch, keypad_view
 * cell highlight) without competing with the IME task for the main
 * queue. Drain the view queue via key_event_view_pop(). */
bool key_event_pop(key_event_t *out, uint32_t timeout_ticks);

/* View-side observer drain. Non-blocking drops are acceptable — the
 * view queue is a mirror for UI highlight/navigation, not a control
 * path. Safe to poll from the LVGL task. */
bool key_event_view_pop(key_event_t *out, uint32_t timeout_ticks);

/* Snapshot of the HW-pressed bitmap — primarily for SWD diagnosis and
 * for future arbiter logic that wants to batch-check multiple keycodes.
 * Bit N corresponds to keycode N (N in 0x01..0x3F). */
uint64_t key_event_hw_pressed_mask(void);

/* ── SWD-observable counters ────────────────────────────────────────── */
extern volatile uint32_t g_key_event_pushed;    /* successful enqueues   */
extern volatile uint32_t g_key_event_dropped;   /* queue-full drops      */
extern volatile uint32_t g_key_event_rejected;  /* INJECT lost to HW     */

/* ── SWD-observable event ring (Phase B diagnostic) ──────────────────── *
 * Each byte captures one successful push:
 *   bit 7 = pressed (1) / release (0)
 *   bits 6..0 = keycode (0x01..0x3F)
 * g_key_event_log_idx is the write index modulo the ring depth —
 * oldest entry is at (idx + 1) % DEPTH, newest at (idx - 1) % DEPTH
 * after any event. Useful to verify matrix→keycode translation
 * without a downstream consumer. */
#define KEY_EVENT_LOG_DEPTH 16u
extern volatile uint8_t  g_key_event_log[KEY_EVENT_LOG_DEPTH];
extern volatile uint32_t g_key_event_log_idx;

#ifdef __cplusplus
} /* extern "C" */
#endif
