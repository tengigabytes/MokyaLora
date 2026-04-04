/*
 * FreeRTOSConfig.h — MokyaLora bringup: FreeRTOS + USB CDC on Core 0
 *
 * Target: RP2350B, Cortex-M33, non-secure (NTZ).
 * Port:   firmware/core1/freertos-kernel/portable/ThirdParty/
 *         Community-Supported-Ports/GCC/RP2350_ARM_NTZ
 *
 * Based on pico-examples/freertos/FreeRTOSConfig_examples_common.h.
 * configNUMBER_OF_CORES = 1: single-core FreeRTOS on Core 1.
 * SYNC/TIME interop disabled — Core 0 is bare-metal (no scheduler).
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ── ARM Cortex-M33 (RP2350, non-secure, no TrustZone) ──────────────────── */
/* Required by portmacrocommon.h — values fixed for NTZ.                    */
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1
#define configENABLE_FPU                        1

/* ── Scheduler ──────────────────────────────────────────────────────────── */
#define configUSE_PREEMPTION                    1
#define configUSE_IDLE_HOOK                     0
#define configUSE_PASSIVE_IDLE_HOOK             0
#define configUSE_TICK_HOOK                     0
#define configCPU_CLOCK_HZ                      150000000UL
#define configTICK_RATE_HZ                      1000
#define configMAX_PRIORITIES                    5
#define configMINIMAL_STACK_SIZE                512
#define configMAX_TASK_NAME_LEN                 16
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TIME_SLICING                  1

/* ── Single-core: FreeRTOS on Core 1 only ──────────────────────────────── */
/* Core 0 is bare-metal (launches Core 1, then idles).                      */
/* Core 1 owns FreeRTOS scheduler + manually-driven TinyUSB.                */
#define configNUMBER_OF_CORES                   1

/* ── Disable SDK interop — Core 0 has no scheduler ────────────────────── */
/* With interop enabled, any SDK call on Core 0 (sleep_ms, mutex) would     */
/* be redirected to FreeRTOS primitives on a core with no scheduler → crash.*/
#define configSUPPORT_PICO_SYNC_INTEROP         0
#define configSUPPORT_PICO_TIME_INTEROP         0

/* ── Memory — heap_4 ────────────────────────────────────────────────────── */
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0
#define configTOTAL_HEAP_SIZE                   ( 64 * 1024 )

/* ── Interrupts ─────────────────────────────────────────────────────────── */
/* Only tested value per port README. USB interrupt (priority 0) is above   */
/* this threshold and must NOT call FreeRTOS ISR-safe APIs — it doesn't.    */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16

/* ── Task notifications ─────────────────────────────────────────────────── */
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1

/* ── Synchronisation ────────────────────────────────────────────────────── */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           0
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_QUEUE_SETS                    0

/* ── Timers — required by pico sync interop (xEventGroupSetBitsFromISR) ─── */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            1024

/* ── Tracing / stats (disabled for bringup) ─────────────────────────────── */
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configGENERATE_RUN_TIME_STATS           0

/* ── Assert ──────────────────────────────────────────────────────────────── */
#define configASSERT( x )   \
    do { if( !( x ) ) { panic( "FreeRTOS assert %s:%d", __FILE__, __LINE__ ); } } while( 0 )

/* ── INCLUDE_* ───────────────────────────────────────────────────────────── */
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    0
#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_uxTaskPriorityGet               0
#define INCLUDE_eTaskGetState                   0
#define INCLUDE_xTaskAbortDelay                 0
#define INCLUDE_uxTaskGetStackHighWaterMark     0
#define INCLUDE_xSemaphoreGetMutexHolder       1

#endif /* FREERTOS_CONFIG_H */
