/* message_detail_view.c — see message_detail_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15  header  "DM detail / !<peer>"
 *   y  16..207 body    multi-line metadata
 *   y 208..223 footer hint (managed by hint_bar)
 *
 * Body fields (inbound shown when applicable, outbound has its own set):
 *   Time  : NNNms ago (boot epoch — Core 1 has no wall clock)
 *   Dir   : RX / TX
 *   PID   : 0xNNN
 *   Ack   : NONE / SENDING / DELIVERED / FAILED  + age if non-zero
 *   Want  : yes/no                          (outbound only)
 *   Hops  : limit/start = X/Y               (inbound only)
 *   SNR   : ±N.N dB                         (inbound only)
 *   RSSI  : -NNN dBm                        (inbound only)
 *   Text  : <truncated ~120 chars + …>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "message_detail_view.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/hint_bar.h"
#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "dm_store.h"

typedef struct {
    lv_obj_t *header;
    lv_obj_t *body;
    uint32_t  peer_node_id;
    uint8_t   msg_offset;
    uint32_t  last_change_seq;
} state_t;

/* PSRAM-resident — single-core access, low frequency. */
static state_t s __attribute__((section(".psram_bss")));

static const char *ack_label(uint8_t ack_state)
{
    switch ((dm_ack_state_t)ack_state) {
        case DM_ACK_NONE:      return "none";
        case DM_ACK_SENDING:   return "sending";
        case DM_ACK_DELIVERED: return "delivered";
        case DM_ACK_FAILED:    return "failed";
        default:               return "?";
    }
}

static void render(void)
{
    char hdr[80];
    snprintf(hdr, sizeof(hdr), "DM detail / !%08lx",
             (unsigned long)s.peer_node_id);
    lv_label_set_text(s.header, hdr);

    dm_msg_t m;
    if (s.peer_node_id == 0u ||
        !dm_store_get_msg(s.peer_node_id, s.msg_offset, &m)) {
        lv_label_set_text(s.body, "(no message)\n\nBACK to return.");
        return;
    }

    char text_clip[140];
    size_t copy = m.text_len < sizeof(text_clip) - 4
                      ? m.text_len : sizeof(text_clip) - 4;
    memcpy(text_clip, m.text, copy);
    if (copy < m.text_len) {
        text_clip[copy] = '.';
        text_clip[copy+1] = '.';
        text_clip[copy+2] = '.';
        text_clip[copy+3] = '\0';
    } else {
        text_clip[copy] = '\0';
    }

    char ack_part[40];
    if (m.ack_epoch != 0u) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        snprintf(ack_part, sizeof(ack_part), "%s (%lums ago)",
                 ack_label(m.ack_state),
                 (unsigned long)(now - m.ack_epoch));
    } else {
        snprintf(ack_part, sizeof(ack_part), "%s", ack_label(m.ack_state));
    }

    char body[480];
    if (m.outbound) {
        snprintf(body, sizeof(body),
                 "Dir   : TX\n"
                 "PID   : 0x%08lx\n"
                 "Sent  : %lums (boot)\n"
                 "Want  : %s\n"
                 "Ack   : %s\n"
                 "\n"
                 "Text  : %s",
                 (unsigned long)m.packet_id,
                 (unsigned long)m.epoch,
                 m.want_ack ? "yes" : "no",
                 ack_part,
                 text_clip);
    } else {
        char snr[16];
        if (m.rx_snr_x4 == INT16_MIN)
            snprintf(snr, sizeof(snr), "--");
        else
            snprintf(snr, sizeof(snr), "%+.2fdB", m.rx_snr_x4 / 4.0);

        char rssi[16];
        if (m.rx_rssi == 0)
            snprintf(rssi, sizeof(rssi), "--");
        else
            snprintf(rssi, sizeof(rssi), "%ddBm", (int)m.rx_rssi);

        char hops[24];
        if (m.hop_limit == 0xFFu && m.hop_start == 0xFFu)
            snprintf(hops, sizeof(hops), "--/--");
        else
            snprintf(hops, sizeof(hops), "%u / %u",
                     (unsigned)m.hop_limit, (unsigned)m.hop_start);

        snprintf(body, sizeof(body),
                 "Dir   : RX\n"
                 "Recv  : %lums (boot)\n"
                 "Hops  : limit/start %s\n"
                 "SNR   : %s\n"
                 "RSSI  : %s\n"
                 "\n"
                 "Text  : %s",
                 (unsigned long)m.epoch,
                 hops,
                 snr, rssi,
                 text_clip);
    }
    lv_label_set_text(s.body, body);
}

static void create(lv_obj_t *panel)
{
    s.last_change_seq = 0u;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = lv_label_create(panel);
    lv_obj_set_pos(s.header, 4, 0);
    lv_obj_set_size(s.header, 320 - 8, 16);
    lv_obj_set_style_text_font(s.header, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.header,
        ui_color(UI_COLOR_ACCENT_FOCUS), 0);
    lv_obj_set_style_pad_all(s.header, 0, 0);
    lv_label_set_text(s.header, "");

    s.body = lv_label_create(panel);
    lv_obj_set_pos(s.body, 4, 18);
    lv_obj_set_size(s.body, 320 - 8, 224 - 18 - 4);
    lv_obj_set_style_text_font(s.body, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.body,
        ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_line_space(s.body, 2, 0);
    lv_obj_set_style_pad_all(s.body, 0, 0);
    lv_label_set_long_mode(s.body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s.body, "");

    render();
    hint_bar_set("", "", "BACK close");
}

static void destroy(void)
{
    s.header = s.body = NULL;
    hint_bar_clear();
}

static void apply(const key_event_t *ev)
{
    /* All view-router-handled keys (BACK, FUNC short) are intercepted
     * by view_router_tick before reaching here. We only need to react
     * to keys we want to consume locally — none currently. */
    (void)ev;
}

static void refresh(void)
{
    if (s.body == NULL) return;
    /* Re-render whenever dm_store mutates so an ack landing while the
     * modal is open updates the display. Cheap (snprintf into a stack
     * buffer + lv_label_set_text). */
    uint32_t cur = dm_store_change_seq();
    if (cur == s.last_change_seq) return;
    s.last_change_seq = cur;
    render();
}

void message_detail_view_set_target(uint32_t peer_node_id, uint8_t msg_offset)
{
    s.peer_node_id = peer_node_id;
    s.msg_offset   = msg_offset;
    /* Force re-render on next create() — the modal panel is destroyed
     * on close (overlay modal cleanup) so this is a safe assumption. */
    s.last_change_seq = 0u;
}

static const view_descriptor_t MESSAGE_DETAIL_DESC = {
    .id      = VIEW_ID_MESSAGE_DETAIL,
    .name    = "msg_detail",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
};

const view_descriptor_t *message_detail_view_descriptor(void)
{
    return &MESSAGE_DETAIL_DESC;
}
