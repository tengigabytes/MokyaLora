/* messages_view.c — see messages_view.h. */

#include "messages_view.h"

#include <stdio.h>
#include <string.h>

#include "messages_inbox.h"
#include "mie_font.h"

static lv_obj_t *s_header;
static lv_obj_t *s_body;
static uint32_t  s_last_seen_seq;

void messages_view_init(lv_obj_t *panel)
{
    const lv_font_t *f16 = mie_font_unifont_sm_16();

    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);

    s_header = lv_label_create(panel);
    if (f16) lv_obj_set_style_text_font(s_header, f16, 0);
    lv_obj_set_style_text_color(s_header, lv_color_hex(0x00FF80), 0);
    lv_label_set_text(s_header, "(no messages yet)");
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_width(s_header, 312);

    s_body = lv_label_create(panel);
    if (f16) lv_obj_set_style_text_font(s_body, f16, 0);
    lv_obj_set_style_text_color(s_body, lv_color_white(), 0);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_body, 4, 0);
    lv_label_set_text(s_body, "");
    lv_obj_set_pos(s_body, 0, 24);
    lv_obj_set_size(s_body, 312, 240 - 24 - 8);
}

void messages_view_apply(const key_event_t *ev)
{
    (void)ev;
    /* Phase 1 has no scrolling / dismiss / reply yet. */
}

void messages_view_refresh(void)
{
    messages_inbox_snapshot_t snap;
    if (!messages_inbox_take_if_new(s_last_seen_seq, &snap)) {
        return;
    }
    s_last_seen_seq = snap.seq;

    char hdr[64];
    /* Meshtastic node IDs are typically rendered as 0x________ (8 hex). */
    snprintf(hdr, sizeof(hdr),
             "From 0x%08lx  ch%u",
             (unsigned long)snap.from_node_id,
             (unsigned)snap.channel_index);
    lv_label_set_text(s_header, hdr);

    /* Body — copy with explicit null termination since IpcPayloadText
     * carries an explicit length and no terminator. */
    char body[MESSAGES_INBOX_TEXT_MAX + 1];
    uint16_t n = snap.text_len;
    if (n > MESSAGES_INBOX_TEXT_MAX) n = MESSAGES_INBOX_TEXT_MAX;
    if (n > 0) memcpy(body, snap.text, n);
    body[n] = '\0';
    lv_label_set_text(s_body, body);
}
