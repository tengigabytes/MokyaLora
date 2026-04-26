/* nodes_view.h — Show one mesh peer at a time, sorted by recency.
 *
 * Header: alias + node id (e.g. "TNGB / 0x58a750ca").
 * Body:   SNR / hops / battery / position lines.
 * Footer: "node N/M" + change-count tracker (so a fresh upsert is
 *         visible without scrolling).
 *
 * UP/DOWN cycles older / newer; DOWN at offset 0 sticks to "newest"
 * the same way messages_view does.
 *
 * Thread model: call all entry points from lvgl_task only.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"
#include "key_event.h"

void nodes_view_init(lv_obj_t *panel);
void nodes_view_apply(const key_event_t *ev);
void nodes_view_refresh(void);
