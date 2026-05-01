/* cpu_load.c — see cpu_load.h. */

#include "cpu_load.h"

#include "FreeRTOS.h"
#include "task.h"

/* ── Idle hook counter ────────────────────────────────────────────── *
 *
 * Single 32-bit counter bumped on every FreeRTOS idle task iteration.
 * 32-bit is fine — even at 100M+ idle/s, wraparound is once per 43s
 * which a 1 s sample window comfortably handles via mod-2^32 subtraction. */
volatile uint32_t g_cpu_idle_count    __attribute__((used)) = 0u;
volatile uint32_t g_cpu_idle_baseline __attribute__((used)) = 0u;
volatile uint8_t  g_cpu_load_pct_instant __attribute__((used)) = UINT8_MAX;
volatile uint8_t  g_cpu_load_pct_avg10   __attribute__((used)) = UINT8_MAX;
volatile uint32_t g_cpu_load_windows  __attribute__((used)) = 0u;

void vApplicationIdleHook(void)
{
    /* Hot-path: must NOT block per FreeRTOS contract. Single increment. */
    g_cpu_idle_count++;
}

/* ── Sample task ──────────────────────────────────────────────────── */

#define CPU_LOAD_AVG_WINDOW  10u    /* number of 1s windows averaged */

/* Rolling buffer + indices in PSRAM (no SWD inspection / no early-boot
 * use); saves 12 B SRAM. */
static uint8_t  s_avg_buf[CPU_LOAD_AVG_WINDOW] __attribute__((section(".psram_bss")));
static uint8_t  s_avg_idx                      __attribute__((section(".psram_bss")));
static uint8_t  s_avg_filled                   __attribute__((section(".psram_bss")));

static void apply_sample(uint32_t delta)
{
    /* Always count the window — even when the baseline can't yet be
     * established. Lets the test see liveness on idle-starved firmware. */
    g_cpu_load_windows++;

    /* Auto-calibrate baseline. First non-zero delta seeds it; subsequent
     * windows that exceed the baseline by >5% recalibrate (e.g. CPU
     * clock changed or the original baseline was contaminated). */
    if (g_cpu_idle_baseline == 0u && delta > 0u) {
        g_cpu_idle_baseline = delta;
        return;
    }
    if (delta > g_cpu_idle_baseline + (g_cpu_idle_baseline / 20u)) {
        g_cpu_idle_baseline = delta;
    }
    if (g_cpu_idle_baseline == 0u) {
        /* Idle never ran — declare 100% busy. */
        g_cpu_load_pct_instant = 100u;
        return;
    }
    /* busy_pct = 100 - 100 * delta / baseline, clamped. */
    uint32_t busy_pct;
    if (delta >= g_cpu_idle_baseline) {
        busy_pct = 0u;
    } else {
        busy_pct = 100u - (uint32_t)((uint64_t)delta * 100u / g_cpu_idle_baseline);
        if (busy_pct > 100u) busy_pct = 100u;
    }
    g_cpu_load_pct_instant = (uint8_t)busy_pct;

    /* Rolling 10-window average. */
    s_avg_buf[s_avg_idx] = (uint8_t)busy_pct;
    s_avg_idx = (uint8_t)((s_avg_idx + 1u) % CPU_LOAD_AVG_WINDOW);
    if (s_avg_filled < CPU_LOAD_AVG_WINDOW) s_avg_filled++;
    if (s_avg_filled == CPU_LOAD_AVG_WINDOW) {
        uint32_t sum = 0u;
        for (int i = 0; i < (int)CPU_LOAD_AVG_WINDOW; i++) sum += s_avg_buf[i];
        g_cpu_load_pct_avg10 = (uint8_t)(sum / CPU_LOAD_AVG_WINDOW);
    } else {
        g_cpu_load_pct_avg10 = UINT8_MAX;
    }
}

static void cpu_load_task(void *pv)
{
    (void)pv;
    uint32_t prev = g_cpu_idle_count;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));
        uint32_t now = g_cpu_idle_count;
        uint32_t delta = now - prev;     /* mod-2^32 */
        prev = now;
        apply_sample(delta);
    }
}

bool cpu_load_start(UBaseType_t priority)
{
    /* 256 words (1 KB). Just a delay loop + mod-2^32 arithmetic — small. */
    BaseType_t rc = xTaskCreate(cpu_load_task, "cpu_load", 256, NULL,
                                priority, NULL);
    return rc == pdPASS;
}

uint8_t  cpu_load_pct_instant(void) { return g_cpu_load_pct_instant; }
uint8_t  cpu_load_pct_avg10(void)   { return g_cpu_load_pct_avg10; }
uint32_t cpu_load_window_count(void){ return g_cpu_load_windows; }
