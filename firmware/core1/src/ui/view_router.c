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

#include "FreeRTOS.h"
#include "task.h"

#include "key_event.h"
#include "mie/keycode.h"

#include "global/status_bar.h"
#include "global/hint_bar.h"
#include "global/global_alert.h"
#include "conversation_view.h"
#include "launcher_view.h"

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
/* When true, the active modal is an overlay: caller's panel was NOT
 * hidden / NOT reparented to the stash. modal_finish must skip the
 * usual unhide-and-reparent path because there's nothing to undo. */
static bool           s_modal_is_overlay = false;

static view_router_modal_done_t s_modal_on_done;
static void                    *s_modal_ctx;

static uint8_t        s_lru_capacity;
static uint32_t       s_lru_counter;

static lv_obj_t      *s_screen;
static lv_obj_t      *s_stash;       /* off-screen parent for cached views */

/* FUNC long-press state machine */
static uint32_t       s_func_press_ms;     /* 0 = not pressed */
static bool           s_func_long_consumed;/* prevent short fire after long */
#define FUNC_LONG_HOLD_MS  2000u

static uint32_t now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    /* Content area sits BELOW the 16 px global Status Bar (G-1).
     * Hint Bar (G-2) is an overlay that floats on top of the panel
     * bottom 16 px when active; views that show it should reserve
     * the bottom 16 px in their layout. */
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, 320, 240 - 16);   /* 224 px */
    lv_obj_set_pos(p, 0, 16);
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
 * previous active, in which case this is a no-op).
 *
 * `keep_prev_visible` skips the hide-and-reparent of the previous
 * active panel. Used by the overlay-modal entry path: caller stays
 * on screen so the IME inline strip can render above it. */
static void switch_active_with_flags(view_id_t target, bool keep_prev_visible)
{
    if (target == s_view_router_active) {
        s_rt[target].lru_seq = ++s_lru_counter;
        return;
    }

    /* Demote old active to cache (if any) — unless we're being told to
     * keep it visible underneath an overlay modal. */
    view_id_t prev = s_view_router_active;
    if (!keep_prev_visible &&
        prev != UINT32_MAX && s_rt[prev].panel != NULL) {
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
    /* Overlay modals must paint ABOVE the kept-visible caller. */
    if (keep_prev_visible) {
        lv_obj_move_foreground(s_rt[target].panel);
    }
    s_rt[target].in_cache = false;
    s_rt[target].lru_seq = ++s_lru_counter;
    s_view_router_active = target;

    /* G-2: refresh global hint_bar from the new view's static hints.
     * Done unconditionally on every switch (cold create OR warm cache
     * promotion) so cached-view promotion — which skips create() —
     * doesn't leak the previous view's hint. Views may override via
     * hint_bar_set() from apply() for submode-specific hints; the
     * override is bounded because the next switch rewrites here.    */
    {
        const view_descriptor_t *d = desc_of(target);
        hint_bar_set(d->hints.left, d->hints.ok, d->hints.right);
    }

    enforce_lru_cap();
}

static void switch_active(view_id_t target)
{
    switch_active_with_flags(target, /*keep_prev_visible=*/false);
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

    /* Global chrome — created once on the screen, survives view swaps. */
    status_bar_init(screen);
    hint_bar_init(screen);
    global_alert_init();

    /* FUNC long-press state */
    s_func_press_ms = 0;
    s_func_long_consumed = false;

    /* Boot view = L-0 home dashboard. */
    s_view_router_active = UINT32_MAX;
    switch_active(VIEW_ID_BOOT_HOME);
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
    bool was_overlay = s_modal_is_overlay;
    s_modal_caller  = UINT32_MAX;
    s_modal_on_done = NULL;
    s_modal_ctx     = NULL;
    s_modal_is_overlay = false;
    /* Restore the caller FIRST so an LRU-evicted caller view is rebuilt
     * before the callback fires. Then run the callback — which may
     * `view_router_navigate()` to a different target (e.g. launcher_view
     * picking VIEW_ID_MESSAGES). The callback's navigate overwrites the
     * restored active view, leaving the caller cached in LRU.
     *
     * For overlay modals the caller never went away — its panel was
     * kept on screen the whole time. We still call switch_active so
     * the active-view bookkeeping (s_view_router_active, LRU seq) gets
     * back to the caller, but with keep_prev_visible=true to avoid
     * hiding the modal panel before its own destroy / del. */
    if (caller != UINT32_MAX) {
        switch_active_with_flags(caller, /*keep_prev_visible=*/was_overlay);
    }
    if (cb) cb(committed, ctx);
}

/* The IME view's widget tree is sized differently for inline (320×24)
 * vs fullscreen (320×224). When the cached panel is the wrong shape
 * for the new request, switch_active just unhides it instead of
 * calling create() — leaving the geometry stale and the inline-mode
 * flag wrong. Force a destroy on modal entry so the create path
 * always runs against the requester's current layout. Cheap (the
 * pre-alloc cells re-allocate in single-digit ms). */
static void ensure_fresh_modal_target(view_id_t target)
{
    if (target == VIEW_ID_IME && s_rt[target].panel != NULL) {
        destroy_view(target);
    }
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
    s_modal_is_overlay = false;
    ensure_fresh_modal_target(target);
    switch_active(target);
}

void view_router_modal_enter_overlay(view_id_t target,
                                     view_router_modal_done_t on_done,
                                     void *ctx)
{
    if (s_modal_caller != UINT32_MAX) return;          /* reject re-entry */
    if (target >= VIEW_ID_COUNT) return;
    s_modal_caller  = s_view_router_active;
    s_modal_on_done = on_done;
    s_modal_ctx     = ctx;
    s_modal_is_overlay = true;
    ensure_fresh_modal_target(target);
    switch_active_with_flags(target, /*keep_prev_visible=*/true);
}

bool view_router_caller_refreshable(view_id_t id)
{
    if (!s_modal_is_overlay) return false;
    if (s_modal_caller == UINT32_MAX) return false;
    return ((view_id_t)s_modal_caller == id);
}

bool view_router_in_modal(void)
{
    return s_modal_caller != UINT32_MAX;
}

void view_router_modal_finish(bool committed)
{
    if (s_modal_caller == UINT32_MAX) return;
    modal_finish(committed);
}

/* Modal callback used when launcher commits — picks the focused tile's
 * target view id and navigates to it after the modal returns control. */
static void launcher_done_cb(bool committed, void *ctx)
{
    (void)ctx;
    if (!committed) return;
    view_id_t target = launcher_view_picked();
    if (target < VIEW_ID_COUNT) {
        view_router_navigate(target);
    }
}

/* Handle FUNC press/release edge: short → open launcher modal (or
 * commit existing modal); long ≥ 2 s → open status bar detail (TODO,
 * stubbed). Legacy: settings_view's str_edit + new conversation_view
 * compose both rely on FUNC short to commit the IME modal — preserved
 * here. SET press in IME modal is an alternate explicit commit (see
 * view_router_tick). */
static void handle_func_event(const key_event_t *ev)
{
    bool in_modal = s_modal_caller != UINT32_MAX;

    if (ev->pressed) {
        s_func_press_ms = now_ms_();
        s_func_long_consumed = false;
        return;
    }

    /* Release */
    uint32_t held = (s_func_press_ms == 0) ? 0 : (now_ms_() - s_func_press_ms);
    s_func_press_ms = 0;

    if (s_func_long_consumed) {
        /* Long-press already fired on hold; ignore release. */
        return;
    }

    if (held >= FUNC_LONG_HOLD_MS) {
        /* Long-press: G-1 detail modal (stub). */
        status_bar_show_alert(0, "FUNC long: G-1 detail (stub)", 1500);
        return;
    }

    /* Short-press */
    if (in_modal) {
        modal_finish(true);
    } else if (s_view_router_active == VIEW_ID_LAUNCHER) {
        /* Should not happen — launcher is always entered as modal — but
         * be safe. */
    } else {
        view_router_modal_enter(VIEW_ID_LAUNCHER, launcher_done_cb, NULL);
    }
}

void view_router_tick(void)
{
    /* Drain the view-observer mirror queue (see key_event.c). The IME
     * task owns the primary queue; popping it here would race. */
    key_event_t ev;
    while (key_event_view_pop(&ev, 0)) {
        if (ev.keycode == MOKYA_KEY_FUNC) {
            handle_func_event(&ev);
            continue;
        }
        /* OK in launcher: commit + navigate to a real target. If the
         * focused tile is a placeholder (target == VIEW_ID_COUNT), fall
         * through to launcher_view's apply() so it can show a "coming
         * soon" toast instead of silently exiting the launcher. */
        if (s_view_router_active == VIEW_ID_LAUNCHER &&
            ev.keycode == MOKYA_KEY_OK && ev.pressed) {
            view_id_t picked = launcher_view_picked();
            if (picked < VIEW_ID_COUNT) {
                modal_finish(true);
                continue;
            }
            /* Placeholder — let launcher's apply() handle it. */
        }
        /* SET in IME modal = explicit "send / apply" commit. Spec-clean
         * alternative to the legacy FUNC-short-as-commit; conversation
         * compose calls this "send", settings text-edit calls it "save".
         * Outside the IME modal, SET keeps its normal per-view semantic
         * (handled by the active view's apply hook). */
        if (s_modal_caller != UINT32_MAX &&
            s_view_router_active == VIEW_ID_IME &&
            ev.keycode == MOKYA_KEY_SET && ev.pressed) {
            modal_finish(true);
            continue;
        }
        /* BACK in any modal cancels */
        if (s_modal_caller != UINT32_MAX &&
            ev.keycode == MOKYA_KEY_BACK && ev.pressed) {
            modal_finish(false);
            continue;
        }
        const view_descriptor_t *d = desc_of(s_view_router_active);
        if (d->apply) d->apply(&ev);
    }

    /* FUNC long-press fires while still held. View-specific overrides
     * land before the generic G-1 stub: e.g. conversation_view opens
     * the A-3 message detail modal. */
    if (s_func_press_ms != 0 && !s_func_long_consumed &&
        (now_ms_() - s_func_press_ms) >= FUNC_LONG_HOLD_MS &&
        (s_modal_caller == UINT32_MAX ||
         s_view_router_active != VIEW_ID_IME)) {
        s_func_long_consumed = true;
        if (s_modal_caller == UINT32_MAX &&
            s_view_router_active == VIEW_ID_MESSAGES_CHAT) {
            conversation_view_open_msg_detail();
        } else {
            status_bar_show_alert(0, "FUNC long: G-1 detail (stub)", 1500);
        }
    }

    /* Active-only refresh: hidden views do nothing (was a per-view
     * HIDDEN guard before this refactor — now the router enforces it). */
    if (s_view_router_active != UINT32_MAX) {
        const view_descriptor_t *d = desc_of(s_view_router_active);
        if (d->refresh) d->refresh();
    }

    /* Overlay-modal extra: caller view is still on screen underneath
     * the modal panel; tick its refresh so unread / status changes
     * keep painting (e.g. dm_store ack arriving while compose is open
     * needs the bubble to repaint behind the IME strip). */
    if (s_modal_is_overlay && s_modal_caller != UINT32_MAX) {
        const view_descriptor_t *cd = desc_of(s_modal_caller);
        if (cd->refresh) cd->refresh();
    }

    /* Status bar tick is independent of active view. */
    status_bar_tick();
    /* G-4 cross-view notifications — drives status_bar alert from
     * dm_store + bq25622. Runs after status_bar_tick so any auto-clear
     * landed first. */
    global_alert_tick();
}
