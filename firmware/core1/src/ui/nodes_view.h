/* nodes_view.h — Show one mesh peer at a time, sorted by recency.
 *
 * Header: alias + node id (e.g. "TNGB / 0x58a750ca").
 * Body:   SNR / hops / battery / position lines.
 * Footer: "node N/M" + change-count tracker.
 *
 * UP/DOWN cycles older / newer; DOWN at offset 0 sticks to "newest".
 * Scroll state persists across destroy/recreate via the view-owned
 * static struct in nodes_view.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

const view_descriptor_t *nodes_view_descriptor(void);
