/* key_event.c — FreeRTOS queue + HW-pressed arbitration bitmap.
 *
 * Memory budget (Phase B):
 *   queue storage     : 16 slots * 2 B               =   32 B
 *   queue control blk : sizeof(StaticQueue_t) approx =   80 B (heap_4)
 *   hw_pressed bitmap : 8 B
 *   counters          : 12 B
 *   total             : ~ 132 B   (heap remaining ~5 KB — fits)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "key_event.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

volatile uint32_t g_key_event_pushed;
volatile uint32_t g_key_event_dropped;
volatile uint32_t g_key_event_rejected;

volatile uint8_t  g_key_event_log[KEY_EVENT_LOG_DEPTH];
volatile uint32_t g_key_event_log_idx;

#define KEY_EVENT_QUEUE_DEPTH  16u

static QueueHandle_t s_queue;

/* Bitmap indexed by keycode (1..0x3F). Bit 0 is unused (MOKYA_KEY_NONE).
 * Updated only from keypad_scan_task; read from any task context.
 * Concurrent R/W on a single uint64_t is not atomic on Cortex-M33
 * (needs LDREXD/STREXD), so all accesses are guarded by a FreeRTOS
 * critical section. Contention is negligible — scan task writes only
 * on state change (~20 ms debounce boundary). */
static uint64_t s_hw_pressed;

static inline bool keycode_valid(mokya_keycode_t k)
{
    return (k >= (mokya_keycode_t)1) && (k < MOKYA_KEY_LIMIT);
}

static inline uint64_t keycode_bit(mokya_keycode_t k)
{
    return (uint64_t)1u << (uint32_t)k;
}

void key_event_init(void)
{
    s_hw_pressed = 0u;
    g_key_event_pushed   = 0u;
    g_key_event_dropped  = 0u;
    g_key_event_rejected = 0u;
    s_queue = xQueueCreate(KEY_EVENT_QUEUE_DEPTH, sizeof(key_event_t));
    /* s_queue == NULL indicates heap exhaustion — callers observe this
     * via KEY_EVENT_ERR_NOT_READY. Creation failure is surfaced by
     * leaving s_queue NULL; the bridge still boots so other Core 1
     * services remain SWD-observable for post-mortem. */
}

static void hw_pressed_set(mokya_keycode_t keycode, bool pressed)
{
    const uint64_t mask = keycode_bit(keycode);
    taskENTER_CRITICAL();
    if (pressed) {
        s_hw_pressed |= mask;
    } else {
        s_hw_pressed &= ~mask;
    }
    taskEXIT_CRITICAL();
}

static bool hw_pressed_test(mokya_keycode_t keycode)
{
    const uint64_t mask = keycode_bit(keycode);
    taskENTER_CRITICAL();
    const bool r = (s_hw_pressed & mask) != 0u;
    taskEXIT_CRITICAL();
    return r;
}

uint64_t key_event_hw_pressed_mask(void)
{
    taskENTER_CRITICAL();
    const uint64_t v = s_hw_pressed;
    taskEXIT_CRITICAL();
    return v;
}

static key_event_result_t push_locked(key_event_t ev)
{
    if (s_queue == NULL) {
        return KEY_EVENT_ERR_NOT_READY;
    }
    /* Non-blocking push. A full queue means a consumer stall — drop
     * and count so debounce doesn't back-pressure. */
    if (xQueueSend(s_queue, &ev, 0) != pdPASS) {
        g_key_event_dropped++;
        return KEY_EVENT_ERR_QUEUE_FULL;
    }
    /* Mirror the enqueued event into the SWD-readable ring. Ordering:
     * log first, then bump idx — readers inspecting the ring will see
     * a coherent newest-entry pointer. */
    const uint32_t slot = g_key_event_log_idx % KEY_EVENT_LOG_DEPTH;
    g_key_event_log[slot] = (uint8_t)((ev.pressed ? 0x80u : 0u)
                                      | (ev.keycode & 0x7Fu));
    g_key_event_log_idx = g_key_event_log_idx + 1u;
    g_key_event_pushed++;
    return KEY_EVENT_OK;
}

key_event_result_t key_event_push_hw(mokya_keycode_t keycode, bool pressed)
{
    if (!keycode_valid(keycode)) {
        return KEY_EVENT_ERR_BAD_KEY;
    }
    /* Update the HW-pressed bitmap BEFORE enqueueing. An INJECT push
     * that races us will see the current HW state correctly:
     *   - release first, then enqueue → inject racing a "just released"
     *     key briefly wins, which is intentional (user let go).
     *   - press first, then enqueue → inject racing an active HW press
     *     is rejected with ERR_BUSY, which is the correct user-wins
     *     outcome. */
    hw_pressed_set(keycode, pressed);
    key_event_t ev = {
        .keycode = keycode,
        .pressed = pressed ? 1u : 0u,
        .source  = (uint8_t)KEY_SOURCE_HW,
        .flags   = 0u,
    };
    return push_locked(ev);
}

key_event_result_t key_event_push_inject(mokya_keycode_t keycode, bool pressed)
{
    if (!keycode_valid(keycode)) {
        return KEY_EVENT_ERR_BAD_KEY;
    }
    /* Arbitration: if HW currently holds this keycode, reject. Only
     * press-injects lose — releases always go through so a stuck
     * inject-press can still be cleared. */
    if (pressed && hw_pressed_test(keycode)) {
        g_key_event_rejected++;
        return KEY_EVENT_ERR_BUSY;
    }
    key_event_t ev = {
        .keycode = keycode,
        .pressed = pressed ? 1u : 0u,
        .source  = (uint8_t)KEY_SOURCE_INJECT,
        .flags   = 0u,
    };
    return push_locked(ev);
}

bool key_event_pop(key_event_t *out, uint32_t timeout_ticks)
{
    if (s_queue == NULL || out == NULL) {
        return false;
    }
    return xQueueReceive(s_queue, out, (TickType_t)timeout_ticks) == pdTRUE;
}
