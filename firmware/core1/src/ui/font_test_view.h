/* font_test_view.h — MIEF / Unifont rendering smoke test.
 *
 * A landscape (320x240) view that exercises the mie_font driver with a
 * set of labels covering: ASCII, CJK Unified Ideographs (Traditional),
 * Bopomofo with tones, CJK punctuation, mixed ASCII + Chinese. If a
 * glyph renders as empty / square / misaligned, the driver or the
 * generated .bin has a bug — see docs/design-notes/mie-architecture.md
 * §4.1 for the MIEF format and the glyph dsc semantics.
 *
 * Thread model: call all three entry points from lvgl_task only.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"
#include "key_event.h"

void font_test_view_init(lv_obj_t *panel);
void font_test_view_apply(const key_event_t *ev);
