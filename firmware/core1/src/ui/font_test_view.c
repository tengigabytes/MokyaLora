/* font_test_view.c — see font_test_view.h. */

#include "font_test_view.h"

#include "mie_font.h"

/* Split-screen demo: the 32 px 2× title at top, the 16 px native body
 * underneath. Both labels are driven from the same MIEF blob — the
 * scale is chosen per-label via mie_font_unifont_sm_{16,32}(). */

/* 《桃花源記》 陶淵明 */
static const char s_title[] =
    "\xE3\x80\x8A\xE6\xA1\x83\xE8\x8A\xB1\xE6\xBA\x90\xE8\xA8\x98\xE3\x80\x8B "
    "\xE9\x99\xB6\xE6\xB7\xB5\xE6\x98\x8E";

/* Opening three paragraphs of 桃花源記. Pre-wrapping is unnecessary —
 * LVGL LV_LABEL_LONG_WRAP breaks on the label width using box_w/adv_w
 * reported by the MIE font driver. */
static const char s_body[] =
    "\xE6\x99\x89\xE5\xA4\xAA\xE5\x85\x83\xE4\xB8\xAD\xEF\xBC\x8C"
    "\xE6\xAD\xA6\xE9\x99\xB5\xE4\xBA\xBA\xE6\x8D\x95\xE9\xAD\x9A"
    "\xE7\x82\xBA\xE6\xA5\xAD\xE3\x80\x82"
    "\xE7\xB7\xA3\xE6\xBA\xAA\xE8\xA1\x8C\xEF\xBC\x8C"
    "\xE5\xBF\x98\xE8\xB7\xAF\xE4\xB9\x8B\xE9\x81\xA0\xE8\xBF\x91\xE3\x80\x82"
    "\xE5\xBF\xBD\xE9\x80\xA2\xE6\xA1\x83\xE8\x8A\xB1\xE6\x9E\x97\xEF\xBC\x8C"
    "\xE5\xA4\xBE\xE5\xB2\xB8\xE6\x95\xB8\xE7\x99\xBE\xE6\xAD\xA5\xEF\xBC\x8C"
    "\xE4\xB8\xAD\xE7\x84\xA1\xE9\x9B\x9C\xE6\xA8\xB9\xEF\xBC\x8C"
    "\xE8\x8A\xB3\xE8\x8D\x89\xE9\xAE\xAE\xE7\xBE\x8E\xEF\xBC\x8C"
    "\xE8\x90\xBD\xE8\x8B\xB1\xE7\xB9\xBD\xE7\xB4\x9B\xE3\x80\x82"
    "\xE6\xBC\x81\xE4\xBA\xBA\xE7\x94\x9A\xE7\x95\xB0\xE4\xB9\x8B\xE3\x80\x82"
    "\xE5\xBE\xA9\xE5\x89\x8D\xE8\xA1\x8C\xEF\xBC\x8C"
    "\xE6\xAC\xB2\xE7\xAA\xAE\xE5\x85\xB6\xE6\x9E\x97\xE3\x80\x82"
    "\xE6\x9E\x97\xE7\x9B\xA1\xE6\xB0\xB4\xE6\xBA\x90\xEF\xBC\x8C"
    "\xE4\xBE\xBF\xE5\xBE\x97\xE4\xB8\x80\xE5\xB1\xB1\xE3\x80\x82"
    "\xE5\xB1\xB1\xE6\x9C\x89\xE5\xB0\x8F\xE5\x8F\xA3\xEF\xBC\x8C"
    "\xE9\xAB\xA3\xE9\xAB\xB4\xE8\x8B\xA5\xE6\x9C\x89\xE5\x85\x89\xE3\x80\x82"
    "\xE4\xBE\xBF\xE8\x88\x8D\xE8\x88\xB9\xE5\xBE\x9E\xE5\x8F\xA3\xE5\x85\xA5\xE3\x80\x82";

#define TITLE_H_PX  36    /* 32 px font + 4 px breathing room */

/* No widget pointers to track — the title/body labels live as anonymous
 * children of `panel`, and `lv_obj_del(panel)` (called by router after
 * destroy) sweeps them. No persistent state. */

static void create(lv_obj_t *panel)
{
    const lv_font_t *f16 = mie_font_unifont_sm_16();
    const lv_font_t *f32 = mie_font_unifont_sm_32();

    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    /* Title — 32 px 2× nearest, centered. */
    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, s_title);
    if (f32) lv_obj_set_style_text_font(title, f32, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, 320);
    lv_obj_set_pos(title, 0, 0);

    /* Body — 16 px native, wrap to full width under the title. */
    lv_obj_t *body = lv_label_create(panel);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body, s_body);
    if (f16) lv_obj_set_style_text_font(body, f16, 0);
    lv_obj_set_style_text_color(body, lv_color_white(), 0);
    lv_obj_set_style_text_line_space(body, 4, 0);
    lv_obj_set_style_text_letter_space(body, 0, 0);
    lv_obj_set_size(body, 320, 240 - TITLE_H_PX);
    lv_obj_set_pos(body, 0, TITLE_H_PX);
}

static void destroy(void)
{
    /* No file-scope widget pointers; `lv_obj_del(panel)` (router) sweeps
     * children. Nothing to null. */
}

static void apply(const key_event_t *ev)
{
    (void)ev;
}

static const view_descriptor_t FONT_TEST_DESC = {
    .id      = VIEW_ID_FONT_TEST,
    .name    = "font_test",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = NULL,
    .flags   = 0,
    .hints   = { NULL, NULL, NULL },
};

const view_descriptor_t *font_test_view_descriptor(void)
{
    return &FONT_TEST_DESC;
}
