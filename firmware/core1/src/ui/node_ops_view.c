/* node_ops_view.c — see node_ops_view.h.
 *
 * 7 spec entries. Today only "DM" is wired (jumps to A-2 conversation
 * view with the node as active peer); the rest hint "TBD" until each
 * underlying mechanism (alias / favorite / ignore / traceroute /
 * request position / remote admin) gets its IPC plumbing.
 *
 * Layout: 16 px header + 7 × 24 px rows + footer hint = 16 + 168 + 24
 * = 208 px (fits the 224 px panel area).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "node_ops_view.h"

#include <stdio.h>
#include <string.h>

#include "phoneapi_cache.h"
#include "global/ui_theme.h"
#include "global/hint_bar.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "nodes_view.h"
#include "chat_list_view.h"
#include "node_alias.h"
#include "ime_task.h"
#include "phoneapi_encode.h"
#include "mokya_trace.h"

#define ROW_H        24
#define HEADER_H     16
#define MAX_ENTRIES   7

typedef enum {
    OP_DM            = 0,
    OP_ALIAS         = 1,
    OP_FAVORITE      = 2,
    OP_IGNORE        = 3,
    OP_TRACEROUTE    = 4,
    OP_REQUEST_POS   = 5,
    OP_REMOTE_ADMIN  = 6,
} op_id_t;

/* Static portion of each label. FAVORITE / IGNORE rows append a
 * dynamic "[on]" / "[off]" suffix in render() based on the cached
 * NodeInfo bits. */
static const char *const s_op_labels[MAX_ENTRIES] = {
    "DM (open conversation)",
    "Set alias",
    "Favorite",
    "Ignore",
    "Traceroute (send)",
    "Request position",
    "Remote admin       (TBD)",
};

/* Which ops are wired today. Placeholders render dimmed; OK on them
 * still flashes a TBD hint via the header label. */
static bool op_is_active(uint8_t i)
{
    switch ((op_id_t)i) {
        case OP_DM:
        case OP_ALIAS:
        case OP_FAVORITE:
        case OP_IGNORE:
        case OP_TRACEROUTE:
        case OP_REQUEST_POS:
            return true;
        default:
            return false;
    }
}

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[MAX_ENTRIES];
    uint8_t   cursor;
    uint32_t  active_num;
    uint32_t  last_change_seq;     /* re-render gate — favorite/ignore
                                    * labels follow phoneapi_cache state
                                    * which mutates after self-admin */
} ops_t;

/* PSRAM-resident (single-core access, low frequency) — saves ~40 B
 * of the tight Core 1 SRAM budget. */
static ops_t s __attribute__((section(".psram_bss")));

static lv_obj_t *make_row(lv_obj_t *parent, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, 4, y);
    lv_obj_set_size(l, 320 - 8, ROW_H);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

static void render(void)
{
    char hdr[80];
    phoneapi_node_t e;
    bool have_node = false;
    if (s.active_num != 0u &&
        phoneapi_cache_get_node_by_id(s.active_num, &e)) {
        have_node = true;
        char nm[24];
        node_alias_format_display(s.active_num, e.short_name,
                                  nm, sizeof(nm));
        snprintf(hdr, sizeof(hdr), "Ops: %s !%08lx",
                 nm, (unsigned long)s.active_num);
    } else if (s.active_num != 0u) {
        char nm[24];
        node_alias_format_display(s.active_num, NULL, nm, sizeof(nm));
        snprintf(hdr, sizeof(hdr), "Ops: %s !%08lx",
                 nm, (unsigned long)s.active_num);
    } else {
        snprintf(hdr, sizeof(hdr), "Ops: (no node)");
    }
    lv_label_set_text(s.header, hdr);

    char buf[80];
    for (uint8_t i = 0; i < MAX_ENTRIES; ++i) {
        const char *suffix = "";
        /* FAVORITE / IGNORE show current state so OK shows a "toggle"
         * intent rather than a permanent action. "(?)" when the cache
         * has no entry for the active node — happens briefly between
         * config-replays after a self-admin write. */
        if (i == OP_FAVORITE) {
            suffix = have_node ? (e.is_favorite ? "  [on]" : "  [off]")
                               : "  [?]";
        } else if (i == OP_IGNORE) {
            suffix = have_node ? (e.is_unmessagable ? "  [on]" : "  [off]")
                                : "  [?]";
        }
        snprintf(buf, sizeof(buf), "%s %s%s",
                 i == s.cursor ? ">" : " ", s_op_labels[i], suffix);
        lv_label_set_text(s.rows[i], buf);
        bool placeholder = !op_is_active(i);
        lv_obj_set_style_text_color(s.rows[i],
            i == s.cursor && !placeholder
                ? ui_color(UI_COLOR_ACCENT_FOCUS)
                : (placeholder
                    ? ui_color(UI_COLOR_TEXT_SECONDARY)
                    : ui_color(UI_COLOR_TEXT_PRIMARY)), 0);
    }
}

/* IME callback for the alias rename flow. Writes the committed text
 * into the local alias store; an empty commit clears the alias. */
static void on_alias_done(bool committed, const char *utf8,
                          uint16_t byte_len, void *ctx)
{
    (void)ctx;
    if (!committed) return;
    /* Use the active node id we stashed when this op was launched.
     * If the user navigated elsewhere mid-modal, s.active_num stays
     * pinned because conversation/launcher don't touch it. */
    if (s.active_num == 0u) return;
    node_alias_set(s.active_num, utf8, byte_len);
    /* Render runs again automatically when view_router restores us
     * as active after the IME modal closes — header will pick up
     * the new alias via phoneapi_cache lookup + node_alias_lookup
     * fallback chain. */
}

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));
    s.active_num = nodes_view_get_active_node();

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = lv_label_create(panel);
    lv_obj_set_pos(s.header, 4, 0);
    lv_obj_set_size(s.header, 320 - 8, HEADER_H);
    lv_obj_set_style_text_font(s.header, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.header,
        ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_all(s.header, 0, 0);
    lv_label_set_text(s.header, "");

    for (uint8_t i = 0; i < MAX_ENTRIES; ++i) {
        s.rows[i] = make_row(panel, HEADER_H + i * ROW_H);
    }

    render();
    hint_bar_set("up/dn pick", "OK do", "BACK detail");
}

static void destroy(void)
{
    s.header = NULL;
    for (uint8_t i = 0; i < MAX_ENTRIES; ++i) s.rows[i] = NULL;
    hint_bar_clear();
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s.cursor > 0u) { s.cursor--; render(); }
            break;
        case MOKYA_KEY_DOWN:
            if (s.cursor + 1u < MAX_ENTRIES) { s.cursor++; render(); }
            break;
        case MOKYA_KEY_OK:
            switch ((op_id_t)s.cursor) {
                case OP_DM:
                    if (s.active_num != 0u) {
                        chat_list_set_active_peer(s.active_num);
                        view_router_navigate(VIEW_ID_MESSAGES_CHAT);
                    }
                    break;
                case OP_ALIAS: {
                    if (s.active_num == 0u) break;
                    if (ime_request_text_active()) break;
                    /* Pre-fill with the existing alias if any so the
                     * user edits rather than retypes. */
                    const char *cur = node_alias_lookup(s.active_num);
                    ime_text_request_t req = {
                        .prompt    = "Alias",
                        .initial   = cur,
                        .max_bytes = (uint16_t)NODE_ALIAS_MAX_LEN,
                        .mode_hint = IME_TEXT_MODE_DEFAULT,
                        .flags     = IME_TEXT_FLAG_NONE,
                        .layout    = IME_TEXT_LAYOUT_FULLSCREEN,
                        /* draft_id keyed by node so per-peer drafts
                         * survive cancel-and-resume. Use an arbitrary
                         * tag bit to avoid collision with the
                         * conversation-compose draft namespace
                         * (which uses raw node_num). */
                        .draft_id  = s.active_num | 0x80000000u,
                    };
                    (void)ime_request_text(&req, on_alias_done, NULL);
                    break;
                }
                case OP_FAVORITE: {
                    if (s.active_num == 0u) break;
                    /* Decide direction off the cached bit so a stale
                     * cache (no entry) defaults to "set" — safer than
                     * defaulting to clear. */
                    phoneapi_node_t e;
                    bool have = phoneapi_cache_get_node_by_id(s.active_num, &e);
                    bool currently_set = have && e.is_favorite;
                    TRACE("nodops", "fav_fire",
                          "active=%lu have=%u cur=%u",
                          (unsigned long)s.active_num,
                          (unsigned)have, (unsigned)currently_set);
                    uint32_t pid = 0u;
                    bool ok = phoneapi_encode_admin_set_favorite(s.active_num,
                                                                  !currently_set,
                                                                  &pid);
                    char buf[80];
                    if (ok) {
                        snprintf(buf, sizeof(buf),
                                 "%s favorite (pid=%#lx)",
                                 currently_set ? "Cleared" : "Set",
                                 (unsigned long)pid);
                    } else {
                        snprintf(buf, sizeof(buf),
                                 "Favorite push failed (no my_node?)");
                    }
                    lv_label_set_text(s.header, buf);
                    break;
                }
                case OP_IGNORE: {
                    if (s.active_num == 0u) break;
                    phoneapi_node_t e;
                    bool currently_set =
                        phoneapi_cache_get_node_by_id(s.active_num, &e) &&
                        e.is_unmessagable;
                    uint32_t pid = 0u;
                    bool ok = phoneapi_encode_admin_set_ignored(s.active_num,
                                                                 !currently_set,
                                                                 &pid);
                    char buf[80];
                    if (ok) {
                        snprintf(buf, sizeof(buf),
                                 "%s ignore (pid=%#lx)",
                                 currently_set ? "Cleared" : "Set",
                                 (unsigned long)pid);
                    } else {
                        snprintf(buf, sizeof(buf),
                                 "Ignore push failed (no my_node?)");
                    }
                    lv_label_set_text(s.header, buf);
                    break;
                }
                case OP_TRACEROUTE: {
                    if (s.active_num == 0u) break;
                    uint32_t pid = 0u;
                    bool ok = phoneapi_encode_traceroute(s.active_num, 0u, &pid);
                    /* Surface immediate feedback in the header — the
                     * actual route reply lands later via cascade
                     * rx_packet (RouteDiscovery) and shows up in the
                     * RTT trace; v1 doesn't render the route in-UI. */
                    if (ok) {
                        char buf[80];
                        snprintf(buf, sizeof(buf),
                                 "Traceroute sent (pid=%#lx) — see RTT for reply",
                                 (unsigned long)pid);
                        lv_label_set_text(s.header, buf);
                    } else {
                        lv_label_set_text(s.header, "Traceroute push failed");
                    }
                    break;
                }
                case OP_REQUEST_POS: {
                    if (s.active_num == 0u) break;
                    uint32_t pid = 0u;
                    bool ok = phoneapi_encode_position_request(s.active_num,
                                                               0u, &pid);
                    if (ok) {
                        char buf[80];
                        snprintf(buf, sizeof(buf),
                                 "Position request sent (pid=%#lx)",
                                 (unsigned long)pid);
                        lv_label_set_text(s.header, buf);
                    } else {
                        lv_label_set_text(s.header, "Position request failed");
                    }
                    break;
                }
                default:
                    /* Placeholder hint — flash the header with a TBD
                     * marker so the press is visibly registered. */
                    lv_label_set_text(s.header, "(TBD — not wired yet)");
                    break;
            }
            break;
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_NODE_DETAIL);
            break;
        default: break;
    }
}

static void refresh(void)
{
    if (s.header == NULL) return;
    uint32_t cur = phoneapi_cache_change_seq();
    if (cur == s.last_change_seq) return;
    s.last_change_seq = cur;
    /* render() rebuilds row labels using fresh is_favorite /
     * is_unmessagable from the cache, plus updates the header alias.
     * Cheap (snprintf + label_set_text per row). */
    render();
}

static const view_descriptor_t NODE_OPS_DESC = {
    .id      = VIEW_ID_NODE_OPS,
    .name    = "node_ops",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
};

const view_descriptor_t *node_ops_view_descriptor(void)
{
    return &NODE_OPS_DESC;
}
