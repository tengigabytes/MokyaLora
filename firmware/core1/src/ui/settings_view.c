/* settings_view.c — see settings_view.h. */

#include "settings_view.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ipc_protocol.h"
#include "mie_font.h"
#include "mie/keycode.h"
#include "settings_client.h"
#include "settings_keys.h"
#include "view_router.h"
#include "ime_task.h"

/* Index of the IME view in view_router's table. Must match the
 * init order in view_router.c (keypad=0, rf=1, font_test=2, ime=3,
 * messages=4, nodes=5, settings=6). */
#define VIEW_INDEX_IME  3

/* Layout (landscape 320×240):
 *   y   0 .. 23   header  — "[Group X/Y] group_name [pending: N]"
 *   y  24 .. 215  body    — key list (cursor "> " prefix on selected row)
 *   y 216 .. 239  footer  — hint / status
 */

static lv_obj_t *s_panel;
static lv_obj_t *s_header;
static lv_obj_t *s_body;
static lv_obj_t *s_footer;

/* ── Per-key cached state ────────────────────────────────────────────── *
 *
 * Index space is the row-order in settings_keys.c (settings_key_find
 * doesn't expose an index, so we track our own parallel array via
 * pointer compare).
 *
 * value/value_len = last value seen from a CONFIG_VALUE reply or our
 * own SET (so the UI shows the new value immediately even before
 * Apply round-trips).
 *
 * dirty = caller pressed OK in edit mode; pending until next Apply.
 * The Apply success clears dirty for all keys in the group.
 */
#define KEY_CACHE_MAX  14u  /* equals settings_keys_total_count() — keep in sync */
/* Per-key cached value buffer. Owner long_name (39 B) is stored
 * truncated to keep BSS tight; Stage 3 string editing won't go through
 * this cache — IME view will own the buffer. */
#define VAL_BUF_MAX    16u

typedef struct {
    bool      have_value;
    bool      dirty;
    uint8_t   value_len;
    uint8_t   value[VAL_BUF_MAX];
} key_cache_t;

static key_cache_t s_cache[KEY_CACHE_MAX];

/* ── UI state ────────────────────────────────────────────────────────── */

typedef enum {
    UI_BROWSE = 0,
    UI_EDIT,
} ui_mode_t;

static ui_mode_t s_mode;
static uint8_t   s_cur_group;     /* settings_group_t */
static uint8_t   s_cur_row;       /* index in current group's row list (0 .. n_keys = Apply) */
static bool      s_dirty_load;    /* trigger GET burst on next refresh after activation */
static uint32_t  s_last_render_seq;
static uint32_t  s_render_seq;

/* Edit overlay working copy (raw u8/u32 representation in little-endian
 * bytes — same wire format we send via SET). */
static uint8_t  s_edit_buf[VAL_BUF_MAX];
static uint16_t s_edit_len;
static uint16_t s_edit_key;       /* IPC_CFG_* of the key being edited */

static char s_footer_msg[48];     /* sticky footer override (msg ≤ ~40 chars) */

/* Stage 3 string-edit state. While the modal IME borrow is active,
 * s_pending_str_key holds the IPC_CFG_* of the key the user is
 * editing so the callback can route the typed string back into our
 * cache and emit IPC_CMD_SET_CONFIG. Cleared on every callback. */
static uint16_t s_pending_str_key;

/* Forward decls for the STR edit modal callback (defined near
 * confirm_edit; referenced earlier from enter_edit_for_row). */
static void str_edit_done(bool committed, void *ctx);

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int find_key_index(uint16_t ipc_key)
{
    uint8_t idx = 0;
    for (uint8_t g = 0; g < SG_GROUP_COUNT; ++g) {
        uint8_t cnt = 0;
        const settings_key_def_t *defs = settings_keys_in_group(g, &cnt);
        for (uint8_t i = 0; i < cnt; ++i) {
            if (defs[i].ipc_key == ipc_key) return (int)(idx + i);
        }
        idx += cnt;
    }
    return -1;
}

static int row_to_global_index(uint8_t group, uint8_t row, uint8_t group_n_keys)
{
    if (row >= group_n_keys) return -1;  /* "Apply" row */
    uint8_t idx = 0;
    for (uint8_t pg = 0; pg < group; ++pg) {
        uint8_t pc = 0;
        (void)settings_keys_in_group(pg, &pc);
        idx += pc;
    }
    return (int)(idx + row);
}

static uint32_t value_to_u32(const uint8_t *buf, uint8_t len)
{
    uint32_t v = 0;
    if (len > 4) len = 4;
    for (uint8_t i = 0; i < len; ++i) v |= ((uint32_t)buf[i]) << (8u * i);
    return v;
}

static void value_from_u32(uint8_t *buf, uint8_t len, uint32_t v)
{
    if (len > 4) len = 4;
    for (uint8_t i = 0; i < len; ++i) buf[i] = (uint8_t)(v >> (8u * i));
}

static int32_t value_to_i32(const uint8_t *buf, uint8_t len)
{
    /* Sign-extend from the top byte of the field. */
    uint32_t u = value_to_u32(buf, len);
    if (len == 1 && (u & 0x80u)) u |= 0xFFFFFF00u;
    else if (len == 2 && (u & 0x8000u)) u |= 0xFFFF0000u;
    return (int32_t)u;
}

static void format_value(const settings_key_def_t *def,
                         const uint8_t *buf, uint8_t len,
                         char *out, size_t out_sz)
{
    if (len == 0) { snprintf(out, out_sz, "(?)"); return; }
    switch (def->kind) {
    case SK_KIND_BOOL:
        snprintf(out, out_sz, "%s", buf[0] ? "true" : "false");
        break;
    case SK_KIND_ENUM_U8: {
        uint8_t v = buf[0];
        if (def->enum_values && v < def->enum_count && def->enum_values[v]) {
            snprintf(out, out_sz, "%s", def->enum_values[v]);
        } else {
            snprintf(out, out_sz, "%u", (unsigned)v);
        }
        break;
    }
    case SK_KIND_U8:
        snprintf(out, out_sz, "%u", (unsigned)buf[0]);
        break;
    case SK_KIND_I8:
        snprintf(out, out_sz, "%d", (int)(int8_t)buf[0]);
        break;
    case SK_KIND_U32:
        snprintf(out, out_sz, "%lu", (unsigned long)value_to_u32(buf, len));
        break;
    case SK_KIND_STR: {
        size_t n = (len < out_sz - 1) ? len : (out_sz - 1);
        memcpy(out, buf, n);
        out[n] = '\0';
        break;
    }
    default:
        snprintf(out, out_sz, "?");
        break;
    }
}

static uint8_t pending_count_in_group(uint8_t group)
{
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(group, &n_keys);
    if (!defs) return 0;
    uint8_t pending = 0;
    int base = row_to_global_index(group, 0, n_keys + 1);
    if (base < 0) return 0;
    for (uint8_t i = 0; i < n_keys; ++i) {
        if (s_cache[base + i].dirty) pending++;
    }
    return pending;
}

static bool group_needs_reboot(uint8_t group)
{
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(group, &n_keys);
    if (!defs) return false;
    int base = row_to_global_index(group, 0, n_keys + 1);
    if (base < 0) return false;
    for (uint8_t i = 0; i < n_keys; ++i) {
        if (s_cache[base + i].dirty && defs[i].needs_reboot) return true;
    }
    return false;
}

static void send_get_burst(uint8_t group)
{
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(group, &n_keys);
    if (!defs) return;
    for (uint8_t i = 0; i < n_keys; ++i) {
        (void)settings_client_send_get(defs[i].ipc_key);
    }
}

/* ── Rendering ───────────────────────────────────────────────────────── */

static void render_browse(void)
{
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(s_cur_group, &n_keys);

    char hdr[64];
    uint8_t pending = pending_count_in_group(s_cur_group);
    if (pending > 0) {
        snprintf(hdr, sizeof(hdr), "[%u/%u] %s  *%u",
                 (unsigned)(s_cur_group + 1),
                 (unsigned)SG_GROUP_COUNT,
                 settings_group_name((settings_group_t)s_cur_group),
                 (unsigned)pending);
    } else {
        snprintf(hdr, sizeof(hdr), "[%u/%u] %s",
                 (unsigned)(s_cur_group + 1),
                 (unsigned)SG_GROUP_COUNT,
                 settings_group_name((settings_group_t)s_cur_group));
    }
    lv_label_set_text(s_header, hdr);

    /* Body: fixed-size buffer; ~10 lines × ~40 chars. */
    char body[640];
    size_t pos = 0;
    int base = row_to_global_index(s_cur_group, 0, n_keys + 1);
    for (uint8_t i = 0; i < n_keys && pos < sizeof(body) - 80; ++i) {
        const settings_key_def_t *d = &defs[i];
        char val[48];
        if (base >= 0 && s_cache[base + i].have_value) {
            format_value(d, s_cache[base + i].value,
                         s_cache[base + i].value_len, val, sizeof(val));
        } else {
            snprintf(val, sizeof(val), "...");
        }
        const char *cursor = (i == s_cur_row) ? "> " : "  ";
        const char *flag   = (base >= 0 && s_cache[base + i].dirty) ? " *" : "";
        pos += (size_t)snprintf(&body[pos], sizeof(body) - pos,
                                "%s%s : %s%s\n",
                                cursor, d->label, val, flag);
    }
    /* Trailing Apply row */
    if (pos < sizeof(body) - 40) {
        const char *cursor = (s_cur_row == n_keys) ? "> " : "  ";
        const char *label;
        if (pending == 0)               label = "[no changes]";
        else if (group_needs_reboot(s_cur_group)) label = "[Apply + reboot]";
        else                            label = "[Apply]";
        snprintf(&body[pos], sizeof(body) - pos, "%s%s\n", cursor, label);
    }
    lv_label_set_text(s_body, body);

    if (s_footer_msg[0]) {
        lv_label_set_text(s_footer, s_footer_msg);
    } else {
        lv_label_set_text(s_footer,
                          "UP/DN row  L/R group  OK edit  BACK clr");
    }
}

static void render_edit(void)
{
    const settings_key_def_t *d = settings_key_find(s_edit_key);
    if (!d) { s_mode = UI_BROWSE; render_browse(); return; }

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "edit: %s.%s",
             settings_group_name((settings_group_t)d->group), d->label);
    lv_label_set_text(s_header, hdr);

    char body[256];
    char val[48];
    format_value(d, s_edit_buf, (uint8_t)s_edit_len, val, sizeof(val));

    if (d->kind == SK_KIND_STR) {
        snprintf(body, sizeof(body),
                 "current : %s\n"
                 "\n"
                 "(string editing arrives in Stage 3 — IME wiring)",
                 val);
    } else if (d->kind == SK_KIND_ENUM_U8 || d->kind == SK_KIND_BOOL) {
        snprintf(body, sizeof(body),
                 "value : %s\n"
                 "\n"
                 "UP / DOWN  cycle\n"
                 "OK         confirm\n"
                 "BACK       cancel",
                 val);
    } else {
        snprintf(body, sizeof(body),
                 "value : %s\n"
                 "range : %ld .. %ld\n"
                 "\n"
                 "UP / DOWN  ±1\n"
                 "L / R      ±10\n"
                 "OK         confirm\n"
                 "BACK       cancel",
                 val, (long)d->min, (long)d->max);
    }
    lv_label_set_text(s_body, body);

    if (d->needs_reboot) {
        lv_label_set_text(s_footer, "* changing this key requires reboot *");
    } else {
        lv_label_set_text(s_footer, "soft-reload (no reboot)");
    }
}

static void render_all(void)
{
    if (s_mode == UI_EDIT) render_edit();
    else                   render_browse();
}

/* ── Edit-mode key handling ──────────────────────────────────────────── */

static void edit_step(const settings_key_def_t *d, int step)
{
    uint32_t cur_u = value_to_u32(s_edit_buf, (uint8_t)s_edit_len);
    int32_t  cur_i = value_to_i32(s_edit_buf, (uint8_t)s_edit_len);

    switch (d->kind) {
    case SK_KIND_BOOL:
        s_edit_buf[0] = !s_edit_buf[0];
        s_edit_len = 1;
        break;
    case SK_KIND_ENUM_U8: {
        int32_t v = (int32_t)s_edit_buf[0];
        int32_t cnt = (int32_t)d->enum_count;
        if (cnt <= 0) cnt = 1;
        v = ((v + step) % cnt + cnt) % cnt;   /* wrap, accept negative step */
        s_edit_buf[0] = (uint8_t)v;
        s_edit_len = 1;
        break;
    }
    case SK_KIND_U8: {
        int32_t v = (int32_t)cur_u + step;
        if (v < d->min) v = d->min;
        if (v > d->max) v = d->max;
        s_edit_buf[0] = (uint8_t)v;
        s_edit_len = 1;
        break;
    }
    case SK_KIND_I8: {
        int32_t v = cur_i + step;
        if (v < d->min) v = d->min;
        if (v > d->max) v = d->max;
        s_edit_buf[0] = (uint8_t)(int8_t)v;
        s_edit_len = 1;
        break;
    }
    case SK_KIND_U32: {
        int64_t v = (int64_t)cur_u + step;
        if (v < d->min) v = d->min;
        if (v > d->max) v = d->max;
        value_from_u32(s_edit_buf, 4, (uint32_t)v);
        s_edit_len = 4;
        break;
    }
    default:
        break;
    }
}

static void enter_edit_for_row(uint8_t row)
{
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(s_cur_group, &n_keys);
    if (!defs || row >= n_keys) return;
    int gidx = row_to_global_index(s_cur_group, row, n_keys + 1);
    if (gidx < 0) return;

    s_edit_key = defs[row].ipc_key;

    /* Strings skip the numeric edit overlay entirely — hand straight off
     * to the IME view via a modal borrow (Stage 3). */
    if (defs[row].kind == SK_KIND_STR) {
        s_pending_str_key = s_edit_key;
        ime_view_clear_text();
        snprintf(s_footer_msg, sizeof(s_footer_msg),
                 "type %s, FUNC = done", defs[row].label);
        s_render_seq++;
        view_router_modal_enter(VIEW_INDEX_IME, str_edit_done, NULL);
        return;
    }

    if (s_cache[gidx].have_value) {
        s_edit_len = s_cache[gidx].value_len;
        memcpy(s_edit_buf, s_cache[gidx].value, s_edit_len);
    } else {
        memset(s_edit_buf, 0, sizeof(s_edit_buf));
        /* Default-size by kind so step math has somewhere to write. */
        s_edit_len = (defs[row].kind == SK_KIND_U32) ? 4 : 1;
    }
    s_mode = UI_EDIT;
    s_render_seq++;
}

/* UTF-8 safe truncation: walk back from `max_bytes` until we land on
 * a code-point boundary (any byte whose top two bits are NOT 10b).
 * Returns the largest length ≤ max_bytes that ends on a clean boundary. */
static int utf8_truncate_to(const char *s, int len, int max_bytes)
{
    if (len <= max_bytes) return len;
    int i = max_bytes;
    while (i > 0 && (((unsigned char)s[i]) & 0xC0u) == 0x80u) i--;
    return i;
}

static void str_edit_done(bool committed, void *ctx)
{
    (void)ctx;
    uint16_t key = s_pending_str_key;
    s_pending_str_key = 0;
    if (!committed || key == 0) {
        ime_view_clear_text();
        s_render_seq++;
        return;
    }

    const settings_key_def_t *d = settings_key_find(key);
    if (!d) { ime_view_clear_text(); s_render_seq++; return; }

    int byte_len = 0;
    const char *t = ime_view_text(&byte_len, NULL);
    if (t == NULL || byte_len <= 0) {
        snprintf(s_footer_msg, sizeof(s_footer_msg),
                 "(empty — not committed)");
        ime_view_clear_text();
        s_render_seq++;
        return;
    }

    /* Truncate to the key's max byte length on a UTF-8 boundary. */
    int send_len = utf8_truncate_to(t, byte_len, (int)d->max);
    if (send_len <= 0) {
        ime_view_clear_text();
        s_render_seq++;
        return;
    }

    /* Cache for display (truncated again to VAL_BUF_MAX on a UTF-8
     * boundary; the cache is just for the browse list, the SET payload
     * carries the full send_len bytes). */
    int gidx = find_key_index(key);
    if (gidx >= 0) {
        int cache_len = utf8_truncate_to(t, send_len, (int)VAL_BUF_MAX);
        s_cache[gidx].value_len  = (uint8_t)cache_len;
        memcpy(s_cache[gidx].value, t, cache_len);
        s_cache[gidx].have_value = true;
        s_cache[gidx].dirty      = true;
    }

    (void)settings_client_send_set(key, t, (uint16_t)send_len);
    ime_view_clear_text();
    s_render_seq++;
}

static void confirm_edit(void)
{
    const settings_key_def_t *d = settings_key_find(s_edit_key);
    if (!d) { s_mode = UI_BROWSE; return; }
    if (d->kind == SK_KIND_STR) {
        /* Hand off to the IME view via a modal borrow. The user types
         * the string with the normal IME, then presses FUNC to "done";
         * view_router invokes str_edit_done() which routes the typed
         * UTF-8 into s_cache + IPC_CMD_SET_CONFIG. */
        s_pending_str_key = s_edit_key;
        ime_view_clear_text();
        snprintf(s_footer_msg, sizeof(s_footer_msg),
                 "type %s, FUNC = done", d->label);
        s_mode = UI_BROWSE;
        s_render_seq++;
        view_router_modal_enter(VIEW_INDEX_IME, str_edit_done, NULL);
        return;
    }

    int gidx = find_key_index(s_edit_key);
    if (gidx < 0) { s_mode = UI_BROWSE; return; }

    /* Cache the new value locally so the browse view shows it immediately. */
    s_cache[gidx].value_len  = (uint8_t)s_edit_len;
    memcpy(s_cache[gidx].value, s_edit_buf, s_edit_len);
    s_cache[gidx].have_value = true;
    s_cache[gidx].dirty      = true;

    /* Push SET to Core 0 — handler accumulates pending_segments. The
     * actual flash write happens at Apply (COMMIT). */
    (void)settings_client_send_set(s_edit_key, s_edit_buf, s_edit_len);

    s_mode = UI_BROWSE;
    s_render_seq++;
}

static void apply_changes(void)
{
    if (pending_count_in_group(s_cur_group) == 0) {
        snprintf(s_footer_msg, sizeof(s_footer_msg), "(no pending changes)");
        s_render_seq++;
        return;
    }
    bool needs_reboot = group_needs_reboot(s_cur_group);
    if (!settings_client_send_commit(needs_reboot)) {
        snprintf(s_footer_msg, sizeof(s_footer_msg),
                 "commit push failed (ring full)");
        s_render_seq++;
        return;
    }
    snprintf(s_footer_msg, sizeof(s_footer_msg),
             needs_reboot ? "applying + reboot..." : "applying...");

    /* Clear dirty flags on the assumption Core 0 will accept; if a
     * later CONFIG_RESULT carries an error we'll surface it, but for
     * now an OK clears the asterisks. */
    uint8_t n_keys = 0;
    const settings_key_def_t *defs = settings_keys_in_group(s_cur_group, &n_keys);
    int base = (defs != NULL) ? row_to_global_index(s_cur_group, 0, n_keys + 1) : -1;
    if (base >= 0) {
        for (uint8_t i = 0; i < n_keys; ++i) s_cache[base + i].dirty = false;
    }
    s_render_seq++;
}

/* ── Reply ingestion ─────────────────────────────────────────────────── */

static void drain_replies(void)
{
    settings_client_reply_t r;
    while (settings_client_take_reply(&r)) {
        if (r.kind == (uint8_t)SCR_VALUE) {
            int gidx = find_key_index(r.key);
            if (gidx >= 0 && gidx < (int)KEY_CACHE_MAX) {
                uint8_t n = (uint8_t)((r.value_len < VAL_BUF_MAX)
                                      ? r.value_len : VAL_BUF_MAX);
                s_cache[gidx].value_len  = n;
                memcpy(s_cache[gidx].value, r.value, n);
                s_cache[gidx].have_value = true;
            }
            s_render_seq++;
        } else if (r.kind == (uint8_t)SCR_OK) {
            /* commit OK has key=0; SET OK has key= the IPC_CFG_* */
            if (r.key == 0) {
                snprintf(s_footer_msg, sizeof(s_footer_msg), "applied");
                s_render_seq++;
            }
        } else if (r.kind == (uint8_t)SCR_UNKNOWN_KEY) {
            snprintf(s_footer_msg, sizeof(s_footer_msg),
                     "err: unknown key 0x%04x", (unsigned)r.key);
            s_render_seq++;
        } else if (r.kind == (uint8_t)SCR_INVALID_VALUE) {
            snprintf(s_footer_msg, sizeof(s_footer_msg),
                     "err: invalid value for 0x%04x", (unsigned)r.key);
            s_render_seq++;
        } else if (r.kind == (uint8_t)SCR_BUSY) {
            snprintf(s_footer_msg, sizeof(s_footer_msg), "err: core 0 busy");
            s_render_seq++;
        }
    }
}

/* ── Public entry points ─────────────────────────────────────────────── */

void settings_view_init(lv_obj_t *panel)
{
    const lv_font_t *f16 = mie_font_unifont_sm_16();
    s_panel = panel;

    settings_client_init();

    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);

    s_header = lv_label_create(panel);
    if (f16) lv_obj_set_style_text_font(s_header, f16, 0);
    lv_obj_set_style_text_color(s_header, lv_color_hex(0xFFFF80), 0);
    lv_label_set_text(s_header, "[1/6] Device");
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_width(s_header, 312);

    s_body = lv_label_create(panel);
    if (f16) lv_obj_set_style_text_font(s_body, f16, 0);
    lv_obj_set_style_text_color(s_body, lv_color_white(), 0);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_body, 2, 0);
    lv_label_set_text(s_body, "loading...");
    lv_obj_set_pos(s_body, 0, 24);
    lv_obj_set_size(s_body, 312, 240 - 24 - 24);

    s_footer = lv_label_create(panel);
    if (f16) lv_obj_set_style_text_font(s_footer, f16, 0);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(0x808080), 0);
    lv_label_set_text(s_footer, "");
    lv_obj_set_pos(s_footer, 0, 240 - 24);
    lv_obj_set_width(s_footer, 312);

    s_dirty_load = true;
}

void settings_view_apply(const key_event_t *ev)
{
    if (!ev || !ev->pressed) return;

    s_footer_msg[0] = '\0';   /* any keypress clears the sticky status */

    if (s_mode == UI_EDIT) {
        const settings_key_def_t *d = settings_key_find(s_edit_key);
        if (!d) { s_mode = UI_BROWSE; s_render_seq++; return; }

        switch (ev->keycode) {
        case MOKYA_KEY_UP:    edit_step(d, +1);  s_render_seq++; break;
        case MOKYA_KEY_DOWN:  edit_step(d, -1);  s_render_seq++; break;
        case MOKYA_KEY_RIGHT: edit_step(d, +10); s_render_seq++; break;
        case MOKYA_KEY_LEFT:  edit_step(d, -10); s_render_seq++; break;
        case MOKYA_KEY_OK:    confirm_edit();              break;
        case MOKYA_KEY_BACK:  s_mode = UI_BROWSE;
                              s_render_seq++;              break;
        default: break;
        }
        return;
    }

    /* UI_BROWSE */
    uint8_t n_keys = 0;
    (void)settings_keys_in_group(s_cur_group, &n_keys);
    uint8_t row_max = n_keys;   /* extra slot = Apply */

    switch (ev->keycode) {
    case MOKYA_KEY_UP:
        if (s_cur_row > 0) s_cur_row--;
        s_render_seq++;
        break;
    case MOKYA_KEY_DOWN:
        if (s_cur_row < row_max) s_cur_row++;
        s_render_seq++;
        break;
    case MOKYA_KEY_LEFT:
        s_cur_group = (uint8_t)((s_cur_group + SG_GROUP_COUNT - 1) % SG_GROUP_COUNT);
        s_cur_row = 0;
        send_get_burst(s_cur_group);
        s_render_seq++;
        break;
    case MOKYA_KEY_RIGHT:
        s_cur_group = (uint8_t)((s_cur_group + 1) % SG_GROUP_COUNT);
        s_cur_row = 0;
        send_get_burst(s_cur_group);
        s_render_seq++;
        break;
    case MOKYA_KEY_OK:
        if (s_cur_row == n_keys) apply_changes();
        else                     enter_edit_for_row(s_cur_row);
        break;
    case MOKYA_KEY_BACK: {
        /* Clear pending dirty flags in this group (cancel pending edits). */
        const settings_key_def_t *defs = settings_keys_in_group(s_cur_group, &n_keys);
        if (defs != NULL) {
            int base = row_to_global_index(s_cur_group, 0, n_keys + 1);
            if (base >= 0) {
                for (uint8_t i = 0; i < n_keys; ++i) s_cache[base + i].dirty = false;
            }
        }
        send_get_burst(s_cur_group);   /* re-read truth from Core 0 */
        s_render_seq++;
        break;
    }
    default: break;
    }
}

void settings_view_refresh(void)
{
    if (s_panel == NULL) return;

    /* Drain replies even when hidden so caches stay current; just skip
     * the LVGL relabel work while off-screen. */
    drain_replies();

    if (lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    if (s_dirty_load) {
        send_get_burst(s_cur_group);
        s_dirty_load = false;
        s_render_seq++;
    }

    if (s_render_seq != s_last_render_seq) {
        s_last_render_seq = s_render_seq;
        render_all();
    }
}
