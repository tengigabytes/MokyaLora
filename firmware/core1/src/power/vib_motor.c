/* vib_motor.c — see vib_motor.h. */

#include "vib_motor.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"

#include "bq25622.h"

/* PWM frequency choice: 25 kHz keeps the audible whine out of the
 * human range, well under the SSM3K56ACT switching limits. The motor
 * itself is mechanical (~150 Hz characteristic), so the PWM rail is
 * just a duty source. sys_clk = 150 MHz → divider 60 / wrap 100 gives
 * a 25 kHz period with 1 % duty resolution. */
#define VIB_PWM_DIVIDER  60
#define VIB_PWM_WRAP     100u
#define VIB_VSYS_GUARD_MV  3300u

/* Pattern queue depth — only the most-recent matters; if the task is
 * busy with one, a new request preempts. Use a 1-deep mailbox semantic
 * via xQueueOverwrite. */
#define VIB_QUEUE_LEN     1u

/* PSRAM-resident state (no SWD coherency needed for these — debug
 * snapshots come through vib_motor_get_stats). */
static TaskHandle_t   s_task            __attribute__((section(".psram_bss")));
static QueueHandle_t  s_queue           __attribute__((section(".psram_bss")));
static volatile bool  s_initialised     __attribute__((section(".psram_bss")));
static volatile bool  s_suspended       __attribute__((section(".psram_bss")));
static volatile bool  s_cancel          __attribute__((section(".psram_bss")));
static volatile bool  s_busy            __attribute__((section(".psram_bss")));
static uint8_t        s_pwm_slice       __attribute__((section(".psram_bss")));
static uint8_t        s_pwm_chan        __attribute__((section(".psram_bss")));

/* SWD-readable counters in .bss for direct inspection. */
volatile uint8_t  g_vib_intensity_last __attribute__((used)) = 0u;
volatile uint16_t g_vib_plays_total    __attribute__((used)) = 0u;
volatile uint16_t g_vib_plays_dropped  __attribute__((used)) = 0u;

static void vib_set_duty(uint8_t intensity_pct)
{
    if (!s_initialised) return;
    if (intensity_pct > 100u) intensity_pct = 100u;
    /* Direct linear map: 0 % = level 0, 100 % = wrap. */
    pwm_set_chan_level(s_pwm_slice, s_pwm_chan, (uint16_t)intensity_pct);
}

static bool vib_vsys_ok(void)
{
    /* Charger driver provides last-sampled VSYS via its public state.
     * If the driver hasn't reported yet (vsys_mv == 0) treat as OK so
     * we don't dead-lock pattern playback during early boot. */
    const bq25622_state_t *bq = bq25622_get_state();
    if (bq == NULL || bq->vsys_mv == 0u) return true;
    return bq->vsys_mv >= VIB_VSYS_GUARD_MV;
}

static void vib_task(void *pv)
{
    (void)pv;
    vib_pattern_t cur;
    for (;;) {
        if (xQueueReceive(s_queue, &cur, portMAX_DELAY) != pdTRUE) continue;
        if (s_suspended) continue;

        s_busy = true;
        s_cancel = false;
        g_vib_intensity_last = cur.intensity;

        for (uint8_t i = 0; i < cur.n_steps && !s_cancel && !s_suspended; i++) {
            const vib_pulse_t *p = &cur.steps[i];
            if (p->on_ms > 0u) {
                vib_set_duty(cur.intensity);
                vTaskDelay(pdMS_TO_TICKS(p->on_ms));
                vib_set_duty(0u);
            }
            if (p->off_ms > 0u && !s_cancel && !s_suspended) {
                vTaskDelay(pdMS_TO_TICKS(p->off_ms));
            }
        }
        vib_set_duty(0u);
        s_busy = false;
    }
}

bool vib_motor_init(UBaseType_t priority)
{
    if (s_initialised) return true;

    s_queue = xQueueCreate(VIB_QUEUE_LEN, sizeof(vib_pattern_t));
    if (s_queue == NULL) return false;

    /* GPIO + PWM setup. */
    gpio_set_function(VIB_MOTOR_GPIO, GPIO_FUNC_PWM);
    s_pwm_slice = pwm_gpio_to_slice_num(VIB_MOTOR_GPIO);
    s_pwm_chan  = pwm_gpio_to_channel(VIB_MOTOR_GPIO);
    pwm_set_clkdiv(s_pwm_slice, (float)VIB_PWM_DIVIDER);
    pwm_set_wrap(s_pwm_slice, VIB_PWM_WRAP - 1u);
    pwm_set_chan_level(s_pwm_slice, s_pwm_chan, 0u);
    pwm_set_enabled(s_pwm_slice, true);

    BaseType_t rc = xTaskCreate(vib_task, "vib", 256, NULL, priority, &s_task);
    if (rc != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return false;
    }
    s_initialised = true;
    return true;
}

bool vib_play(const vib_pattern_t *pat)
{
    if (!s_initialised || pat == NULL) return false;
    if (s_suspended) {
        g_vib_plays_dropped++;
        return false;
    }
    if (!vib_vsys_ok()) {
        g_vib_plays_dropped++;
        return false;
    }
    /* Truncate over-long patterns silently; clamp intensity. */
    vib_pattern_t copy = *pat;
    if (copy.n_steps > VIB_PATTERN_MAX_STEPS) copy.n_steps = VIB_PATTERN_MAX_STEPS;
    if (copy.intensity > 100u) copy.intensity = 100u;

    /* Mailbox semantics: overwrite drops any pending pattern.
     * If a pattern is mid-play, set s_cancel so the task aborts the
     * current sequence and pulls the new one immediately. */
    s_cancel = true;
    xQueueOverwrite(s_queue, &copy);
    g_vib_plays_total++;
    return true;
}

bool vib_play_preset(vib_preset_t preset, uint8_t intensity)
{
    vib_pattern_t pat;
    memset(&pat, 0, sizeof(pat));
    pat.intensity = intensity;
    switch (preset) {
        case VIB_PRESET_TICK:
            pat.n_steps = 1;
            pat.steps[0] = (vib_pulse_t){ .on_ms = 30, .off_ms = 0 };
            break;
        case VIB_PRESET_SHORT:
            pat.n_steps = 1;
            pat.steps[0] = (vib_pulse_t){ .on_ms = 100, .off_ms = 0 };
            break;
        case VIB_PRESET_DOUBLE:
            pat.n_steps = 2;
            pat.steps[0] = (vib_pulse_t){ .on_ms = 80, .off_ms = 120 };
            pat.steps[1] = (vib_pulse_t){ .on_ms = 80, .off_ms = 0 };
            break;
        case VIB_PRESET_LONG:
            pat.n_steps = 1;
            pat.steps[0] = (vib_pulse_t){ .on_ms = 600, .off_ms = 0 };
            break;
        case VIB_PRESET_TRIPLE:
            pat.n_steps = 3;
            pat.steps[0] = (vib_pulse_t){ .on_ms = 60, .off_ms = 80 };
            pat.steps[1] = (vib_pulse_t){ .on_ms = 60, .off_ms = 80 };
            pat.steps[2] = (vib_pulse_t){ .on_ms = 60, .off_ms = 0 };
            break;
        default:
            return false;
    }
    return vib_play(&pat);
}

void vib_cancel(void)
{
    if (!s_initialised) return;
    s_cancel = true;
    /* Drain any pending mailbox so it doesn't fire after the cancel. */
    vib_pattern_t scratch;
    (void)xQueueReceive(s_queue, &scratch, 0);
    vib_set_duty(0u);
}

void vib_suspend(void)
{
    s_suspended = true;
    s_cancel = true;
    if (s_initialised) vib_set_duty(0u);
}

void vib_resume(void)
{
    s_suspended = false;
}

void vib_motor_get_stats(vib_motor_stats_t *out)
{
    if (out == NULL) return;
    out->initialised    = s_initialised;
    out->suspended      = s_suspended;
    out->busy           = s_busy;
    out->intensity_last = g_vib_intensity_last;
    out->plays_total    = g_vib_plays_total;
    out->plays_dropped  = g_vib_plays_dropped;
}
