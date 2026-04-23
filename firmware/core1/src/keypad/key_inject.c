/* key_inject.c — see key_inject.h
 * SPDX-License-Identifier: Apache-2.0
 */
#include "key_inject.h"
#include "key_event.h"

#include "FreeRTOS.h"
#include "task.h"

volatile key_inject_buf_t g_key_inject_buf __attribute__((used));

static TaskHandle_t s_task = NULL;

static void key_inject_task_fn(void *arg)
{
    (void)arg;
    /* Initialise under task context so the magic appears only once the
     * scheduler is truly running — a host sees magic=0 on a halted core
     * and knows the channel isn't live yet. */
    g_key_inject_buf.producer_idx = 0;
    g_key_inject_buf.consumer_idx = 0;
    g_key_inject_buf.pushed       = 0;
    g_key_inject_buf.rejected     = 0;
    g_key_inject_buf.magic        = KEY_INJECT_MAGIC;

    for (;;) {
        uint32_t prod = g_key_inject_buf.producer_idx;
        while (g_key_inject_buf.consumer_idx != prod) {
            uint32_t idx   = g_key_inject_buf.consumer_idx
                             % KEY_INJECT_RING_EVENTS;
            uint8_t  ev    = g_key_inject_buf.events[idx * 2 + 0];
            uint8_t  flags = g_key_inject_buf.events[idx * 2 + 1];
            uint8_t  kc    = ev & 0x7Fu;
            int      pressed = (ev & 0x80u) ? 1 : 0;

            if (kc >= 0x01u && kc < 0x40u) {
                key_event_result_t r =
                    key_event_push_inject_flags((mokya_keycode_t)kc,
                                                 pressed, flags);
                if (r == KEY_EVENT_OK) {
                    g_key_inject_buf.pushed++;
                } else {
                    g_key_inject_buf.rejected++;
                }
            } else {
                g_key_inject_buf.rejected++;
            }
            g_key_inject_buf.consumer_idx++;
        }
        /* 5 ms poll — halves inject latency vs the original 10 ms while
         * costing negligible CPU (task sleeps 99 % of the time when no
         * events are queued). */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void key_inject_task_start(void)
{
    if (s_task) return;
    /* Small stack — the task loops on a tight poll and calls the queue
     * push helper (tiny footprint). 256 words = 1 KB. */
    /* Priority +2. Originally +1 (starved by every app task), bumped
     * to +2. +3 matches ime but starves ime when both RTT + SWD
     * inject tasks also sit at +3 — ime then can't generate candidates
     * fast enough. +2 is the sweet spot: lower than ime, but ime's
     * blocking waits give us enough time.                              */
    xTaskCreate(key_inject_task_fn, "key_inject",
                256, NULL, tskIDLE_PRIORITY + 2, &s_task);
}
