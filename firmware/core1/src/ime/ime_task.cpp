/* ime_task.cpp -- see ime_task.h for contract.
 *
 * Single consumer of the KeyEvent queue (firmware-architecture.md §4.4).
 * Listener callbacks therefore run on this task's thread and cannot
 * re-enter ImeLogic (contract documented in ime_logic.h). The shared
 * snapshot state is protected by a FreeRTOS mutex that the LVGL view
 * reader briefly takes while rendering.
 *
 * Text + cursor model mirrors mie_repl.cpp. The listener inserts
 * committed text at the cursor, deletes one codepoint before the
 * cursor on DEL-past-pending, and moves the cursor on DPAD presses
 * that ImeLogic flagged as cursor-moves (no pending / no candidates).
 *
 * SPDX-License-Identifier: MIT
 */
#include "ime_task.h"

#include <cstring>
#include <new>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "hardware/xip_cache.h"

#include <mie/ime_logic.h>
#include <mie/trie_searcher.h>

#include "key_event.h"

namespace {

/* ── Static state ─────────────────────────────────────────────────── */

mie::TrieSearcher g_zh_searcher;
mie::TrieSearcher g_en_searcher;

alignas(mie::ImeLogic) uint8_t g_ime_storage[sizeof(mie::ImeLogic)];
mie::ImeLogic *g_ime = nullptr;

SemaphoreHandle_t g_snapshot_mutex = nullptr;

/* Text buffer — the committed string the listener builds up. Bounded
 * at ~2 KB; when the front half is full and more commits arrive, the
 * oldest half is dropped to keep the tail visible. That's a scratchpad
 * sized for the REPL-style visual test, not a transcript store. */
constexpr size_t kTextCapacity = 2048;
char   g_text[kTextCapacity + 1] = {0};
int    g_text_len = 0;
int    g_cursor   = 0;

/* ── UTF-8 helpers ────────────────────────────────────────────────── *
 * prev_boundary: given a valid byte offset in a well-formed UTF-8
 * buffer, return the start byte of the codepoint immediately before.
 * next_boundary: return the start byte of the codepoint at-or-after.
 * Both stay within [0, g_text_len].                                   */

inline int prev_boundary(int pos) {
    if (pos <= 0) return 0;
    --pos;
    while (pos > 0 && ((unsigned char)g_text[pos] & 0xC0) == 0x80) --pos;
    return pos;
}

inline int next_boundary(int pos) {
    if (pos >= g_text_len) return g_text_len;
    ++pos;
    while (pos < g_text_len && ((unsigned char)g_text[pos] & 0xC0) == 0x80) ++pos;
    return pos;
}

/* Extract up to 2 codepoints immediately before g_cursor into `out`.
 * Used to keep ImeLogic's SmartEn auto-space / auto-capitalize state
 * in sync with the REPL-style text buffer. */
void extract_ctx(char out[16]) {
    out[0] = '\0';
    if (g_cursor <= 0) return;
    int end   = g_cursor;
    int start = end;
    for (int cp = 0; cp < 2 && start > 0; ++cp) start = prev_boundary(start);
    int n = end - start;
    if (n < 0) n = 0;
    if (n > 15) n = 15;
    std::memcpy(out, g_text + start, (size_t)n);
    out[n] = '\0';
}

/* Compact the front half of the buffer so `need` more bytes fit.
 * Preserves cursor/text_len invariants. Called only while holding
 * g_snapshot_mutex. */
bool make_room_for(size_t need) {
    if ((size_t)g_text_len + need <= kTextCapacity) return true;
    int keep = (int)(kTextCapacity / 2);
    if (g_text_len > keep) {
        int drop = g_text_len - keep;
        std::memmove(g_text, g_text + drop, (size_t)keep);
        g_text_len = keep;
        g_cursor  -= drop;
        if (g_cursor < 0) g_cursor = 0;
        /* Normalise cursor to a codepoint boundary after the shift. */
        while (g_cursor > 0 && ((unsigned char)g_text[g_cursor] & 0xC0) == 0x80) --g_cursor;
        g_text[g_text_len] = '\0';
    }
    return (size_t)g_text_len + need <= kTextCapacity;
}

void insert_at_cursor(const char *s, size_t n) {
    if (n == 0) return;
    if (!make_room_for(n)) {
        /* Even after drop-half, caller exceeds capacity — truncate. */
        n = kTextCapacity - (size_t)g_text_len;
        if (n == 0) return;
    }
    std::memmove(g_text + g_cursor + n, g_text + g_cursor,
                 (size_t)(g_text_len - g_cursor));
    std::memcpy(g_text + g_cursor, s, n);
    g_text_len += (int)n;
    g_cursor   += (int)n;
    g_text[g_text_len] = '\0';
}

void delete_before_cursor() {
    if (g_cursor <= 0) return;
    int prev = prev_boundary(g_cursor);
    int drop = g_cursor - prev;
    std::memmove(g_text + prev, g_text + g_cursor,
                 (size_t)(g_text_len - g_cursor));
    g_text_len -= drop;
    g_cursor    = prev;
    g_text[g_text_len] = '\0';
}

/* ── Listener adapter ─────────────────────────────────────────────── */

/* Listener callbacks all run on the ime_task thread, from within
 * ImeLogic::process_key(). ime_task holds g_snapshot_mutex across the
 * entire process_key()/tick() call (see ime_task_fn), so the callbacks
 * don't need to re-take the mutex and can't deadlock against it. The
 * outer-scope lock also protects readers against torn reads of
 * candidates_[] while ImeLogic is rebuilding the pool. */
class IMEListener : public mie::IImeListener {
public:
    mie::ImeLogic *ime = nullptr;       /* back-pointer for sync_text_context */

    void on_commit(const char *utf8) override {
        if (!utf8 || !*utf8) return;
        insert_at_cursor(utf8, std::strlen(utf8));
    }

    void on_cursor_move(mie::NavDir d) override {
        switch (d) {
            case mie::NavDir::Left:  g_cursor = prev_boundary(g_cursor); break;
            case mie::NavDir::Right: g_cursor = next_boundary(g_cursor); break;
            case mie::NavDir::Up:                                        break;
            case mie::NavDir::Down:                                      break;
        }
        char ctx[16] = {0};
        extract_ctx(ctx);
        if (ime) ime->set_text_context(ctx);
    }

    void on_delete_before() override {
        delete_before_cursor();
        char ctx[16] = {0};
        extract_ctx(ctx);
        if (ime) ime->set_text_context(ctx);
    }

    void on_composition_changed() override {
        /* Nothing to do — the outer-scope mutation loop bumps the
         * dirty counter once per process_key/tick cycle. */
    }
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
        bool got_ev = key_event_pop(&ev, pdMS_TO_TICKS(kTickMs));

        /* Hold the snapshot mutex across the full process_key()/tick()
         * call so LVGL readers never observe candidates_[] mid-rebuild.
         * Listener callbacks (on_commit, on_cursor_move, on_delete_before)
         * execute within this critical section without re-taking the
         * mutex — they mutate the text buffer and call set_text_context
         * on the same thread. */
        xSemaphoreTake(g_snapshot_mutex, portMAX_DELAY);
        if (got_ev) {
            mie::KeyEvent mev;
            mev.keycode = (mokya_keycode_t)ev.keycode;
            mev.pressed = ev.pressed != 0;
            mev.now_ms  = now_ms();

            g_ime->process_key(mev);
        }
        g_ime->tick(now_ms());
        xSemaphoreGive(g_snapshot_mutex);

        if (got_ev) {
            __atomic_add_fetch(&g_ime_dirty_counter, 1u, __ATOMIC_RELEASE);
        }
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
            have_en = false;
        }
    }

    mie::TrieSearcher *en = have_en ? &g_en_searcher : nullptr;
    g_ime = new (g_ime_storage) mie::ImeLogic(g_zh_searcher, en);
    g_listener.ime = g_ime;
    g_ime->set_listener(&g_listener);

    g_snapshot_mutex = xSemaphoreCreateMutex();
    if (!g_snapshot_mutex) return false;

    /* 2048 words = 8 KB. run_search() puts Candidate tmp[50] on stack
     * (50×36 = 1800 B) plus ~260 B of strip tables and callee frames —
     * 1024 words was right at the cliff and produced visible candidate
     * corruption on some prefix searches. */
    return xTaskCreate(ime_task_fn, "ime", 2048, nullptr, priority, nullptr)
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

const char *ime_view_text(int *byte_len, int *cursor_bytes) {
    if (byte_len)     *byte_len = g_text_len;
    if (cursor_bytes) *cursor_bytes = g_cursor;
    return g_text;
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

int ime_view_candidate_count(void) {
    return g_ime ? g_ime->candidate_count() : 0;
}

const char *ime_view_candidate(int idx) {
    if (!g_ime || idx < 0 || idx >= g_ime->candidate_count()) return "";
    return g_ime->candidate(idx).word;
}

int ime_view_selected(void) {
    return g_ime ? g_ime->selected() : 0;
}

void ime_view_set_selected(int idx) {
    if (!g_ime || !g_snapshot_mutex) return;
    xSemaphoreTake(g_snapshot_mutex, portMAX_DELAY);
    g_ime->set_selected(idx);
    xSemaphoreGive(g_snapshot_mutex);
    __atomic_add_fetch(&g_ime_dirty_counter, 1u, __ATOMIC_RELEASE);
}

} // extern "C"
