/* font_test_view.h — MIEF / Unifont rendering smoke test.
 *
 * A landscape (320x240) view that exercises the mie_font driver with a
 * set of labels covering: ASCII, CJK Unified Ideographs (Traditional),
 * Bopomofo with tones, CJK punctuation, mixed ASCII + Chinese. If a
 * glyph renders as empty / square / misaligned, the driver or the
 * generated .bin has a bug — see docs/design-notes/mie-architecture.md
 * §4.1 for the MIEF format and the glyph dsc semantics.
 *
 * Debug-only: included in the build when MOKYA_DEBUG_VIEWS is defined.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

const view_descriptor_t *font_test_view_descriptor(void);
