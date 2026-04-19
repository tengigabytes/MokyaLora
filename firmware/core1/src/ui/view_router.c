/* view_router.c — see view_router.h. */

#include "view_router.h"

#include "key_event.h"
#include "keypad_view.h"
#include "rf_debug_view.h"
#include "font_test_view.h"
#include "mie/keycode.h"

/* ── View table ──────────────────────────────────────────────────────── *
 *
 * `panel` is a container lv_obj_t covering the full screen; the view's
 * own widgets get parented to it. FUNC-press cycles `active` modulo
 * VIEW_COUNT. `hidden` is set via LV_OBJ_FLAG_HIDDEN so LVGL skips
 * rendering the off-screen view entirely.                            */

typedef void (*view_apply_fn)(const key_event_t *);
typedef void (*view_refresh_fn)(void);

typedef struct {
    const char       *name;
    lv_obj_t         *panel;
    view_apply_fn     apply;
    view_refresh_fn   refresh;
} view_entry_t;

#define VIEW_COUNT  3
static view_entry_t s_views[VIEW_COUNT];
static int          s_active;

/* Optional helper: create a full-screen container with no padding /
 * border / scroll so init'd views paint pixel-perfect at (0,0). */
static lv_obj_t *make_panel(lv_obj_t *screen)
{
    lv_obj_t *p = lv_obj_create(screen);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void activate(int idx)
{
    for (int i = 0; i < VIEW_COUNT; ++i) {
        if (i == idx) lv_obj_clear_flag(s_views[i].panel, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag  (s_views[i].panel, LV_OBJ_FLAG_HIDDEN);
    }
    s_active = idx;
}

void view_router_init(lv_obj_t *screen)
{
    /* Create panels first so init hooks can populate them. */
    s_views[0].name    = "keypad";
    s_views[0].panel   = make_panel(screen);
    s_views[0].apply   = keypad_view_apply;
    s_views[0].refresh = NULL;                       /* event-driven only */
    keypad_view_init(s_views[0].panel);

    s_views[1].name    = "rf";
    s_views[1].panel   = make_panel(screen);
    s_views[1].apply   = rf_debug_view_apply;
    s_views[1].refresh = rf_debug_view_refresh;
    rf_debug_view_init(s_views[1].panel);

    s_views[2].name    = "font_test";
    s_views[2].panel   = make_panel(screen);
    s_views[2].apply   = font_test_view_apply;
    s_views[2].refresh = NULL;
    font_test_view_init(s_views[2].panel);

    activate(0);   /* keypad_view visible at boot; FUNC cycles to rf / font_test */
}

void view_router_tick(void)
{
    key_event_t ev;
    while (key_event_pop(&ev, 0)) {
        /* FUNC press edge cycles views. Release is intentionally
         * forwarded to the active view so it can clear its "pressed"
         * highlight for FUNC if the user held it. */
        if (ev.keycode == MOKYA_KEY_FUNC && ev.pressed) {
            activate((s_active + 1) % VIEW_COUNT);
            continue;
        }
        if (s_views[s_active].apply) {
            s_views[s_active].apply(&ev);
        }
    }

    /* Refresh ALL views each tick so hidden views stay current and the
     * user sees fresh data the moment they swap back. Cheap because the
     * refresh paths only mutate label text when the snapshot changes. */
    for (int i = 0; i < VIEW_COUNT; ++i) {
        if (s_views[i].refresh) s_views[i].refresh();
    }
}
