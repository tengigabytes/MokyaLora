/* mie_font.c — see mie_font.h. */

#include "mie_font.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "mie_font_partition.h"

/* MIEF blob is a standalone flash partition — no longer .incbin'd into
 * the Core 1 image. Font driver reads directly from XIP (cached M0
 * flash alias). See mie_font_partition.h for flash map. The blob's
 * own header carries num_glyphs, which gives us a bounded size for
 * parsing; we don't need a separate _end symbol. */
#define MIE_FONT_BLOB_BASE  ((const uint8_t *)MIE_FONT_PARTITION_ADDR)
#define MIE_FONT_BLOB_MAX   ((uint32_t)MIE_FONT_PARTITION_SIZE)

/* MIEF v1 layout (little-endian):
 *   Header (12 B):
 *     [0..3]   magic 'MIEF'
 *     [4]      version   (expect 1)
 *     [5]      px_height (expect 16)
 *     [6]      bpp       (expect 1)
 *     [7]      flags     (bit0 = RLE; we require 0)
 *     [8..11]  num_glyphs
 *   Index (num_glyphs * 8 B, sorted ascending by codepoint):
 *     [+0]     codepoint  uint32
 *     [+4]     data_offs  uint32   (byte offset into glyph-data section)
 *   Glyph data (variable):
 *     [+0]     adv_w      uint8
 *     [+1]     box_w      uint8
 *     [+2]     box_h      uint8
 *     [+3]     ofs_x      int8
 *     [+4]     ofs_y      int8
 *     [+5..]   bitmap     1 bpp MSB-first, box_h * ceil(box_w/8) bytes
 *                         (omitted when box_w == 0 or box_h == 0)
 */

#define MIEF_HEADER_SIZE       12
#define MIEF_INDEX_STRIDE      8
#define MIEF_GLYPH_HDR_SIZE    5

typedef struct {
    const uint8_t *index;        /* 8-byte entries, sorted by codepoint    */
    const uint8_t *glyph_data;   /* base for data_offs                      */
    uint32_t       num_glyphs;
    uint8_t        px_height;
    uint8_t        bpp;
    bool           ready;
} mief_blob_t;

/* One shared parsed blob, two font views selecting the integer scale. */
static mief_blob_t s_blob;

typedef struct {
    const mief_blob_t *blob;
    uint8_t            scale;    /* 1 = native 16 px, 2 = 32 px 2x nearest */
} mief_view_t;

static const mief_view_t s_view_1x = { &s_blob, 1 };
static const mief_view_t s_view_2x = { &s_blob, 2 };
static lv_font_t         s_font_1x;
static lv_font_t         s_font_2x;
static bool              s_fonts_bound;

/* Little-endian unaligned u32 read — Cortex-M33 tolerates misalignment
 * for normal loads, but being explicit keeps this portable. */
static inline uint32_t mief_read_u32(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint32_t mief_find_offset(const mief_blob_t *blob, uint32_t cp)
{
    uint32_t lo = 0;
    uint32_t hi = blob->num_glyphs;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        const uint8_t *e = blob->index + mid * MIEF_INDEX_STRIDE;
        uint32_t cp_mid = mief_read_u32(e);
        if (cp_mid == cp) return mief_read_u32(e + 4);
        if (cp_mid < cp) lo = mid + 1;
        else             hi = mid;
    }
    return UINT32_MAX;
}

static bool mie_font_get_glyph_dsc(const lv_font_t *font,
                                   lv_font_glyph_dsc_t *dsc,
                                   uint32_t letter,
                                   uint32_t letter_next)
{
    (void)letter_next;
    const mief_view_t *view = (const mief_view_t *)font->dsc;
    const mief_blob_t *blob = view->blob;
    uint32_t off = mief_find_offset(blob, letter);
    if (off == UINT32_MAX) return false;

    const uint8_t *g = blob->glyph_data + off;
    uint8_t s = view->scale;
    dsc->adv_w          = (uint16_t)g[0] * s;
    dsc->box_w          = (uint16_t)g[1] * s;
    dsc->box_h          = (uint16_t)g[2] * s;
    dsc->ofs_x          = (int16_t)(int8_t)g[3] * s;
    dsc->ofs_y          = (int16_t)(int8_t)g[4] * s;
    dsc->format         = (uint8_t)LV_FONT_GLYPH_FORMAT_A1;
    dsc->is_placeholder = 0;
    dsc->gid.index      = off;   /* cache for get_glyph_bitmap */
    return true;
}

/* Unpack MIEF 1 bpp MSB-first → A8 (0x00 / 0xFF) into the LVGL draw
 * buffer. At scale=1 each source bit maps to one A8 pixel; at scale=2
 * each source bit fills a 2×2 block in the destination.
 *
 * lv_draw_label.c allocates the draw_buf with LV_COLOR_FORMAT_A8 sized
 * to the (scaled) box_w × box_h we returned in get_glyph_dsc, stride
 * = lv_draw_buf_width_to_stride(box_w, A8), so we write row-by-row
 * using draw_buf->header.stride.
 */
static const void *mie_font_get_glyph_bitmap(lv_font_glyph_dsc_t *g_dsc,
                                             lv_draw_buf_t *draw_buf)
{
    const mief_view_t *view = (const mief_view_t *)g_dsc->resolved_font->dsc;
    const mief_blob_t *blob = view->blob;
    uint8_t            s    = view->scale;

    const uint8_t *g = blob->glyph_data + g_dsc->gid.index;
    uint8_t src_w = g[1];
    uint8_t src_h = g[2];
    if (src_w == 0 || src_h == 0) return NULL;

    const uint8_t *bitmap_in = g + MIEF_GLYPH_HDR_SIZE;
    uint32_t row_bytes = ((uint32_t)src_w + 7u) >> 3;
    uint8_t *out = draw_buf->data;
    uint32_t stride = draw_buf->header.stride;

    if (s == 1) {
        for (uint32_t y = 0; y < src_h; ++y) {
            const uint8_t *row_in = bitmap_in + y * row_bytes;
            uint8_t *row_out = out + y * stride;
            for (uint32_t x = 0; x < src_w; ++x) {
                uint8_t bit = row_in[x >> 3] & (uint8_t)(0x80u >> (x & 7u));
                row_out[x] = bit ? 0xFFu : 0x00u;
            }
        }
    } else {
        /* 2× nearest-neighbor: each source pixel → 2×2 output block. */
        for (uint32_t y = 0; y < src_h; ++y) {
            const uint8_t *row_in  = bitmap_in + y * row_bytes;
            uint8_t       *row_a   = out + (y * 2u)       * stride;
            uint8_t       *row_b   = out + (y * 2u + 1u)  * stride;
            for (uint32_t x = 0; x < src_w; ++x) {
                uint8_t bit = row_in[x >> 3] & (uint8_t)(0x80u >> (x & 7u));
                uint8_t v   = bit ? 0xFFu : 0x00u;
                uint32_t dx = x * 2u;
                row_a[dx]     = v;
                row_a[dx + 1] = v;
                row_b[dx]     = v;
                row_b[dx + 1] = v;
            }
        }
    }
    return draw_buf;
}

static void mie_font_release_glyph(const lv_font_t *font,
                                   lv_font_glyph_dsc_t *g_dsc)
{
    (void)font;
    (void)g_dsc;
}

static bool mie_font_parse_blob(const uint8_t *blob, uint32_t blob_size)
{
    if (blob_size < MIEF_HEADER_SIZE)                 return false;
    if (memcmp(blob, "MIEF", 4) != 0)                 return false;
    if (blob[4] != 1)                                 return false;   /* version */
    if (blob[5] != 16)                                return false;   /* px_height */
    if (blob[6] != 1)                                 return false;   /* bpp */
    if ((blob[7] & 0x01u) != 0)                       return false;   /* RLE unsupported */

    uint32_t num_glyphs  = mief_read_u32(blob + 8);
    uint32_t index_bytes = num_glyphs * MIEF_INDEX_STRIDE;
    if ((uint64_t)MIEF_HEADER_SIZE + index_bytes > blob_size) return false;

    s_blob.index      = blob + MIEF_HEADER_SIZE;
    s_blob.glyph_data = blob + MIEF_HEADER_SIZE + index_bytes;
    s_blob.num_glyphs = num_glyphs;
    s_blob.px_height  = blob[5];
    s_blob.bpp        = blob[6];
    s_blob.ready      = true;
    return true;
}

static void mie_font_bind(lv_font_t *dst, const mief_view_t *view)
{
    uint8_t s = view->scale;
    dst->get_glyph_dsc       = mie_font_get_glyph_dsc;
    dst->get_glyph_bitmap    = mie_font_get_glyph_bitmap;
    dst->release_glyph       = mie_font_release_glyph;
    dst->line_height         = (int32_t)s_blob.px_height * s;   /* 16 or 32 */
    dst->base_line           = 3 * s;
    dst->subpx               = LV_FONT_SUBPX_NONE;
    dst->kerning             = LV_FONT_KERNING_NONE;
    dst->underline_position  = -1 * s;
    dst->underline_thickness = 1 * s;
    dst->dsc                 = view;
    dst->fallback            = NULL;
    dst->user_data           = NULL;
}

static bool mie_font_ensure_ready(void)
{
    if (s_fonts_bound) return true;
    if (!s_blob.ready) {
        if (!mie_font_parse_blob(MIE_FONT_BLOB_BASE, MIE_FONT_BLOB_MAX)) return false;
    }
    mie_font_bind(&s_font_1x, &s_view_1x);
    mie_font_bind(&s_font_2x, &s_view_2x);
    s_fonts_bound = true;
    return true;
}

const lv_font_t *mie_font_unifont_sm_16(void)
{
    return mie_font_ensure_ready() ? &s_font_1x : NULL;
}

const lv_font_t *mie_font_unifont_sm_32(void)
{
    return mie_font_ensure_ready() ? &s_font_2x : NULL;
}
