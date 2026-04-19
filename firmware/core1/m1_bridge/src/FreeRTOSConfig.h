/*
 * FreeRTOSConfig.h — MokyaLora Phase 2 M1.1-B Core 1 m1_bridge
 *
 * Target: RP2350B Cortex-M33 NTZ (non-secure, no TrustZone)
 * Port:   firmware/core1/freertos-kernel/portable/ThirdParty/
 *         Community-Supported-Ports/GCC/RP2350_ARM_NTZ
 *
 * Single-core FreeRTOS on Core 1 only (Core 0 runs Meshtastic bare-metal
 * Arduino-Pico + single-core FreeRTOS in a separate image).
 *
 * Heap budget:
 *   Core 1 owns the 312 KB region at 0x2002C000..0x20078000 (see
 *   docs/design-notes/firmware-architecture.md §2.2 "Core 1 SRAM
 *   breakdown"). FreeRTOS Heap4 is fixed at 32 KB by the architecture
 *   spec — it backs all task stacks (heap_4 dynamic allocation), TCBs,
 *   timer queue, and any runtime xQueueCreate / xSemaphoreCreate. The
 *   remaining 280 KB is split between the LVGL framebuffer (150 KB),
 *   LVGL internal heap (48 KB), driver/MIE/app state, main stack, and
 *   margin per the same table.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ── Cortex-M33 non-secure (NTZ) ─────────────────────────────────────────── */
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

/* ── Single-core only ───────────────────────────────────────────────────── */
#define configNUMBER_OF_CORES                   1

/* ── SDK interop disabled — Core 1 owns its own hardware ─────────────────── */
#define configSUPPORT_PICO_SYNC_INTEROP         0
#define configSUPPORT_PICO_TIME_INTEROP         0

/* ── Memory — Heap4 ─────────────────────────────────────────────────────── */
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0
#define configTOTAL_HEAP_SIZE                   ( 48 * 1024 )

/* ── Interrupt priorities ────────────────────────────────────────────────── */
/* Per the RP2350_ARM_NTZ port README, configMAX_SYSCALL_INTERRUPT_PRIORITY
 * is the only tested value. USBCTRL IRQ must run ABOVE this threshold
 * (priority <16), and TinyUSB's dcd callback must not call FreeRTOS
 * xxxFromISR() APIs — we use task notifications driven from bridge_task,
 * not from the ISR, so this is safe. */
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

/* ── Timers ─────────────────────────────────────────────────────────────── */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            512

/* ── Tracing / stats ────────────────────────────────────────────────────── */
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configGENERATE_RUN_TIME_STATS           0

/* ── Assert ──────────────────────────────────────────────────────────────── */
/* panic() comes from pico_runtime. FreeRTOS asserts drop into it with file
 * and line so we can catch them over SWD. */
#define configASSERT( x )   \
    do { if( !( x ) ) { panic( "FreeRTOS assert %s:%d", __FILE__, __LINE__ ); } } while( 0 )

/* ── INCLUDE_* ───────────────────────────────────────────────────────────── */
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_uxTaskPriorityGet               0
#define INCLUDE_eTaskGetState                   0
#define INCLUDE_xTaskAbortDelay                 0
#define INCLUDE_uxTaskGetStackHighWaterMark     0
#define INCLUDE_xSemaphoreGetMutexHolder        1

#endif /* FREERTOS_CONFIG_H */
