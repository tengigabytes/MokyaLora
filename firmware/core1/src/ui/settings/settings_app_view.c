/* settings_app_view.c — see settings_app_view.h. */

#include "settings_app_view.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "global/hint_bar.h"

#include "key_event.h"
#include "mie/keycode.h"

#include "settings_tree.h"
#include "settings_keys.h"
#include "settings_client.h"
#include "template_toggle.h"
#include "template_enum.h"
#include "template_number.h"
#include "phoneapi_cache.h"
#include "ipc_protocol.h"

/* ── Layout (BROWSE mode) ───────────────────────────────────────────── */

#define PANEL_W       320
#define PANEL_H       224
#define BC_Y            0
#define BC_H           16
#define LIST_Y         18
#define ROW_H          16
#define MAX_VISIBLE   12

/* ── State ──────────────────────────────────────────────────────────── */

typedef enum {
    SAV_MODE_BROWSE      = 0,
    SAV_MODE_EDIT_TOGGLE = 1,
    SAV_MODE_EDIT_ENUM   = 2,
    SAV_MODE_EDIT_NUMBER = 3,
} sav_mode_t;

typedef struct {
    lv_obj_t *panel;
    lv_obj_t *bc_lbl;
    lv_obj_t *rows[MAX_VISIBLE];

    settings_tree_node_t *cursor;        /* current parent in BROWSE        */
    uint16_t              sel_row;
    uint16_t              scroll_top;

    sav_mode_t            mode;
    settings_tree_node_t *edit_leaf;     /* leaf being edited in non-BROWSE */
} settings_app_t;

static settings_app_t s;

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, int h,
                            lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

/* ── BROWSE render ──────────────────────────────────────────────────── */

static void show_browse_widgets(bool visible)
{
    /* Toggle visibility of the list rows (breadcrumb stays in both modes). */
    for (uint16_t i = 0; i < MAX_VISIBLE; ++i) {
        if (!s.rows[i]) continue;
        if (visible) lv_obj_clear_flag(s.rows[i], LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(s.rows[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_browse(void)
{
    if (s.cursor == NULL) return;

    /* Breadcrumb */
    char bc_buf[80];
    settings_tree_format_breadcrumb(s.cursor, bc_buf, sizeof(bc_buf));
    lv_label_set_text(s.bc_lbl, bc_buf);

    uint16_t n = settings_tree_child_count(s.cursor);

    /* Clamp selection / scroll. */
    if (n == 0) s.sel_row = 0;
    else if (s.sel_row >= n) s.sel_row = (uint16_t)(n - 1);
    if (s.sel_row < s.scroll_top) s.scroll_top = s.sel_row;
    if (s.sel_row >= s.scroll_top + MAX_VISIBLE) {
        s.scroll_top = (uint16_t)(s.sel_row - MAX_VISIBLE + 1);
    }
    if (s.scroll_top > 0 && s.scroll_top + MAX_VISIBLE > n) {
        s.scroll_top = (n > MAX_VISIBLE) ? (uint16_t)(n - MAX_VISIBLE) : 0;
    }

    char buf[80];
    for (uint16_t i = 0; i < MAX_VISIBLE; ++i) {
        uint16_t child_idx = (uint16_t)(s.scroll_top + i);
        if (child_idx >= n) {
            lv_label_set_text(s.rows[i], "");
            continue;
        }
        settings_tree_node_t *child = settings_tree_child_at(s.cursor, child_idx);
        const char *lbl = settings_tree_node_label(child);
        bool focused = (child_idx == s.sel_row);

        if (settings_tree_node_kind(child) == ST_NODE_GROUP) {
            uint16_t sub = settings_tree_child_count(child);
            snprintf(buf, sizeof(buf), "%s %s (%u)",
                     focused ? ">" : " ", lbl, (unsigned)sub);
        } else {
            snprintf(buf, sizeof(buf), "%s %s",
                     focused ? ">" : " ", lbl);
        }
        lv_label_set_text(s.rows[i], buf);
        lv_obj_set_style_text_color(s.rows[i],
            focused ? ui_color(UI_COLOR_ACCENT_FOCUS)
                    : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    }
}

/* ── BROWSE → EDIT_TOGGLE transition ────────────────────────────────── */

/* Best-effort current-value lookup for a leaf key. Reads phoneapi
 * cache where possible; falls back to 0 if unknown. */
static uint8_t read_u8_current(const settings_key_def_t *kd)
{
    /* phoneapi_cache exposes a small set of mirrored config sub-fields
     * via per-section accessors. For the long-tail of keys we don't
     * have a uniform accessor here — return 0 until each sub-section
     * getter is wired. (This is a render-time hint only; the IPC SET
     * on apply is correct regardless.) */
    (void)kd;
    return 0u;
}

static void update_breadcrumb_to_leaf(settings_tree_node_t *leaf)
{
    char bc_buf[80];
    settings_tree_format_breadcrumb(leaf, bc_buf, sizeof(bc_buf));
    lv_label_set_text(s.bc_lbl, bc_buf);
}

static void enter_toggle_edit(settings_tree_node_t *leaf)
{
    const settings_key_def_t *kd = settings_tree_node_key(leaf);
    if (!kd || kd->kind != SK_KIND_BOOL) return;

    s.mode      = SAV_MODE_EDIT_TOGGLE;
    s.edit_leaf = leaf;
    update_breadcrumb_to_leaf(leaf);
    show_browse_widgets(false);
    template_toggle_open(s.panel, kd, read_u8_current(kd) != 0u);
}

static void enter_enum_edit(settings_tree_node_t *leaf)
{
    const settings_key_def_t *kd = settings_tree_node_key(leaf);
    if (!kd || kd->kind != SK_KIND_ENUM_U8) return;

    s.mode      = SAV_MODE_EDIT_ENUM;
    s.edit_leaf = leaf;
    update_breadcrumb_to_leaf(leaf);
    show_browse_widgets(false);
    template_enum_open(s.panel, kd, read_u8_current(kd));
}

/* Best-effort current i32 lookup for numeric kinds. Until phoneapi
 * cache exposes uniform per-key getters, fall back to min so the
 * user sees a stable starting point (clamped by template_number). */
static int32_t read_i32_current(const settings_key_def_t *kd)
{
    if (!kd) return 0;
    return kd->min;
}

static void enter_number_edit(settings_tree_node_t *leaf)
{
    const settings_key_def_t *kd = settings_tree_node_key(leaf);
    if (!kd) return;
    if (kd->kind != SK_KIND_U8 && kd->kind != SK_KIND_I8 &&
        kd->kind != SK_KIND_U32 && kd->kind != SK_KIND_U32_FLAGS) return;

    s.mode      = SAV_MODE_EDIT_NUMBER;
    s.edit_leaf = leaf;
    update_breadcrumb_to_leaf(leaf);
    show_browse_widgets(false);
    template_number_open(s.panel, kd, read_i32_current(kd));
}

static void exit_toggle_edit(void)
{
    if (template_toggle_committed()) {
        const settings_key_def_t *kd = settings_tree_node_key(s.edit_leaf);
        if (kd) {
            uint8_t v = template_toggle_value() ? 1u : 0u;
            settings_client_send_set(kd->ipc_key, /*channel*/0u, &v, 1u);
            settings_client_send_commit(kd->needs_reboot != 0u);
        }
    }
    template_toggle_close();
    s.mode      = SAV_MODE_BROWSE;
    s.edit_leaf = NULL;
    show_browse_widgets(true);
    render_browse();
}

static void exit_enum_edit(void)
{
    if (template_enum_committed()) {
        const settings_key_def_t *kd = settings_tree_node_key(s.edit_leaf);
        if (kd) {
            uint8_t v = template_enum_value();
            settings_client_send_set(kd->ipc_key, /*channel*/0u, &v, 1u);
            settings_client_send_commit(kd->needs_reboot != 0u);
        }
    }
    template_enum_close();
    s.mode      = SAV_MODE_BROWSE;
    s.edit_leaf = NULL;
    show_browse_widgets(true);
    render_browse();
}

static void exit_number_edit(void)
{
    if (template_number_committed()) {
        const settings_key_def_t *kd = settings_tree_node_key(s.edit_leaf);
        if (kd) {
            int32_t v32 = template_number_value();
            /* Pack to wire-width matching the key kind. Little-endian
             * matches Core 0's IPC config decoder convention. */
            switch (kd->kind) {
                case SK_KIND_U8: {
                    uint8_t b = (uint8_t)(v32 < 0 ? 0 : (v32 > 0xFF ? 0xFF : v32));
                    settings_client_send_set(kd->ipc_key, 0u, &b, 1u);
                    break;
                }
                case SK_KIND_I8: {
                    int8_t  b = (int8_t)(v32 < -128 ? -128 : (v32 > 127 ? 127 : v32));
                    settings_client_send_set(kd->ipc_key, 0u, &b, 1u);
                    break;
                }
                case SK_KIND_U32:
                case SK_KIND_U32_FLAGS: {
                    uint32_t u = (uint32_t)v32;
                    uint8_t  buf[4] = {
                        (uint8_t)(u & 0xFFu),
                        (uint8_t)((u >> 8)  & 0xFFu),
                        (uint8_t)((u >> 16) & 0xFFu),
                        (uint8_t)((u >> 24) & 0xFFu),
                    };
                    settings_client_send_set(kd->ipc_key, 0u, buf, 4u);
                    break;
                }
                default: break;
            }
            settings_client_send_commit(kd->needs_reboot != 0u);
        }
    }
    template_number_close();
    s.mode      = SAV_MODE_BROWSE;
    s.edit_leaf = NULL;
    show_browse_widgets(true);
    render_browse();
}

/* ── BROWSE OK handler ──────────────────────────────────────────────── */

static void browse_open_child(void)
{
    uint16_t n = settings_tree_child_count(s.cursor);
    if (n == 0) return;
    settings_tree_node_t *child =
        settings_tree_child_at(s.cursor, s.sel_row);
    if (!child) return;

    if (settings_tree_node_kind(child) == ST_NODE_LEAF) {
        const settings_key_def_t *kd = settings_tree_node_key(child);
        if (kd) {
            switch (kd->kind) {
                case SK_KIND_BOOL:
                    enter_toggle_edit(child);
                    return;
                case SK_KIND_ENUM_U8:
                    enter_enum_edit(child);
                    return;
                case SK_KIND_U8:
                case SK_KIND_I8:
                case SK_KIND_U32:
                case SK_KIND_U32_FLAGS:
                    enter_number_edit(child);
                    return;
                default: break;
            }
        }
        /* Other kinds — not yet implemented in Phase 4. Flash the
         * breadcrumb so OK still feels responsive. */
        char tbd[80];
        snprintf(tbd, sizeof(tbd),
                 "(template TBD for kind=%u key=0x%04X)",
                 kd ? kd->kind : 0,
                 kd ? kd->ipc_key : 0);
        lv_label_set_text(s.bc_lbl, tbd);
        return;
    }
    /* Enter the group: cursor moves down, sel resets. */
    s.cursor = child;
    s.sel_row = 0;
    s.scroll_top = 0;
    render_browse();
}

static void browse_back(void)
{
    settings_tree_node_t *p = settings_tree_parent(s.cursor);
    if (p == NULL) {
        /* Already at root — leave Settings, go home. */
        view_router_navigate(VIEW_ID_BOOT_HOME);
    } else {
        s.cursor = p;
        s.sel_row = 0;
        s.scroll_top = 0;
        render_browse();
    }
}

/* ── Lifecycle ──────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));
    s.panel  = panel;
    s.cursor = settings_tree_root();
    s.mode   = SAV_MODE_BROWSE;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.bc_lbl = make_label(panel, 4, BC_Y, PANEL_W - 8, BC_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    for (uint16_t i = 0; i < MAX_VISIBLE; ++i) {
        s.rows[i] = make_label(panel, 4, LIST_Y + i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }

    /* Per spec: Settings App hides the Hint Bar. */
    hint_bar_clear();

    render_browse();
}

static void destroy(void)
{
    /* If we left the view mid-edit, drop the active template's widgets
     * without committing — they live as siblings of our rows under
     * `s.panel`, which the router is about to lv_obj_del. */
    if (s.mode == SAV_MODE_EDIT_TOGGLE) template_toggle_close();
    if (s.mode == SAV_MODE_EDIT_ENUM)   template_enum_close();
    if (s.mode == SAV_MODE_EDIT_NUMBER) template_number_close();
    s.bc_lbl = NULL;
    for (uint16_t i = 0; i < MAX_VISIBLE; ++i) s.rows[i] = NULL;
    s.panel = NULL;
    s.mode = SAV_MODE_BROWSE;
    s.edit_leaf = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;

    if (s.mode == SAV_MODE_EDIT_TOGGLE) {
        (void)template_toggle_apply_key(ev);
        if (template_toggle_done()) {
            exit_toggle_edit();
        }
        return;
    }
    if (s.mode == SAV_MODE_EDIT_ENUM) {
        (void)template_enum_apply_key(ev);
        if (template_enum_done()) {
            exit_enum_edit();
        }
        return;
    }
    if (s.mode == SAV_MODE_EDIT_NUMBER) {
        (void)template_number_apply_key(ev);
        if (template_number_done()) {
            exit_number_edit();
        }
        return;
    }

    /* BROWSE mode */
    if (s.cursor == NULL) return;
    uint16_t n = settings_tree_child_count(s.cursor);

    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (n > 0 && s.sel_row > 0) { s.sel_row--; render_browse(); }
            break;
        case MOKYA_KEY_DOWN:
            if (n > 0 && s.sel_row + 1 < n) { s.sel_row++; render_browse(); }
            break;
        case MOKYA_KEY_OK:
            browse_open_child();
            break;
        case MOKYA_KEY_BACK:
            browse_back();
            break;
        default: break;
    }
}

static void refresh(void)
{
    /* Idle: nothing to do in either mode. List updates happen on
     * key events (BROWSE) or template-driven re-render (EDIT). */
}

static const view_descriptor_t SETTINGS_APP_DESC = {
    .id      = VIEW_ID_SETTINGS,
    .name    = "settings_app",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
};

const view_descriptor_t *settings_app_view_descriptor(void)
{
    return &SETTINGS_APP_DESC;
}
