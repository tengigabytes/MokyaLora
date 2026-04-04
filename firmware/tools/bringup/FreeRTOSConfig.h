/*
 * FreeRTOSConfig.h — MokyaLora Step 16 Stage B bringup test
 *
 * Target: RP2350B, Cortex-M33, non-secure (NTZ).
 * Port:   firmware/core1/freertos-kernel/portable/ThirdParty/
 *         Community-Supported-Ports/GCC/RP2350_ARM_NTZ
 *
 * configNUMBER_OF_CORES = 1: FreeRTOS runs on Core 1 only.
 * Core 0 is a bare-metal launcher (multicore_launch_core1 then loops).
 * This matches the production architecture and avoids SMP complexity.
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
#define configMINIMAL_STACK_SIZE                256
#define configMAX_TASK_NAME_LEN                 16
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TIME_SLICING                  1

/* ── Single-core: FreeRTOS runs on Core 1 only ──────────────────────────── */
/* Core 0 is bare-metal (multicore_launch_core1 launcher).                  */
#define configNUMBER_OF_CORES                   1

/* ── Memory — heap_4 ────────────────────────────────────────────────────── */
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0
#define configTOTAL_HEAP_SIZE                   ( 16 * 1024 )

/* ── Interrupts ─────────────────────────────────────────────────────────── */
/* Only tested value per port README. USB interrupt (priority 0) is above   */
/* this threshold and must NOT call FreeRTOS ISR-safe APIs — it doesn't.    */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16

/* ── Task notifications ─────────────────────────────────────────────────── */
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1

/* ── Synchronisation ────────────────────────────────────────────────────── */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           0
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_QUEUE_SETS                    0

/* ── Timers — required by pico sync interop (xEventGroupSetBitsFromISR) ─── */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            256

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

#endif /* FREERTOS_CONFIG_H */
