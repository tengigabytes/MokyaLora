/* lvgl_glue.h — LVGL v9 runtime bring-up for Core 1.
 *
 * Owns lv_init(), tick source, the LVGL display object, the partial draw
 * buffer, and the lv_timer_handler() task. Once `lvgl_glue_start()` returns
 * pdPASS the display is live and any LVGL API can be called from any task
 * (LVGL's FreeRTOS OSAL serialises access via its global mutex).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "FreeRTOS.h"
#include "task.h"

/* Create the LVGL service task. Must be called after vTaskStartScheduler()
 * is about to run (LVGL OSAL needs the scheduler available before lv_init).
 * Returns pdPASS on success. The task itself runs lv_init(), creates the
 * display object, and then loops on lv_timer_handler(). */
BaseType_t lvgl_glue_start(UBaseType_t priority);
