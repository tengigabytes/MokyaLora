/* ime_task.cpp -- see ime_task.h for contract.
 *
 * Single consumer of the KeyEvent queue (firmware-architecture.md §4.4);
 * listener callbacks therefore run on this task's thread and cannot
 * re-enter ImeLogic (contract documented in ime_logic.h). The shared
 * snapshot state is protected by a FreeRTOS mutex that the LVGL view
 * reader briefly takes while rendering.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ime_task.h"

#include <cstring>
#include <new>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <mie/ime_logic.h>
#include <mie/trie_searcher.h>

#include "key_event.h"

namespace {

/* ── Static state ─────────────────────────────────────────────────── */

mie::TrieSearcher g_zh_searcher;
mie::TrieSearcher g_en_searcher;

/* ImeLogic constructor takes refs to TrieSearcher, so we placement-new
 * into aligned storage once searchers are loaded. */
alignas(mie::ImeLogic) uint8_t g_ime_storage[sizeof(mie::ImeLogic)];
mie::ImeLogic *g_ime = nullptr;

SemaphoreHandle_t g_snapshot_mutex = nullptr;

/* Commit buffer: appended on each on_commit(). When full, the front
 * half is discarded to keep recent commits visible — the LVGL view is
 * a scratchpad, not a persistent transcript. */
constexpr size_t kCommitCapacity = 1024;
char   g_commit_buf[kCommitCapacity + 1] = {0};
size_t g_commit_len = 0;

/* ── Listener adapter ─────────────────────────────────────────────── */

class IMEListener : public mie::IImeListener {
public:
    void on_commit(const char *utf8) override {
        if (!utf8) return;
        size_t add = std::strlen(utf8);
        if (add == 0) return;

        /* Take the mutex; callers of the view API are already holding
         * it briefly while rendering, and the ime task is the only
         * writer. Use xSemaphoreTake with a short timeout so a stuck
         * LVGL pass doesn't wedge the IME. */
        if (xSemaphoreTake(g_snapshot_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            return;   /* dropping a commit is visible (no text shown);
                       * better than deadlocking the IME task. */
        }

        if (g_commit_len + add >= kCommitCapacity) {
            /* Keep the last half to preserve recent context. */
            size_t keep = kCommitCapacity / 2;
            if (g_commit_len > keep) {
                std::memmove(g_commit_buf,
                             g_commit_buf + (g_commit_len - keep),
                             keep);
                g_commit_len = keep;
            }
            /* If add > keep slot, truncate the new fragment. */
            if (add + g_commit_len >= kCommitCapacity) {
                add = kCommitCapacity - g_commit_len - 1;
            }
        }
        std::memcpy(g_commit_buf + g_commit_len, utf8, add);
        g_commit_len += add;
        g_commit_buf[g_commit_len] = '\0';

        xSemaphoreGive(g_snapshot_mutex);
        __atomic_add_fetch(&g_ime_dirty_counter, 1u, __ATOMIC_RELEASE);
    }

    void on_composition_changed() override {
        __atomic_add_fetch(&g_ime_dirty_counter, 1u, __ATOMIC_RELEASE);
    }

    /* on_cursor_move / on_delete_before: leave the defaults (no-op)
     * until M5 text-area widget wires a textarea cursor. */
};

IMEListener g_listener;

/* ── Task body ────────────────────────────────────────────────────── */

constexpr TickType_t kTickMs = 20;   /* ImeLogic::tick() cadence. */

static inline uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void ime_task_fn(void *) {
    for (;;) {
        key_event_t ev{};
        if (key_event_pop(&ev, pdMS_TO_TICKS(kTickMs))) {
            mie::KeyEvent mev;
            mev.keycode = (mokya_keycode_t)ev.keycode;
            mev.pressed = ev.pressed != 0;
            mev.now_ms  = now_ms();
            /* process_key drives the listener when state changes. */
            g_ime->process_key(mev);
        }
        /* Tick regardless — advances multi-tap auto-commit and SYM1
         * long-press timers. Cheap no-op when no timers pending. */
        g_ime->tick(now_ms());
    }
}

} // namespace

/* ── Public API ──────────────────────────────────────────────────── */

extern "C" {

volatile uint32_t g_ime_dirty_counter = 0;

bool ime_task_start(const mie_dict_pointers_t *dict, UBaseType_t priority) {
    if (!dict || !dict->zh_dat || dict->zh_dat_size == 0 ||
        !dict->zh_val || dict->zh_val_size == 0) {
        return false;
    }

    if (!g_zh_searcher.load_from_memory(dict->zh_dat, dict->zh_dat_size,
                                        dict->zh_val, dict->zh_val_size)) {
        return false;
    }

    bool have_en = (dict->en_dat && dict->en_dat_size > 0 &&
                    dict->en_val && dict->en_val_size > 0);
    if (have_en) {
        if (!g_en_searcher.load_from_memory(dict->en_dat, dict->en_dat_size,
                                            dict->en_val, dict->en_val_size)) {
            /* EN is optional — log-style failure would go through the
             * listener once that's wired; for now silently continue
             * with ZH-only so the user can still type Chinese. */
            have_en = false;
        }
    }

    mie::TrieSearcher *en = have_en ? &g_en_searcher : nullptr;
    g_ime = new (g_ime_storage) mie::ImeLogic(g_zh_searcher, en);
    g_ime->set_listener(&g_listener);

    g_snapshot_mutex = xSemaphoreCreateMutex();
    if (!g_snapshot_mutex) return false;

    return xTaskCreate(ime_task_fn, "ime", 1024, nullptr, priority, nullptr)
           == pdPASS;
}

bool ime_view_lock(TickType_t timeout_ticks) {
    return g_snapshot_mutex &&
           xSemaphoreTake(g_snapshot_mutex, timeout_ticks) == pdTRUE;
}

void ime_view_unlock(void) {
    if (g_snapshot_mutex) xSemaphoreGive(g_snapshot_mutex);
}

const char *ime_view_pending(int *byte_len, int *matched_prefix, int *style) {
    if (!g_ime) {
        if (byte_len)       *byte_len = 0;
        if (matched_prefix) *matched_prefix = 0;
        if (style)          *style = 0;
        return "";
    }
    mie::PendingView pv = g_ime->pending_view();
    if (byte_len)       *byte_len = pv.byte_len;
    if (matched_prefix) *matched_prefix = pv.matched_prefix_bytes;
    if (style)          *style = (int)pv.style;
    return pv.str ? pv.str : "";
}

const char *ime_view_mode_indicator(void) {
    return g_ime ? g_ime->mode_indicator() : "";
}

int ime_view_page_candidate_count(void) {
    return g_ime ? g_ime->page_cand_count() : 0;
}

const char *ime_view_page_candidate(int idx) {
    if (!g_ime || idx < 0 || idx >= g_ime->page_cand_count()) return "";
    return g_ime->page_cand(idx).word;
}

int ime_view_page_selected(void) {
    return g_ime ? g_ime->page_sel() : 0;
}

int ime_view_page(void) {
    return g_ime ? g_ime->page() : 0;
}

int ime_view_page_count(void) {
    return g_ime ? g_ime->page_count() : 0;
}

const char *ime_view_commit_text(int *byte_len) {
    if (byte_len) *byte_len = (int)g_commit_len;
    return g_commit_buf;
}

void ime_view_clear_commit(void) {
    if (!g_snapshot_mutex) return;
    xSemaphoreTake(g_snapshot_mutex, portMAX_DELAY);
    g_commit_len = 0;
    g_commit_buf[0] = '\0';
    xSemaphoreGive(g_snapshot_mutex);
    __atomic_add_fetch(&g_ime_dirty_counter, 1u, __ATOMIC_RELEASE);
}

} // extern "C"
