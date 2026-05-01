/* vib_motor.h — ERM vibration motor driver (HD-EMB1104-SM-2).
 *
 * Hardware: GPIO 9 → SSM3K56ACT N-MOSFET low-side switch → motor → VSYS.
 * Flyback diode installed (Rev A bring-up). PWM4_B slice/channel.
 *
 * The driver owns a small FreeRTOS task that consumes a SPSC pattern
 * queue. Patterns are sequences of (on_ms, off_ms) pulses with an
 * intensity (0..100, mapped to PWM duty). Only one pattern plays at a
 * time — a new request preempts whatever is in flight.
 *
 * Power gates:
 *   - master enable (notif_settings_t) — checked by the notification
 *     core BEFORE calling vib_play; this driver is mode-agnostic.
 *   - VSYS guard — if charger reports VSYS < 3.3 V, vib_play returns
 *     without queueing. Prevents brown-out under low battery.
 *   - Sleep gate — vib_suspend()/vib_resume() for the future DORMANT
 *     state machine; suspended state silently drops new requests and
 *     stops the current pattern.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_VIB_MOTOR_H
#define MOKYA_CORE1_VIB_MOTOR_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GPIO 9 = MTR_PWM (PWM slice 4 channel B). */
#define VIB_MOTOR_GPIO          9u
#define VIB_PATTERN_MAX_STEPS   8u

typedef struct {
    uint16_t on_ms;
    uint16_t off_ms;
} vib_pulse_t;

typedef struct {
    uint8_t      n_steps;       /* 0..VIB_PATTERN_MAX_STEPS */
    uint8_t      intensity;     /* 0..100 → PWM duty */
    uint8_t      reserved[2];
    vib_pulse_t  steps[VIB_PATTERN_MAX_STEPS];
} vib_pattern_t;

/* Built-in pattern presets. Convenience wrappers around vib_play. */
typedef enum {
    VIB_PRESET_TICK = 0,        /* single 30 ms pulse — keypress feedback */
    VIB_PRESET_SHORT,           /* 100 ms pulse — ACK / charge insert */
    VIB_PRESET_DOUBLE,          /* 80-120-80 ms — DM */
    VIB_PRESET_LONG,            /* 600 ms pulse — SOS / urgent */
    VIB_PRESET_TRIPLE,          /* 60×3 with 80 ms gaps — broadcast */
    VIB_PRESET_COUNT
} vib_preset_t;

/* Initialise the PWM slice + start the pattern task. Idempotent. Returns
 * true on success. The task runs at the supplied priority. */
bool vib_motor_init(UBaseType_t priority);

/* Enqueue the supplied pattern (replaces any in-flight pattern). The
 * driver makes its own copy. Returns true if accepted, false if
 * suspended / VSYS guard tripped / driver not initialised. */
bool vib_play(const vib_pattern_t *pat);

/* Convenience: play a built-in preset at the supplied intensity. */
bool vib_play_preset(vib_preset_t preset, uint8_t intensity);

/* Cancel the current pattern (if any). Safe to call from any task. */
void vib_cancel(void);

/* Sleep gates for the future DORMANT state machine. Suspending stops
 * the current pulse and silently drops new requests until resume. */
void vib_suspend(void);
void vib_resume(void);

/* SWD-readable state for diag UI. */
typedef struct {
    bool    initialised;
    bool    suspended;
    bool    busy;               /* pattern currently playing */
    uint8_t intensity_last;
    uint16_t plays_total;       /* monotonic count of accepted patterns */
    uint16_t plays_dropped;     /* pre-empted / suspended / vsys */
} vib_motor_stats_t;

void vib_motor_get_stats(vib_motor_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif /* MOKYA_CORE1_VIB_MOTOR_H */
