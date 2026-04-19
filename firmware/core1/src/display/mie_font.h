/* mie_font.h — LVGL v9 custom font driver for the MIEF v1 binary format.
 *
 * MIEF v1 (see firmware/mie/tools/gen_font.py) is a compact 1 bpp bitmap
 * font format produced from GNU Unifont. The blob is embedded directly
 * in the Core 1 image via .incbin (see mie_font_blob.S) and parsed at
 * runtime with zero heap allocation — the index table is binary-searched
 * and glyph bitmaps are unpacked on demand into the draw buffer LVGL
 * hands us through get_glyph_bitmap_cb.
 *
 * Two lv_font_t views share the same blob:
 *   - 16 px native (1:1)  — `mie_font_unifont_sm_16()`
 *   - 32 px 2× nearest   — `mie_font_unifont_sm_32()`
 * Use the 32 px variant for titles / banners and the 16 px variant for
 * body text. No extra flash cost for the 2× path — scaling is a
 * per-pixel 2×2 block fill at draw time.
 *
 * Coverage: ASCII + Latin-1 supplement + CJK punctuation + Bopomofo +
 * the full charlist from firmware/mie/data/charlist.txt (~19 K glyphs,
 * ~830 KB). Every glyph is 16 px tall at native scale, variable-width
 * bounding box, baseline 3 px above the cell bottom.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"

/* 16 px native. Idempotent. Returns NULL if the embedded MIEF blob fails
 * header validation. */
const lv_font_t *mie_font_unifont_sm_16(void);

/* 32 px via 2× nearest-neighbor upscaling of the same blob. Glyph edges
 * are blocky by design (the underlying Unifont bitmap is hand-drawn on
 * a 16×16 grid). Idempotent; NULL on blob validation failure. */
const lv_font_t *mie_font_unifont_sm_32(void);
