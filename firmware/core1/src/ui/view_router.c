/* view_router.c — see view_router.h. */

#include "view_router.h"

#include "key_event.h"
#include "keypad_view.h"
#include "rf_debug_view.h"
#include "font_test_view.h"
#include "ime_view.h"
#include "messages_view.h"
#include "nodes_view.h"
#include "settings_view.h"
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

#define VIEW_COUNT  7
static view_entry_t s_views[VIEW_COUNT];
static int          s_view_router_active;

/* Modal borrow state (Stage 3 — IME string edit for settings_view).
 * `s_modal_caller` is -1 when no modal is active, otherwise the index
 * of the view that requested the borrow. `s_modal_on_done` fires when
 * the modal exits via FUNC press. */
static int                       s_modal_caller = -1;
static view_router_modal_done_t  s_modal_on_done;
static void                     *s_modal_ctx;

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
    s_view_router_active = idx;
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

    s_views[3].name    = "ime";
    s_views[3].panel   = make_panel(screen);
    s_views[3].apply   = ime_view_apply;
    s_views[3].refresh = ime_view_refresh;
    ime_view_init(s_views[3].panel);

    s_views[4].name    = "messages";
    s_views[4].panel   = make_panel(screen);
    s_views[4].apply   = messages_view_apply;
    s_views[4].refresh = messages_view_refresh;
    messages_view_init(s_views[4].panel);

    s_views[5].name    = "nodes";
    s_views[5].panel   = make_panel(screen);
    s_views[5].apply   = nodes_view_apply;
    s_views[5].refresh = nodes_view_refresh;
    nodes_view_init(s_views[5].panel);

    s_views[6].name    = "settings";
    s_views[6].panel   = make_panel(screen);
    s_views[6].apply   = settings_view_apply;
    s_views[6].refresh = settings_view_refresh;
    settings_view_init(s_views[6].panel);

    activate(0);   /* keypad_view visible at boot; FUNC cycles keypad → rf → font_test → ime → messages → nodes → settings */
}

static void modal_finish(bool committed)
{
    int caller = s_modal_caller;
    view_router_modal_done_t cb = s_modal_on_done;
    void *ctx = s_modal_ctx;
    s_modal_caller  = -1;
    s_modal_on_done = NULL;
    s_modal_ctx     = NULL;
    if (cb) cb(committed, ctx);
    if (caller >= 0 && caller < VIEW_COUNT) activate(caller);
}

void view_router_modal_enter(int target_view,
                             view_router_modal_done_t on_done,
                             void *ctx)
{
    if (s_modal_caller >= 0) return;                  /* reject re-entry */
    if (target_view < 0 || target_view >= VIEW_COUNT) return;
    s_modal_caller  = s_view_router_active;
    s_modal_on_done = on_done;
    s_modal_ctx     = ctx;
    activate(target_view);
}

bool view_router_in_modal(void)
{
    return s_modal_caller >= 0;
}

void view_router_tick(void)
{
    /* Drain the view-observer mirror queue (see key_event.c). The IME
     * task owns the primary queue; popping it here would race. */
    key_event_t ev;
    while (key_event_view_pop(&ev, 0)) {
        /* FUNC press edge: outside modal it cycles views; inside modal
         * it commits the borrow and fires the on_done callback before
         * snapping back to the caller view. */
        if (ev.keycode == MOKYA_KEY_FUNC && ev.pressed) {
            if (s_modal_caller >= 0) modal_finish(true);
            else                     activate((s_view_router_active + 1) % VIEW_COUNT);
            continue;
        }
        if (s_views[s_view_router_active].apply) {
            s_views[s_view_router_active].apply(&ev);
        }
    }

    /* Refresh ALL views each tick so hidden views stay current and the
     * user sees fresh data the moment they swap back. Cheap because the
     * refresh paths only mutate label text when the snapshot changes. */
    for (int i = 0; i < VIEW_COUNT; ++i) {
        if (s_views[i].refresh) s_views[i].refresh();
    }
}
