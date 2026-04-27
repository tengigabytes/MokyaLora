/* messages_view.c — see messages_view.h. */

#include "messages_view.h"

#include <stdio.h>
#include <string.h>

#ifdef MOKYA_PHONEAPI_CASCADE
#include "phoneapi_cache.h"
typedef phoneapi_text_msg_t  msgs_entry_t;
#define msgs_take_at_offset(o, e)  phoneapi_msgs_take_at_offset((o), (e))
#define msgs_count()               phoneapi_msgs_count()
#define msgs_latest_seq()          phoneapi_msgs_latest_seq()
#define MSGS_TEXT_MAX              PHONEAPI_MSG_TEXT_MAX
#else
#include "messages_inbox.h"
typedef messages_inbox_entry_t msgs_entry_t;
#define msgs_take_at_offset(o, e)  messages_inbox_take_at_offset((o), (e))
#define msgs_count()               messages_inbox_count()
#define msgs_latest_seq()          messages_inbox_latest_seq()
#define MSGS_TEXT_MAX              MESSAGES_INBOX_TEXT_MAX
#endif

#include "messages_send.h"
#include "messages_tx_status.h"
#include "mie_font.h"
#include "mie/keycode.h"
#include "ime_task.h"

/* Three-line layout:
 *   header  (green)   — "From 0xNNNNNNNN  ch%d"   at y=0
 *   body    (white)   — wrapped UTF-8             at y=24, fills middle
 *   footer  (gray)    — "msg N/M"                 at bottom
 */
static lv_obj_t *s_panel;
static lv_obj_t *s_header;
static lv_obj_t *s_body;
static lv_obj_t *s_footer;

/* Browse offset — 0 = newest message; increments mean older.
 * `s_sticky_to_newest` keeps offset pinned at 0 so a fresh arrival
 * automatically displays. The user breaks stickiness by pressing UP. */
static uint32_t s_offset;
static bool     s_sticky_to_newest = true;

/* Latest seq we've already rendered for the current offset, so we
 * skip redundant lv_label_set_text calls. */
static uint32_t s_displayed_seq;

/* Outgoing-send tracking for footer overlay. While the user is waiting
 * on TX_ACK feedback for their last send, the footer shows status
 * instead of the "msg N/M" navigation indicator. UP/DOWN clears the
 * pending state so navigation always wins back the footer. */
static bool     s_pending_tx_active;
static uint8_t  s_pending_tx_seq;
static uint32_t s_pending_tx_target;
static uint32_t s_last_tx_change_seq;

static void render_offset(uint32_t offset)
{
    msgs_entry_t entry;
    if (!msgs_take_at_offset(offset, &entry)) {
        lv_label_set_text(s_header, "(no messages yet)");
        lv_label_set_text(s_body, "");
        lv_label_set_text(s_footer, "msg 0/0");
        s_displayed_seq = 0;
        return;
    }

    char hdr[64];
    snprintf(hdr, sizeof(hdr),
             "From 0x%08lx  ch%u",
             (unsigned long)entry.from_node_id,
             (unsigned)entry.channel_index);
    lv_label_set_text(s_header, hdr);

    char body[MSGS_TEXT_MAX + 1];
    uint16_t n = entry.text_len;
    if (n > MSGS_TEXT_MAX) n = MSGS_TEXT_MAX;
    if (n > 0) memcpy(body, entry.text, n);
    body[n] = '\0';
    lv_label_set_text(s_body, body);

    /* Footer: "msg N/M" where N is 1-indexed position from oldest, M is
     * total count. So offset 0 (newest) = "msg M/M". */
    uint32_t total = msgs_count();
    uint32_t shown = (total > offset) ? (total - offset) : 0u;
    char foot[32];
    snprintf(foot, sizeof(foot), "msg %lu/%lu",
             (unsigned long)shown,
             (unsigned long)total);
    lv_label_set_text(s_footer, foot);

    s_displayed_seq = entry.seq;
}

static void create(lv_obj_t *panel)
{
    const lv_font_t *f16 = mie_font_unifont_sm_16();
    s_panel = panel;

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
    lv_obj_set_size(s_body, 312, 240 - 24 - 24);

    s_footer = lv_label_create(panel);
    if (f16) lv_obj_set_style_text_font(s_footer, f16, 0);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(0x808080), 0);
    lv_label_set_text(s_footer, "msg 0/0");
    lv_obj_set_pos(s_footer, 0, 240 - 24);
    lv_obj_set_width(s_footer, 312);
}

static void apply(const key_event_t *ev)
{
    if (!ev || !ev->pressed) return;

    /* OK on this view = reply to the sender of the currently-displayed
     * message with whatever the IME has committed. The reply is a DM
     * (to_node_id = sender's id, channel = original channel) so it
     * doesn't broadcast on the mesh. Empty IME buffer or empty inbox
     * is a no-op with a footer hint. */
    if (ev->keycode == MOKYA_KEY_OK) {
        msgs_entry_t target;
        if (!msgs_take_at_offset(s_offset, &target)) {
            lv_label_set_text(s_footer, "(no recipient — wait for a msg)");
            return;
        }

        uint8_t  send_buf[MESSAGES_SEND_TEXT_MAX];
        uint16_t send_len = 0;

        if (ime_view_lock(pdMS_TO_TICKS(20))) {
            int tlen = 0;
            const char *t = ime_view_text(&tlen, NULL);
            if (tlen < 0) tlen = 0;
            if (tlen > (int)sizeof(send_buf)) tlen = (int)sizeof(send_buf);
            if (t != NULL && tlen > 0) {
                memcpy(send_buf, t, (size_t)tlen);
                send_len = (uint16_t)tlen;
            }
            ime_view_unlock();
        }

        if (send_len == 0) {
            lv_label_set_text(s_footer, "(IME empty — type first)");
            return;
        }

        uint8_t sent_seq = 0u;
        bool ok = messages_send_text(target.from_node_id,
                                     target.channel_index,
                                     /*want_ack=*/true,
                                     send_buf,
                                     send_len,
                                     &sent_seq);
        if (ok) {
            ime_view_clear_text();
            s_pending_tx_active = true;
            s_pending_tx_seq    = sent_seq;
            s_pending_tx_target = target.from_node_id;
            char foot[48];
            snprintf(foot, sizeof(foot),
                     "sending → 0x%08lx",
                     (unsigned long)target.from_node_id);
            lv_label_set_text(s_footer, foot);
        } else {
            lv_label_set_text(s_footer, "send failed (ring full)");
        }
        return;
    }

    uint32_t total = msgs_count();
    if (total == 0) return;

    if (ev->keycode == MOKYA_KEY_UP) {
        /* UP = older. Cap at the oldest available (offset = total-1). */
        if (s_offset + 1u < total) {
            s_offset++;
        }
        s_sticky_to_newest = false;
        s_pending_tx_active = false;   /* nav wins back the footer */
        render_offset(s_offset);
    } else if (ev->keycode == MOKYA_KEY_DOWN) {
        /* DOWN = newer. Reaching offset 0 re-arms sticky-to-newest. */
        if (s_offset > 0u) {
            s_offset--;
        }
        if (s_offset == 0u) {
            s_sticky_to_newest = true;
        }
        s_pending_tx_active = false;
        render_offset(s_offset);
    }
}

/* If a TX_ACK has arrived for our most recent send, repaint the footer
 * with the new status. delivered/failed both stay until the user
 * navigates (UP/DOWN) — gives them time to read the result.
 * Returns true when the footer was overlaid (caller should skip the
 * inbox-driven footer text for this tick). */
static bool maybe_render_tx_status_footer(void)
{
    if (!s_pending_tx_active) return false;

    messages_tx_status_t tx;
    messages_tx_status_get(&tx);
    if (tx.change_seq == s_last_tx_change_seq) {
        /* Nothing new since the last refresh; keep whatever the footer
         * currently shows ("sending → ..." or a previously rendered
         * delivered/failed). */
        return true;
    }
    s_last_tx_change_seq = tx.change_seq;

    if (tx.ipc_seq != s_pending_tx_seq) {
        /* TX status is for a different send (shouldn't happen with our
         * single-message-at-a-time UX but be defensive). */
        return true;
    }

    char foot[64];
    switch (tx.result) {
    case MESSAGES_TX_RESULT_SENDING:
        snprintf(foot, sizeof(foot),
                 "sending → 0x%08lx",
                 (unsigned long)s_pending_tx_target);
        break;
    case MESSAGES_TX_RESULT_DELIVERED:
        snprintf(foot, sizeof(foot),
                 "delivered → 0x%08lx",
                 (unsigned long)s_pending_tx_target);
        break;
    case MESSAGES_TX_RESULT_FAILED:
        snprintf(foot, sizeof(foot),
                 "failed (err %u) → 0x%08lx",
                 (unsigned)tx.error_reason,
                 (unsigned long)s_pending_tx_target);
        break;
    default:
        return true;
    }
    lv_label_set_text(s_footer, foot);
    return true;
}

static void refresh(void)
{
    /* Active-only refresh: router no longer calls us when hidden. */
    if (s_panel == NULL) return;

    uint32_t latest = msgs_latest_seq();
    if (latest == 0u) {
        if (s_displayed_seq != 0u) {
            render_offset(0);
        }
        (void)maybe_render_tx_status_footer();
        return;
    }

    /* Sticky-to-newest: a fresh arrival re-renders at offset 0 even if
     * the user is currently browsing — but only when sticky is set.
     * If they pressed UP they get to stay at their offset until they
     * either DOWN-back-to-0 or switch views. */
    if (s_sticky_to_newest) {
        s_offset = 0u;
    }

    /* Re-render if our cached seq for the current offset is stale.
     * This catches both "new message arrived at sticky offset 0" and
     * "ring buffer eviction shifted what offset N points at". */
    msgs_entry_t peek;
    if (msgs_take_at_offset(s_offset, &peek)) {
        if (peek.seq != s_displayed_seq) {
            render_offset(s_offset);
            /* render_offset just rewrote the footer to "msg N/M".
             * If a send is in-flight, replay the TX status overlay
             * (next refresh will pick up any newer change_seq too). */
        }
    }

    (void)maybe_render_tx_status_footer();
}

static void destroy(void)
{
    s_panel = s_header = s_body = s_footer = NULL;
    s_displayed_seq = 0u;          /* force first render after recreate */
    s_last_tx_change_seq = 0u;     /* re-overlay TX status on next refresh */
    /* s_offset / s_sticky_to_newest / s_pending_tx_* persist across
     * destroy via static .bss — preserves scroll + in-flight TX UX. */
}

static const view_descriptor_t MESSAGES_DESC = {
    .id      = VIEW_ID_MESSAGES,
    .name    = "messages",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
};

const view_descriptor_t *messages_view_descriptor(void)
{
    return &MESSAGES_DESC;
}
