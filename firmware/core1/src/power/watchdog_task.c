/* watchdog_task.c — see watchdog_task.h. */

#include "watchdog_task.h"

#include "hardware/watchdog.h"

#include "ipc_shared_layout.h"

volatile uint32_t g_wd_state;
volatile uint32_t g_wd_silent_max;

#define WD_STATE_KICK         (1u << 24)
#define WD_STATE_PAUSED_KICK  (2u << 24)
#define WD_STATE_SILENT       (3u << 24)
#define WD_STATE_COUNT_MASK   0x00FFFFFFu

static TaskHandle_t s_handle;

static void wd_task(void *pv)
{
    (void)pv;

    /* Enable the HW watchdog from inside the task so the timer only
     * starts after the scheduler is up and we have just successfully
     * scheduled at least once. pause_on_debug=true keeps the chip alive
     * while a J-Link halts the CPU for SWD inspection. */
    watchdog_enable(WD_HW_TIMEOUT_MS, /*pause_on_debug=*/true);

    uint32_t last_seen   = __atomic_load_n(&g_ipc_shared.c0_heartbeat,
                                           __ATOMIC_RELAXED);
    uint32_t silent_ticks = 0;

    for (;;) {
        uint32_t cur = __atomic_load_n(&g_ipc_shared.c0_heartbeat,
                                       __ATOMIC_RELAXED);
        uint32_t paused = __atomic_load_n(&g_ipc_shared.wd_pause,
                                          __ATOMIC_RELAXED);

        if (cur != last_seen) {
            last_seen    = cur;
            silent_ticks = 0;
        } else {
            silent_ticks++;
            if (silent_ticks > g_wd_silent_max) g_wd_silent_max = silent_ticks;
        }

        uint32_t cnt = (g_wd_state & WD_STATE_COUNT_MASK);
        if (paused != 0u) {
            /* While caller is in a known-long blocking op, keep kicking
             * unconditionally — silence is expected, not a fault. */
            watchdog_update();
            cnt = (cnt + 1) & WD_STATE_COUNT_MASK;
            g_wd_state = WD_STATE_PAUSED_KICK | cnt;
        } else if (silent_ticks >= WD_SILENT_LIMIT_TICKS) {
            /* Real Core 0 hang. Stop kicking; HW watchdog (3 s) wins. */
            g_wd_state = WD_STATE_SILENT | cnt;
        } else {
            watchdog_update();
            cnt = (cnt + 1) & WD_STATE_COUNT_MASK;
            g_wd_state = WD_STATE_KICK | cnt;
        }

        vTaskDelay(pdMS_TO_TICKS(WD_TICK_MS));
    }
}

BaseType_t watchdog_task_start(UBaseType_t priority)
{
    if (s_handle != NULL) return pdPASS;
    /* 192 words plenty — wd_task only does atomic loads, integer math,
     * and a register write inside watchdog_update(). No printf, no
     * blocking calls, no callbacks. */
    BaseType_t rc = xTaskCreate(wd_task, "wd", 192, NULL, priority, &s_handle);
    return rc;
}
