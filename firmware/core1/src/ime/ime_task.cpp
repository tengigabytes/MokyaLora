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
#include "lru_persist.h"
#include "mokya_trace.h"
#include "view_router.h"

#include <cstring>
#include <new>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "hardware/xip_cache.h"

#include <mie/ime_logic.h>
#include <mie/trie_searcher.h>
#include <mie/composition_searcher.h>

#include "key_event.h"

namespace {

/* ── Static state ─────────────────────────────────────────────────── */

mie::TrieSearcher        g_zh_searcher;
mie::TrieSearcher        g_en_searcher;
mie::CompositionSearcher g_v4_searcher;     /* attached when v4 dict loaded */

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
/* Phase 1.6: commit counter drives LRU persist throttling. Incremented on
 * every on_commit that actually inserts text (not cursor/delete events).
 * Consumed by the throttle loop in ime_task_fn. */
volatile uint32_t g_commit_count   = 0;
volatile bool     g_lru_save_dirty = false;

class IMEListener : public mie::IImeListener {
public:
    mie::ImeLogic *ime = nullptr;       /* back-pointer for sync_text_context */

    void on_commit(const char *utf8) override {
        if (!utf8 || !*utf8) return;
        insert_at_cursor(utf8, std::strlen(utf8));
        /* Track commit count for LRU persist throttle. */
        g_commit_count++;
        g_lru_save_dirty = true;
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

/* LRU persist throttle parameters. Plan: save every 50 commits, on mode
 * cycle, or after 30 s of idle with pending dirty state. A save takes two
 * 4 KB flash sector erases plus a ~6 KB page program — roughly 50 ms
 * with Core 0 parked, well below keypress latency. */
constexpr uint32_t kLruSaveEveryNCommits = 50u;
constexpr uint32_t kLruSaveIdleMs        = 30u * 1000u;

void ime_task_fn(void *) {
    uint32_t last_committed_count  = 0;
    uint32_t last_commit_ms        = now_ms();
    uint8_t  last_mode_byte        = ime_view_mode_byte();

    for (;;) {
        key_event_t ev{};
        bool got_ev = key_event_pop(&ev, pdMS_TO_TICKS(kTickMs));

        /* Hold the snapshot mutex across the full process_key()/tick()
         * call so LVGL readers never observe candidates_[] mid-rebuild.
         * Listener callbacks (on_commit, on_cursor_move, on_delete_before)
         * execute within this critical section without re-taking the
         * mutex — they mutate the text buffer and call set_text_context
         * on the same thread. */
        if (got_ev) {
            TRACE("ime", "key_pop", "kc=0x%02x,p=%u",
                  (unsigned)ev.keycode, (unsigned)ev.pressed);
        }
        xSemaphoreTake(g_snapshot_mutex, portMAX_DELAY);
        if (got_ev) {
            mie::KeyEvent mev;
            mev.keycode = (mokya_keycode_t)ev.keycode;
            mev.pressed = ev.pressed != 0;
            mev.now_ms  = now_ms();
            mev.flags   = (uint8_t)ev.flags;

            TRACE("ime", "proc_start", "kc=0x%02x", (unsigned)ev.keycode);
            g_ime->process_key(mev);
            TRACE_BARE("ime", "proc_end");
        }
        TRACE_BARE("ime", "tick_start");
        g_ime->tick(now_ms());
        TRACE_BARE("ime", "tick_end");
        xSemaphoreGive(g_snapshot_mutex);

        if (got_ev) {
            __atomic_add_fetch(&g_ime_dirty_counter, 1u, __ATOMIC_RELEASE);
            TRACE_BARE("ime", "done");
        }

        /* ── Phase 1.6: LRU persist throttle ─────────────────────────── *
         * Trigger a save on any of:
         *   - kLruSaveEveryNCommits since the last save
         *   - mode cycled (user made an explicit session change)
         *   - kLruSaveIdleMs without new commits, with dirty state
         *
         * All three are quiet signals that the user is "between thoughts"
         * so the 50-ish ms flash stall doesn't interrupt a typing burst.
         * We take the snapshot mutex to keep ImeLogic state stable while
         * serialise runs. */
        uint32_t commits_now = g_commit_count;
        uint32_t now         = now_ms();
        uint8_t  mode_now    = ime_view_mode_byte();

        if (commits_now != last_committed_count) {
            last_commit_ms = now;
        }
        bool commit_tripwire   = (commits_now - last_committed_count) >= kLruSaveEveryNCommits;
        bool mode_tripwire     = (mode_now != last_mode_byte);
        bool idle_tripwire     = g_lru_save_dirty &&
                                 (now - last_commit_ms) >= kLruSaveIdleMs;

        if (commit_tripwire || mode_tripwire || idle_tripwire) {
            TRACE("ime", "lru_save", "reason=%u,commits=%u",
                  (unsigned)(commit_tripwire ? 0 : (mode_tripwire ? 1 : 2)),
                  (unsigned)commits_now);
            xSemaphoreTake(g_snapshot_mutex, portMAX_DELAY);
            /* serialise + flash op happens here — Core 0 will be parked
             * by flash_safety_wrap.c for the duration. */
            (void)lru_persist_save(g_ime);
            xSemaphoreGive(g_snapshot_mutex);
            TRACE_BARE("ime", "lru_save_done");
            last_committed_count = commits_now;
            last_mode_byte       = mode_now;
            g_lru_save_dirty     = false;
        }
    }
}

} // namespace

/* ── Public API ──────────────────────────────────────────────────── */

extern "C" {

volatile uint32_t g_ime_dirty_counter = 0;

bool ime_task_start(const mie_dict_pointers_t *dict, UBaseType_t priority) {
    if (!dict) return false;

    /* Path A: MIED v4 (composition dict) — single-blob; preferred when present.
     * v2 sections may also be loaded for the legacy fallback / English
     * dictionary, but if v4 is attached run_search uses the composition
     * engine and ignores TrieSearcher state. */
    bool have_v4 = (dict->v4_blob && dict->v4_blob_size > 0);
    bool v4_loaded = false;
    if (have_v4) {
        v4_loaded = g_v4_searcher.load_from_memory(dict->v4_blob,
                                                    dict->v4_blob_size);
    }

    /* Path B: MDBL v2 — the original two-section format. Required when v4
     * is absent so SmartZh still has a Chinese dictionary; also provides
     * the English dict (v4 currently has no English support). */
    bool have_zh_v2 = (dict->zh_dat && dict->zh_dat_size > 0 &&
                      dict->zh_val && dict->zh_val_size > 0);
    if (have_zh_v2) {
        if (!g_zh_searcher.load_from_memory(dict->zh_dat, dict->zh_dat_size,
                                            dict->zh_val, dict->zh_val_size)) {
            have_zh_v2 = false;
        }
    }

    /* Need at least one Chinese dict path. */
    if (!v4_loaded && !have_zh_v2) {
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

    /* When v4 is loaded, attach it; ImeLogic.run_search will route SmartZh
     * to the composition engine. The legacy zh_searcher remains as a
     * structural prerequisite of the constructor (reference, not optional). */
    if (v4_loaded) {
        g_ime->attach_composition_searcher(&g_v4_searcher);
    }

    g_listener.ime = g_ime;
    g_ime->set_listener(&g_listener);

    /* Phase 1.6: allocate the save-path flash-page scratch buffer while
     * the FreeRTOS heap is still contiguous; later throttled saves run at
     * arbitrary times when fragmentation could starve the 6400 B request.
     * The reserve is owned for the lifetime of the image — on a 48 KB heap
     * this still leaves headroom well above the 20 % panic threshold. */
    if (!lru_persist_init()) return false;

    /* Best-effort restore of the personalised LRU cache from flash.
     * An unprogrammed (all 0xFF) partition returns false here and the
     * engine runs with an empty cache — same as first-boot behaviour. */
    lru_persist_load(g_ime);

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

void ime_view_clear_text(void) {
    if (!ime_view_lock(pdMS_TO_TICKS(20))) return;
    g_text_len = 0;
    g_cursor   = 0;
    g_text[0]  = '\0';
    ime_view_unlock();
    /* Bump dirty so the IME view's gated refresh repaints the cleared
     * commit buffer on its next tick. */
    __atomic_add_fetch(&g_ime_dirty_counter, 1u, __ATOMIC_RELEASE);
}

const char *ime_view_mode_indicator(void) {
    return g_ime ? g_ime->mode_indicator() : "";
}

uint8_t ime_view_mode_byte(void) {
    if (!g_ime) return 0xFFu;
    switch (g_ime->mode()) {
        case mie::InputMode::SmartZh: return 0u;
        case mie::InputMode::SmartEn: return 1u;
        case mie::InputMode::Direct:  return 2u;
    }
    return 0xFFu;
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

bool ime_view_picker_active(void) {
    return g_ime && g_ime->picker_active();
}
int ime_view_picker_cell_count(void) {
    return g_ime ? g_ime->picker_cell_count() : 0;
}
int ime_view_picker_cols(void) {
    return g_ime ? g_ime->picker_cols() : 0;
}
const char *ime_view_picker_cell(int idx) {
    return (g_ime && idx >= 0 && idx < g_ime->picker_cell_count())
           ? g_ime->picker_cell(idx) : "";
}
int ime_view_picker_selected(void) {
    return g_ime ? g_ime->picker_selected() : 0;
}

/* ── Generic text-input request (post-Stage 3) ──────────────────────── *
 *
 * One request in flight at a time; rejected with `false` if re-entered.
 * IME view index in the view_router table is fixed at 3 (matches
 * view_router.c init order — cross-checked by settings_view).
 */

#define IME_REQUEST_VIEW_INDEX  3

struct text_request_state_t {
    bool             active;
    uint16_t         max_bytes;
    uint8_t          flags;
    ime_text_done_fn done;
    void            *ctx;
};
static text_request_state_t s_text_req = {false, 0, 0, nullptr, nullptr};

/* Walk back from `max_bytes` until we land on a UTF-8 codepoint
 * boundary (any byte whose top two bits are NOT 10b). Returns the
 * largest length ≤ min(len, max_bytes) that ends cleanly. */
static int utf8_truncate_clean(const char *s, int len, int max_bytes)
{
    if (len <= max_bytes) return len;
    int i = max_bytes;
    while (i > 0 && (((unsigned char)s[i]) & 0xC0u) == 0x80u) i--;
    return i;
}

static void seed_text_unsafe(const char *utf8, int byte_len)
{
    /* Caller must hold g_snapshot_mutex. */
    if (byte_len < 0) byte_len = 0;
    if (byte_len > (int)kTextCapacity) byte_len = (int)kTextCapacity;
    if (byte_len > 0 && utf8 != NULL) {
        std::memcpy(g_text, utf8, (size_t)byte_len);
    }
    g_text_len = byte_len;
    g_cursor   = byte_len;
    g_text[g_text_len] = '\0';
}

static void modal_trampoline(bool committed, void *ctx)
{
    (void)ctx;   /* user ctx is in s_text_req.ctx, not this */
    if (!s_text_req.active) return;

    /* Snapshot user state then clear the slot so the callback can
     * legitimately call ime_request_text again (one-shot chain). */
    ime_text_done_fn done       = s_text_req.done;
    void            *user_ctx   = s_text_req.ctx;
    uint16_t         max_bytes  = s_text_req.max_bytes;
    s_text_req.active = false;
    s_text_req.done   = nullptr;
    s_text_req.ctx    = nullptr;

    int len = g_text_len;
    if (max_bytes > 0 && len > (int)max_bytes) {
        len = utf8_truncate_clean(g_text, len, (int)max_bytes);
    }

    if (done) done(committed, g_text, (uint16_t)(len < 0 ? 0 : len), user_ctx);

    /* Always clear so the next request starts fresh; mirrors the
     * historical messages_send + Stage 3 behaviour. Acquires the
     * mutex via the public helper. */
    ime_view_clear_text();
}

bool ime_request_text_active(void)
{
    return s_text_req.active;
}

bool ime_request_text(const ime_text_request_t *req,
                     ime_text_done_fn          done,
                     void                     *ctx)
{
    if (req == nullptr || done == nullptr)        return false;
    if (s_text_req.active)                        return false;
    if (g_snapshot_mutex == nullptr)              return false;

    /* Pre-fill g_text under the mutex. ime_view_clear_text takes the
     * mutex itself, so we don't call it here; instead seed_text_unsafe
     * runs inside our own lock. */
    if (xSemaphoreTake(g_snapshot_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    int initial_len = 0;
    if (req->initial != nullptr) {
        initial_len = (int)std::strlen(req->initial);
        if (req->max_bytes > 0 && initial_len > (int)req->max_bytes) {
            initial_len = utf8_truncate_clean(req->initial, initial_len,
                                              (int)req->max_bytes);
        }
    }
    seed_text_unsafe(req->initial, initial_len);
    xSemaphoreGive(g_snapshot_mutex);
    /* Bump the dirty counter so the IME view's gated refresh repaints
     * the seeded prefill on first entry. */
    __atomic_add_fetch(&g_ime_dirty_counter, 1u, __ATOMIC_RELEASE);

    s_text_req.max_bytes = req->max_bytes;
    s_text_req.flags     = req->flags;
    s_text_req.done      = done;
    s_text_req.ctx       = ctx;
    s_text_req.active    = true;

    view_router_modal_enter(IME_REQUEST_VIEW_INDEX, modal_trampoline, nullptr);
    return true;
}

} // extern "C"
