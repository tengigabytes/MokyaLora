/*
 * core1_freertos_test.c — Step 16 Stage B: FreeRTOS on Core 1
 *
 * Architecture (matches production design):
 *   Core 0 — bare-metal launcher: wakes Core 1, then loops doing nothing.
 *   Core 1 — owns all peripherals: initialises USB CDC, starts FreeRTOS
 *             (configNUMBER_OF_CORES = 1, single-core scheduler on Core 1),
 *             runs heartbeat task.
 *
 * Core 1 calls stdio_init_all() itself, so printf() in FreeRTOS tasks goes
 * directly through its own USB CDC driver without cross-core mutex issues.
 *
 * Expected USB CDC output:
 *   === Step 16 Stage B: Core 1 FreeRTOS ===
 *   [Stage B] HEARTBEAT 1/5 — core 1 — tick XXXX
 *   ...
 *   [Stage B] HEARTBEAT 5/5 — core 1 — tick XXXX
 *   [Stage B] PASS — FreeRTOS task ran 5 heartbeats on Core 1
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "FreeRTOS.h"
#include "task.h"

#define HEARTBEAT_COUNT  5

/* ---------------------------------------------------------------------------
 * heartbeat_task — the only user task; runs on Core 1 under FreeRTOS.
 * ---------------------------------------------------------------------------*/
static void heartbeat_task(void *pv)
{
    (void)pv;
    printf("[Stage B] Heartbeat task started on core %d\n\n", (int)get_core_num());

    for (int i = 1; i <= HEARTBEAT_COUNT; i++) {
        printf("[Stage B] HEARTBEAT %d/%d — core %d — tick %lu\n",
               i, HEARTBEAT_COUNT,
               (int)get_core_num(),
               (unsigned long)xTaskGetTickCount());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    printf("[Stage B] PASS — FreeRTOS task ran %d heartbeats on Core 1\n",
           HEARTBEAT_COUNT);

    while (true) vTaskDelay(pdMS_TO_TICKS(5000));
}

/* ---------------------------------------------------------------------------
 * core1_entry — executed by Core 1.
 * Core 1 owns stdio and runs FreeRTOS independently.
 * ---------------------------------------------------------------------------*/
static void core1_entry(void)
{
    /* Core 1 initialises USB CDC; Core 0 has nothing to do with stdio. */
    stdio_init_all();
    sleep_ms(2000);   /* wait for USB CDC to enumerate on the host */

    printf("\n=== Step 16 Stage B: Core 1 FreeRTOS ===\n");
    printf("Core 1 owns USB CDC; FreeRTOS single-core on Core 1\n");

    BaseType_t ret = xTaskCreate(
        heartbeat_task, "heartbeat",
        512,                      /* stack depth in words */
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL
    );
    configASSERT(ret == pdPASS);

    /* Start FreeRTOS on Core 1 — never returns. */
    vTaskStartScheduler();

    panic("FreeRTOS scheduler exited unexpectedly");
}

/* ---------------------------------------------------------------------------
 * main — executed by Core 0 (bare-metal launcher only).
 * Wakes Core 1, then yields the processor permanently.
 * ---------------------------------------------------------------------------*/
int main(void)
{
    multicore_launch_core1(core1_entry);

    /* Core 0 has no further role in this test. */
    while (true) {
        tight_loop_contents();
    }
}
