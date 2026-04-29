/* view_router.h — Core 1 view switcher.
 *
 * Owns the KeyEvent queue consumer and dispatches events to exactly one
 * active view at a time. FUNC key (MOKYA_KEY_FUNC, press edge) cycles
 * between registered views; all other events are forwarded to the
 * active view's _apply hook.
 *
 * Lazy-create + LRU cache (post 2026-04-27 refactor):
 *   Views are NOT all created at boot. Only the active view + up to
 *   `lru_capacity` recently-touched views keep live LVGL widget trees.
 *   Switching to a destroyed view runs its `create()` callback;
 *   evicted views run `destroy()` which tears the widget tree but
 *   preserves UX state in the view's own state struct (e.g. messages
 *   scroll offset, settings cache). LVGL pool usage decouples from
 *   total view count — adding production pages no longer linearly
 *   eats the 56 KB pool.
 *
 * Adding a new view:
 *   1. Define `static const view_descriptor_t XXX_DESC = { ... }` in
 *      <name>_view.c with create / destroy / apply / refresh callbacks
 *   2. Export `const view_descriptor_t *<name>_view_descriptor(void);`
 *   3. Add the id to `view_id_t` enum and slot it into
 *      `g_view_registry[]` in view_registry.c
 *
 * Thread model (LV_USE_OS = LV_OS_NONE): all entry points MUST be
 * called from the lvgl_task context.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "key_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── View identity ───────────────────────────────────────────────────── *
 *
 * Post-Phase 1 router: FUNC short-press is no longer a cycle — it opens
 * the L-1 launcher modal. App-to-app navigation is launcher-driven
 * (`view_router_navigate()`), so enum order is purely for catalog
 * stability. Debug-only views are guarded by MOKYA_DEBUG_VIEWS so
 * production builds skip them and `VIEW_ID_COUNT` adjusts
 * automatically.                                                         */
typedef enum {
    VIEW_ID_BOOT_HOME = 0,    /* L-0 boot dashboard                       */
    VIEW_ID_LAUNCHER,         /* L-1 9-grid app menu (modal)              */
    VIEW_ID_MESSAGES,         /* A-1 chat list (Phase 3: chat_list_view)  */
    VIEW_ID_MESSAGES_CHAT,    /* A-2 conversation thread (Phase 3)        */
    VIEW_ID_NODES,            /* C-1 node list                            */
    VIEW_ID_NODE_DETAIL,      /* C-2 single-node full detail              */
    VIEW_ID_NODE_OPS,         /* C-3 per-node operations menu             */
    VIEW_ID_MY_NODE,          /* C-4 my (local) node info + settings link */
    VIEW_ID_SETTINGS,         /* S-0 settings                             */
    VIEW_ID_TOOLS,            /* T-0 tools / diagnostics list (Phase 3)   */
    VIEW_ID_IME,              /* G-3 IME modal-only                       */
    VIEW_ID_KEYPAD,           /* legacy keypad debug grid (under Tools)   */
#if MOKYA_DEBUG_VIEWS
    VIEW_ID_RF_DEBUG,
    VIEW_ID_FONT_TEST,
#endif
    VIEW_ID_COUNT
} view_id_t;

/* ── Descriptor flags ────────────────────────────────────────────────── */

/* View must never be destroyed (e.g. always-on overlays). Skipped by the
 * LRU eviction loop and does not count against `lru_capacity`. No view
 * uses this today; reserved for future IME picker overlay etc. */
#define VIEW_FLAG_ALWAYS_RESIDENT  (1u << 0)

/* ── Descriptor ──────────────────────────────────────────────────────── *
 *
 * Each view owns one `static const view_descriptor_t` in its `.c`. The
 * router reads it via the registry's `*_view_descriptor()` getter.
 *
 * Lifecycle contract:
 *   - create(panel):  build the entire widget tree under `panel`. The
 *                     view should restore visible UX from its own state
 *                     struct (typically `static <name>_state_t s_state`
 *                     in the .c). Synchronous; should complete in < 20 ms
 *                     (settings ≤ 50 ms is acceptable).
 *   - destroy():      null all widget pointers and reset transient
 *                     render caches. The router calls `lv_obj_del(panel)`
 *                     after this returns; the view MUST NOT call it. UX
 *                     state lives in the view-owned struct (plain .bss),
 *                     not in the LVGL pool, so it survives automatically.
 *   - apply(ev):      handle KeyEvent. Only called while view is active.
 *   - refresh():      called every lvgl_task tick while view is active.
 *                     Hidden views do NOT receive refresh.
 */
typedef struct {
    view_id_t   id;
    const char *name;
    void      (*create)(lv_obj_t *panel);
    void      (*destroy)(void);
    void      (*apply)(const key_event_t *ev);
    void      (*refresh)(void);
    uint32_t    flags;
} view_descriptor_t;

/* Registry table; index is `view_id_t`. Populated by
 * view_registry_populate() during view_router_init. Defined in
 * view_registry.c. Each slot is a pointer to a file-scope const
 * descriptor in the corresponding view's .c. */
extern const view_descriptor_t *g_view_registry[VIEW_ID_COUNT];
void view_registry_populate(void);

/* ── Router API ──────────────────────────────────────────────────────── */

/* Initialise the router on `screen` with LRU capacity = `lru_capacity`
 * non-active cached views (so up to `1 + lru_capacity` widget trees can
 * be alive simultaneously). Default value used at boot is 3, giving
 * 1 active + 3 cached = max 4 trees. After this call, view 0
 * (`VIEW_ID_KEYPAD`) is active and visible. */
void view_router_init(lv_obj_t *screen, uint8_t lru_capacity);

/* Drain any queued KeyEvents, handle FUNC-press as a view cycle,
 * forward other events to the active view, then call the active
 * view's refresh hook. Call once per lvgl_task iteration after
 * lv_timer_handler(). */
void view_router_tick(void);

/* Switch to `target_id`. Creates the view if it has been evicted,
 * promotes it past any cached view in the LRU order. Idempotent if
 * already active. */
void view_router_navigate(view_id_t target_id);

/* Currently active view id. */
view_id_t view_router_active(void);

/* ── Modal view borrow (Stage 3 IME string edit) ─────────────────────── *
 *
 * Lets one view temporarily hand control to another (e.g.
 * settings_view → ime_view to type a string), with a callback that
 * fires when the user is done. While modal, FUNC press no longer
 * cycles views; instead it commits the modal and returns to the
 * caller. Other keys are forwarded to the borrowed view normally.
 *
 * Re-entry is rejected (only one modal at a time). The caller view is
 * pinned in the LRU cache — never evicted — so its widget tree (and
 * therefore its UX state) survives the modal round-trip even if the
 * cache would otherwise have spilled it.
 */
typedef void (*view_router_modal_done_t)(bool committed, void *ctx);

void view_router_modal_enter(view_id_t target,
                             view_router_modal_done_t on_done,
                             void *ctx);

/* Variant: modal opens as an OVERLAY — caller's panel stays on the
 * screen tree, NOT hidden / NOT reparented to the off-screen stash.
 * Used by the A-2 conversation IME inline submode (G-3 Mode A) so the
 * chat history above the 24 px IME strip stays visible while typing.
 *
 * The caller's `apply()` hook is still suppressed (router only forwards
 * keys to the active = modal target), and the caller's `refresh()` is
 * allowed via `view_router_caller_refreshable()` so unread / status
 * updates can paint behind the overlay.
 *
 * On modal_finish, the overlay flag is cleared and the caller comes
 * back as active without a re-create (it never went away).            */
void view_router_modal_enter_overlay(view_id_t target,
                                     view_router_modal_done_t on_done,
                                     void *ctx);

bool view_router_in_modal(void);

/* While in overlay-modal, the caller view's `refresh` should still be
 * called by the router so background updates (e.g. dm_store ack
 * transitions in conv_view) repaint the visible area below the IME
 * strip. Returns true iff the active modal is overlay-style and the
 * given id is the caller pinned underneath. */
bool view_router_caller_refreshable(view_id_t id);

#ifdef __cplusplus
} /* extern "C" */
#endif
