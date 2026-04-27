/* view_router.c — see view_router.h.
 *
 * Lazy create + LRU cache implementation (2026-04-27 refactor):
 *
 *   - At most `1 + lru_capacity` widget trees alive at any moment
 *     (1 active + N cached + caller-pinned-during-modal).
 *   - Active view runs apply / refresh every tick. Cached and destroyed
 *     views run NEITHER until promoted back to active.
 *   - Switching to a destroyed view runs its `create()`. Cached views
 *     just unhide + reparent to the active screen.
 *   - Eviction triggers when cache count exceeds N: oldest non-active,
 *     non-modal-caller, non-ALWAYS_RESIDENT view runs `destroy()` and
 *     its panel gets `lv_obj_del`'d.
 *   - During modal, the caller view is pinned: it keeps its widget tree
 *     so its UX state survives the round-trip even if the LRU would
 *     otherwise have spilled it. Cache may temporarily exceed N while
 *     pinned; settles after `modal_finish` reactivates the caller.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "view_router.h"

#include <stddef.h>
#include <stdint.h>

#include "key_event.h"
#include "mie/keycode.h"

/* ── Runtime state per view ──────────────────────────────────────────── */

typedef struct {
    lv_obj_t *panel;       /* NULL when destroyed */
    bool      in_cache;    /* visible state — true when hidden in stash, false when active or destroyed */
    uint32_t  lru_seq;     /* monotonic touch counter; bigger = more recently used */
} view_runtime_t;

static view_runtime_t s_rt[VIEW_ID_COUNT];
/* uint32_t (not view_id_t) because ARM AAPCS short-enum sizes the enum
 * to 1 byte, but scripts/ime_text_test.py reads this as a u32 over SWD.
 * Keeping it u32 also avoids signed-vs-unsigned ambiguity for the
 * "no view active" sentinel (UINT32_MAX). */
static uint32_t       s_view_router_active = UINT32_MAX;
static uint32_t       s_modal_caller = UINT32_MAX;

static view_router_modal_done_t s_modal_on_done;
static void                    *s_modal_ctx;

static uint8_t        s_lru_capacity;
static uint32_t       s_lru_counter;

static lv_obj_t      *s_screen;
static lv_obj_t      *s_stash;       /* off-screen parent for cached views */

/* ── Helpers ─────────────────────────────────────────────────────────── */

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static const view_descriptor_t *desc_of(view_id_t id)
{
    return g_view_registry[id];
}

static void destroy_view(view_id_t id)
{
    if (s_rt[id].panel == NULL) return;
    const view_descriptor_t *d = desc_of(id);
    if (d->destroy) d->destroy();
    lv_obj_del(s_rt[id].panel);
    s_rt[id].panel = NULL;
    s_rt[id].in_cache = false;
}

static int count_cached(void)
{
    int n = 0;
    for (int i = 0; i < VIEW_ID_COUNT; ++i) if (s_rt[i].in_cache) ++n;
    return n;
}

/* Pick the oldest evictable cached view, or -1 if none. Pinned (modal
 * caller) and ALWAYS_RESIDENT views are skipped. */
static int find_evict_target(void)
{
    int best = -1;
    uint32_t best_seq = UINT32_MAX;
    for (int i = 0; i < VIEW_ID_COUNT; ++i) {
        if (!s_rt[i].in_cache) continue;
        if ((view_id_t)i == s_modal_caller) continue;
        if (desc_of(i)->flags & VIEW_FLAG_ALWAYS_RESIDENT) continue;
        if (s_rt[i].lru_seq < best_seq) {
            best = i;
            best_seq = s_rt[i].lru_seq;
        }
    }
    return best;
}

static void enforce_lru_cap(void)
{
    while (count_cached() > s_lru_capacity) {
        int victim = find_evict_target();
        if (victim < 0) break;          /* cap exceeded but everything pinned */
        destroy_view((view_id_t)victim);
    }
}

/* Move target to active. Creates if destroyed; promotes if cached.
 * Demotes the previous active to cache (unless the new target IS the
 * previous active, in which case this is a no-op). */
static void switch_active(view_id_t target)
{
    if (target == s_view_router_active) {
        s_rt[target].lru_seq = ++s_lru_counter;
        return;
    }

    /* Demote old active to cache (if any) */
    view_id_t prev = s_view_router_active;
    if (prev != UINT32_MAX && s_rt[prev].panel != NULL) {
        lv_obj_add_flag(s_rt[prev].panel, LV_OBJ_FLAG_HIDDEN);
        if (s_stash != NULL && lv_obj_get_parent(s_rt[prev].panel) != s_stash) {
            lv_obj_set_parent(s_rt[prev].panel, s_stash);
        }
        s_rt[prev].in_cache = true;
    }

    /* Promote target: create if missing */
    if (s_rt[target].panel == NULL) {
        s_rt[target].panel = make_panel(s_screen);
        const view_descriptor_t *d = desc_of(target);
        if (d->create) d->create(s_rt[target].panel);
    } else {
        if (lv_obj_get_parent(s_rt[target].panel) != s_screen) {
            lv_obj_set_parent(s_rt[target].panel, s_screen);
        }
        lv_obj_clear_flag(s_rt[target].panel, LV_OBJ_FLAG_HIDDEN);
    }
    s_rt[target].in_cache = false;
    s_rt[target].lru_seq = ++s_lru_counter;
    s_view_router_active = target;

    enforce_lru_cap();
}

/* ── Public API ──────────────────────────────────────────────────────── */

void view_router_init(lv_obj_t *screen, uint8_t lru_capacity)
{
    view_registry_populate();

    s_screen = screen;
    s_lru_capacity = lru_capacity;
    s_lru_counter  = 0;
    s_modal_caller = UINT32_MAX;
    s_modal_on_done = NULL;
    s_modal_ctx     = NULL;

    for (int i = 0; i < VIEW_ID_COUNT; ++i) {
        s_rt[i].panel    = NULL;
        s_rt[i].in_cache = false;
        s_rt[i].lru_seq  = 0;
    }

    /* Off-screen stash: free-standing lv_obj with no parent (LVGL only
     * traverses the active screen tree during refresh, so children of
     * this stash are invisible to the refresh pipeline). */
    s_stash = lv_obj_create(NULL);

    /* Boot view = first id (KEYPAD). */
    s_view_router_active = UINT32_MAX;
    switch_active(VIEW_ID_KEYPAD);
}

void view_router_navigate(view_id_t target)
{
    if (target >= VIEW_ID_COUNT) return;
    switch_active(target);
}

view_id_t view_router_active(void)
{
    return s_view_router_active;
}

static void modal_finish(bool committed)
{
    view_id_t caller = s_modal_caller;
    view_router_modal_done_t cb = s_modal_on_done;
    void *ctx = s_modal_ctx;
    s_modal_caller  = UINT32_MAX;
    s_modal_on_done = NULL;
    s_modal_ctx     = NULL;
    if (cb) cb(committed, ctx);
    if (caller != UINT32_MAX) switch_active(caller);
}

void view_router_modal_enter(view_id_t target,
                             view_router_modal_done_t on_done,
                             void *ctx)
{
    if (s_modal_caller != UINT32_MAX) return;          /* reject re-entry */
    if (target >= VIEW_ID_COUNT) return;
    s_modal_caller  = s_view_router_active;
    s_modal_on_done = on_done;
    s_modal_ctx     = ctx;
    switch_active(target);
}

bool view_router_in_modal(void)
{
    return s_modal_caller != UINT32_MAX;
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
            if (s_modal_caller != UINT32_MAX) {
                modal_finish(true);
            } else {
                view_id_t next = (view_id_t)((s_view_router_active + 1) % VIEW_ID_COUNT);
                switch_active(next);
            }
            continue;
        }
        const view_descriptor_t *d = desc_of(s_view_router_active);
        if (d->apply) d->apply(&ev);
    }

    /* Active-only refresh: hidden views do nothing (was a per-view
     * HIDDEN guard before this refactor — now the router enforces it). */
    if (s_view_router_active != UINT32_MAX) {
        const view_descriptor_t *d = desc_of(s_view_router_active);
        if (d->refresh) d->refresh();
    }
}
