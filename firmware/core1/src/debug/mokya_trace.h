/* mokya_trace.h — structured RTT trace macros for Core 1.
 *
 * Wire format (CSV, one event per line on RTT up channel 0):
 *
 *   <us_timestamp>,<source>,<event>[,<key=val>...]\n
 *
 *   12345678,ime,key_pop,kc=0x12
 *   12345692,lvgl,render_start
 *   12345721,lvgl,flush_done
 *
 * Timestamp source: timer_hw->timerawl (RP2350 1 MHz hardware timer, raw
 * 32-bit lower half — wraps every ~71 minutes, fine for latency profiling).
 * Reading is a single 32-bit MMIO load, no syscall.
 *
 * Channel layout (reserved for future use):
 *   ch 0  text trace (this file)
 *   ch 1  reserved — binary packed trace events (future)
 *
 * Host:
 *   J-Link: JLinkRTTLogger.exe -Device RP2350_M33_1 -if SWD -speed 4000 \
 *           -RTTChannel 0 <output.log>
 *   OpenOCD (raspberrypi fork): rtt setup 0x20000000 0x80000 "SEGGER RTT" ;
 *                               rtt start ; rtt server start 9090 0
 *   pyOCD:  pyocd rtt --target rp2350 --rtt-search-range 0x20000000 0x80000
 *
 * Implementation:
 *   pico-sdk's bundle of SEGGER source omits SEGGER_RTT_printf.c (pico_stdio_rtt
 *   routes through pico's own printf). We therefore format with newlib's
 *   vsnprintf into a stack buffer and ship the bytes via SEGGER_RTT_Write.
 *   The non-blocking SEGGER_RTT_Write skips silently if the up buffer is
 *   full — for latency profiling, the dropped-event signal is itself
 *   information; do not switch to BLOCK_IF_FIFO_FULL or trace points
 *   become serialisation barriers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "SEGGER_RTT.h"
#include "hardware/timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SWD-readable instrumentation. Updated inside mokya_trace_emit. */
extern volatile uint32_t g_trace_emit_count;
extern volatile uint32_t g_trace_drop_events;
extern volatile uint32_t g_trace_drop_bytes;

/* Format a single trace event into an internal stack buffer (~128 B) and
 * push it to RTT up channel 0. Use the TRACE / TRACE_BARE macros below
 * rather than calling this directly, so the timestamp is read at the
 * trace point rather than at function entry. */
void mokya_trace_emit(unsigned long ts_us,
                      const char *src,
                      const char *ev,
                      const char *fmt,
                      ...) __attribute__((format(printf, 4, 5)));

#ifdef __cplusplus
} /* extern "C" */
#endif

/* TRACE — variadic CSV trace event with payload key=val fields.
 *
 *   TRACE("ime", "key_pop", "kc=0x%02x", ev.keycode);
 *
 * Format string MUST be a string literal (printf-style). Use TRACE_BARE
 * if there are no extra fields. Trailing newline appended automatically.
 */
#define TRACE(src, ev, fmt, ...)                                            \
    mokya_trace_emit((unsigned long)timer_hw->timerawl,                     \
                     (src), (ev), (fmt), ##__VA_ARGS__)

/* TRACE_BARE — no payload; emits "<ts>,src,ev\n". */
#define TRACE_BARE(src, ev)                                                 \
    mokya_trace_emit((unsigned long)timer_hw->timerawl,                     \
                     (src), (ev), "")
