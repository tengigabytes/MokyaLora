/* watchdog_task.c — see watchdog_task.h. */

#include "watchdog_task.h"

#include "hardware/watchdog.h"

#include "ipc_shared_layout.h"
#include "postmortem.h"
#include "msp_canary.h"

volatile uint32_t g_wd_state;

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
        }

        uint32_t cnt = (g_wd_state & WD_STATE_COUNT_MASK);
        if (paused != 0u) {
            /* While caller is in a known-long blocking op, keep kicking
             * unconditionally — silence is expected, not a fault. */
            watchdog_update();
            cnt = (cnt + 1) & WD_STATE_COUNT_MASK;
            g_wd_state = WD_STATE_PAUSED_KICK | cnt;
        } else if (silent_ticks >= WD_SILENT_LIMIT_TICKS) {
            /* Real Core 0 hang. Snapshot state into the cross-reset
             * postmortem slot so the post-reset boot can surface a
             * causal report, then stop kicking — HW watchdog (3 s)
             * wins. mokya_pm_snapshot_silent is first-event-wins so
             * we re-arm only on the transition tick (g_wd_state high
             * byte just changed). */
            if ((g_wd_state >> 24) != (WD_STATE_SILENT >> 24)) {
                mokya_pm_snapshot_silent(cur, g_wd_state, silent_ticks,
                                         paused);
            }
            g_wd_state = WD_STATE_SILENT | cnt;
        } else {
            watchdog_update();
            cnt = (cnt + 1) & WD_STATE_COUNT_MASK;
            g_wd_state = WD_STATE_KICK | cnt;
        }

        /* Refresh MSP canary high-water every tick. Scan is bounded by
         * 512 word loads, terminates at the first non-canary, no allocs,
         * no printf — safe on this 192-word stack. Result lands in
         * g_msp_peak_used / g_msp_low_water_addr for SWD readback. */
        msp_canary_refresh();

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
