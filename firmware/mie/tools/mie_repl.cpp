// SPDX-License-Identifier: MIT
// mie_repl.cpp — MokyaInput Engine interactive REPL (PC host only).
//
// Renders a virtual MokyaLora half-keyboard in the terminal and drives
// ImeLogic via its listener API. The listener inserts committed text into
// a local buffer, moves a cursor on DPAD moves when no candidates are
// showing, and deletes before cursor on DEL when pending is empty.
//
// Usage:
//   ./mie_repl [--dat dict_dat.bin]   [--val dict_values.bin]
//              [--en-dat en_dat.bin]  [--en-val en_values.bin]
//
// Controls:
//   Input keys    — append (Smart) / multi-tap cycle (Direct / row-0 in
//                   SmartEn / SYM2)
//   MODE  (`)     — cycle 中 → EN → ABC
//   ，SYM ([)     — short press: ， / , ; long press (500 ms): open picker
//   。.？ ([])    — multi-tap: 。 ？ ！ (ZH) / . ? ! (EN)
//   ←/→           — Smart + candidates: select; otherwise: move text cursor
//   ↑/↓           — Smart + candidates: page; otherwise: move cursor (no-op here)
//   SPACE         — idle: insert space; SmartZh + pending: 1st-tone; others: commit
//   OK  (Enter)   — commit selected candidate / confirm multi-tap
//   DEL           — pending: delete last; empty: delete char before cursor
//   BS (BACK)     — exit REPL (the router contract: BACK is not consumed by MIE)
//   ESC           — exit REPL
//
// Multi-tap / long-press timing is driven by the main loop's tick() call
// using std::chrono::steady_clock as the monotonic ms source.

#include "../hal/pc/hal_pc_stdin.h"
#include "../hal/pc/key_map.h"
#include <mie/ime_logic.h>
#include <mie/keycode.h>
#include <mie/trie_searcher.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/select.h>
#endif

// ── Simple file logger (writes to mie_repl.log) ─────────────────────────────

static FILE* g_log = nullptr;
static void log_open()  { g_log = fopen("mie_repl.log", "w"); if (g_log) { fputs("[mie_repl] log opened\n", g_log); fflush(g_log); } }
static void log_close() { if (g_log) { fputs("[mie_repl] log closed\n", g_log); fclose(g_log); g_log = nullptr; } }
#define LOG(fmt, ...) do { if (g_log) { fprintf(g_log, fmt, ##__VA_ARGS__); fflush(g_log); } } while (0)

// ── Keyboard label table (3 modes) ──────────────────────────────────────────

struct KeyLabel {
    mokya_keycode_t keycode;
    const char*     pc_hint;
    const char*     label_zh;       // SmartZh
    const char*     label_en;       // SmartEn (nullptr → use label_zh)
    const char*     label_direct;   // Direct  (nullptr → use label_en / label_zh)
};

// clang-format off
static const KeyLabel kLabels[] = {
    // Row 0
    {MOKYA_KEY_1, "1",   "ㄅㄉ",  "1/2",  "1/2" }, {MOKYA_KEY_3, "3",  "ˇˋ",    "3/4",  "3/4" },
    {MOKYA_KEY_5, "5",   "ㄓˊ",  "5/6",  "5/6" }, {MOKYA_KEY_7, "7",  "˙ㄚ",   "7/8",  "7/8" },
    {MOKYA_KEY_9, "9/-", "ㄞㄢㄦ","9/0", "9/0" }, {MOKYA_KEY_FUNC, "F1", "FUNC", nullptr, nullptr},
    // Row 1
    {MOKYA_KEY_Q, "q", "ㄆㄊ","Q/W","q/w"}, {MOKYA_KEY_E, "e", "ㄍㄐ","E/R","e/r"},
    {MOKYA_KEY_T, "t", "ㄔㄗ","T/Y","t/y"}, {MOKYA_KEY_U, "u", "ㄧㄛ","U/I","u/i"},
    {MOKYA_KEY_O, "o", "ㄟㄣ","O/P","o/p"}, {MOKYA_KEY_SET, "F2","SET",nullptr,nullptr},
    // Row 2
    {MOKYA_KEY_A, "a",   "ㄇㄋ","A/S","a/s"}, {MOKYA_KEY_D, "d", "ㄎㄑ","D/F","d/f"},
    {MOKYA_KEY_G, "g",   "ㄕㄘ","G/H","g/h"}, {MOKYA_KEY_J, "j", "ㄨㄜ","J/K","j/k"},
    {MOKYA_KEY_L, "l/;", "ㄠㄤ","L",  "l"   }, {MOKYA_KEY_BACK,"BS","BACK",nullptr,nullptr},
    // Row 3
    {MOKYA_KEY_Z,"z",     "ㄈㄌ","Z/X","z/x"}, {MOKYA_KEY_C,"c", "ㄏㄒ","C/V","c/v"},
    {MOKYA_KEY_B,"b",     "ㄖㄙ","B/N","b/n"}, {MOKYA_KEY_M,"m/,","ㄩㄝ","M",  "m"   },
    {MOKYA_KEY_BACKSLASH,"\\/.","ㄡㄥ","—", "—"}, {MOKYA_KEY_DEL,"Del","DEL",nullptr,nullptr},
    // Row 4
    {MOKYA_KEY_MODE,"`",    "MODE",nullptr,nullptr}, {MOKYA_KEY_TAB,"Tab","TAB",nullptr,nullptr},
    {MOKYA_KEY_SPACE,"Spc", "SPACE",nullptr,nullptr},
    {MOKYA_KEY_SYM1,"[",    "，SYM",nullptr,nullptr}, {MOKYA_KEY_SYM2,"]","。？",nullptr,nullptr},
    {MOKYA_KEY_VOL_UP,"=",  "VOL+",nullptr,nullptr},
    // Row 5
    {MOKYA_KEY_UP,  "\xe2\x86\x91","UP",  nullptr,nullptr},
    {MOKYA_KEY_DOWN,"\xe2\x86\x93","DOWN",nullptr,nullptr},
    {MOKYA_KEY_LEFT,"\xe2\x86\x90","LEFT",nullptr,nullptr},
    {MOKYA_KEY_RIGHT,"\xe2\x86\x92","RIGHT",nullptr,nullptr},
    {MOKYA_KEY_OK,  "\xe2\x8f\x8e","OK",  nullptr,nullptr},
    {MOKYA_KEY_VOL_DOWN,"_","VOL-",nullptr,nullptr},
};
// clang-format on

// ── Committed text buffer ───────────────────────────────────────────────────

static std::string g_text;
static int         g_cursor = 0;   // byte offset into g_text
static bool        g_dirty  = true;

static void cursor_move_left() {
    if (g_cursor <= 0) return;
    --g_cursor;
    while (g_cursor > 0 && ((unsigned char)g_text[g_cursor] & 0xC0) == 0x80) --g_cursor;
}
static void cursor_move_right() {
    int n = (int)g_text.size();
    if (g_cursor >= n) return;
    ++g_cursor;
    while (g_cursor < n && ((unsigned char)g_text[g_cursor] & 0xC0) == 0x80) ++g_cursor;
}
static void cursor_delete_before() {
    if (g_cursor <= 0) return;
    int end = g_cursor;
    cursor_move_left();
    g_text.erase((size_t)g_cursor, (size_t)(end - g_cursor));
}

// ── Listener — wire MIE events into the local text buffer ──────────────────

struct ReplListener : public mie::IImeListener {
    mie::ImeLogic* ime = nullptr;   // back-pointer for context sync

    void on_commit(const char* utf8) override {
        if (!utf8 || !*utf8) return;
        int len = (int)std::strlen(utf8);
        g_text.insert((size_t)g_cursor, utf8, (size_t)len);
        g_cursor += len;
        g_dirty = true;
        LOG("listener: commit \"%s\" (cursor -> %d)\n", utf8, g_cursor);
    }

    void on_cursor_move(mie::NavDir d) override {
        switch (d) {
            case mie::NavDir::Left:  cursor_move_left();  break;
            case mie::NavDir::Right: cursor_move_right(); break;
            case mie::NavDir::Up:
            case mie::NavDir::Down:
                break;   // single-line REPL: nothing to do
        }
        sync_text_context();
        g_dirty = true;
        LOG("listener: cursor_move dir=%d -> %d\n", (int)d, g_cursor);
    }

    void on_delete_before() override {
        cursor_delete_before();
        sync_text_context();
        g_dirty = true;
        LOG("listener: delete_before -> cursor %d, len %zu\n", g_cursor, g_text.size());
    }

    void on_composition_changed() override { g_dirty = true; }

    // Feed MIE the last two codepoints immediately before the cursor so
    // its SmartEn leading-space / cap-next flags match the real text
    // state after external edits. Walks two UTF-8 codepoints back from
    // the cursor (enough to see ". ", ", ", 「。」, …).
    void sync_text_context() {
        if (!ime) return;
        if (g_cursor <= 0 || g_text.empty()) {
            ime->set_text_context("");
            return;
        }
        int end   = g_cursor;
        int start = end;
        for (int cp = 0; cp < 2 && start > 0; ++cp) {
            --start;
            while (start > 0 &&
                   ((unsigned char)g_text[start] & 0xC0) == 0x80) {
                --start;
            }
        }
        std::string suffix = g_text.substr((size_t)start, (size_t)(end - start));
        ime->set_text_context(suffix.c_str());
    }
};

// ── Rendering ───────────────────────────────────────────────────────────────

static void clear_screen() { std::fputs("\033[2J\033[H", stdout); }

static const char* pick_label(const KeyLabel& k, mie::InputMode m) {
    switch (m) {
        case mie::InputMode::SmartEn: return k.label_en ? k.label_en : k.label_zh;
        case mie::InputMode::Direct:
            return k.label_direct ? k.label_direct :
                   (k.label_en    ? k.label_en    : k.label_zh);
        default: return k.label_zh;
    }
}

static void render(const mie::ImeLogic& ime) {
    clear_screen();

    const char* top = "\xe2\x94\x8c\xe2\x94\x80 MokyaLora REPL \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90";
    const char* mid = "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xa4";
    const char* bot = "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x98";

    std::puts(top);
    for (int row = 0; row < 6; ++row) {
        std::fputs("\xe2\x94\x82 ", stdout);
        for (int col = 0; col < 6; ++col) {
            mokya_keycode_t target = (mokya_keycode_t)(row * 6 + col + 1);
            const char* pc = "?"; const char* lb = "?";
            for (const auto& k : kLabels) {
                if (k.keycode == target) { pc = k.pc_hint; lb = pick_label(k, ime.mode()); break; }
            }
            std::printf("[%3s:%-5s]", pc, lb);
        }
        std::puts(" \xe2\x94\x82");
    }
    std::puts(mid);

    // Pending composition with style hint.
    mie::PendingView pv = ime.pending_view();
    char input_display[512] = {};
    if (pv.byte_len == 0) {
        std::snprintf(input_display, sizeof(input_display),
                      "(\xe6\x8c\x89\xe4\xb8\x8b\xe9\x8d\xb5\xe5\x85\xa5)");  // (按下鍵入)
    } else if (pv.style == mie::PendingStyle::Inverted) {
        // Reverse video for whole pending.
        std::snprintf(input_display, sizeof(input_display), "\033[7m%s\033[27m", pv.str);
    } else if (pv.style == mie::PendingStyle::PrefixBold &&
               pv.matched_prefix_bytes > 0 && pv.matched_prefix_bytes < pv.byte_len) {
        std::snprintf(input_display, sizeof(input_display),
                      "\033[1m%.*s\033[22m%s",
                      pv.matched_prefix_bytes, pv.str,
                      pv.str + pv.matched_prefix_bytes);
    } else if (pv.style == mie::PendingStyle::PrefixBold) {
        // Whole string is matched — all bold.
        std::snprintf(input_display, sizeof(input_display), "\033[1m%s\033[22m", pv.str);
    } else {
        std::snprintf(input_display, sizeof(input_display), "%s", pv.str);
    }
    std::printf("\xe2\x94\x82 \xe6\xa8\xa1\xe5\xbc\x8f: %s  \xe8\xbc\xb8\xe5\x85\xa5: %-60s \xe2\x94\x82\n",
                ime.mode_indicator(), input_display);

    // Candidate list (current page only).
    int cc = ime.candidate_count();
    if (cc > 0) {
        int pg       = ime.page();
        int pg_total = ime.page_count();
        int pg_count = ime.page_cand_count();
        int sel_in_page = ime.page_sel();
        char cand_buf[512] = {}; int pos = 0;
        for (int i = 0; i < pg_count && pos < 460; ++i) {
            bool sel = (i == sel_in_page);
            int n = std::snprintf(cand_buf + pos, (int)sizeof(cand_buf) - pos,
                                  "%s%s ", sel ? ">" : " ",
                                  ime.page_cand(i).word);
            if (n > 0) pos += n;
        }
        if (pg_total > 1) {
            int n = std::snprintf(cand_buf + pos, (int)sizeof(cand_buf) - pos,
                                  " [%d/%d]", pg + 1, pg_total);
            if (n > 0) pos += n;
        }
        const char* tag = (ime.mode() == mie::InputMode::SmartZh) ? "[\xe4\xb8\xad\xe6\x96\x87]"
                         : "[English]";
        std::printf("\xe2\x94\x82 %s %-56s \xe2\x94\x82\n", tag, cand_buf);
    } else {
        std::printf("\xe2\x94\x82 %-60s \xe2\x94\x82\n", "");
    }

    // Committed text with cursor shown as '|'.
    {
        std::string disp;
        disp.reserve(g_text.size() + 3);
        disp.append(g_text, 0, (size_t)g_cursor);
        disp += '|';
        disp.append(g_text, (size_t)g_cursor);
        std::printf("\xe2\x94\x82 \xe5\xb7\xb2\xe7\xa2\xba\xe8\xaa\x8d: %-50s \xe2\x94\x82\n",
                    disp.c_str());
    }

    std::puts(bot);
    std::puts("  ESC \xe9\x9b\xa2\xe9\x96\x8b  |  `=MODE  |  "
              "\xe2\x86\x90\xe2\x86\x92=\xe5\x80\x99\xe9\x81\xb8/\xe6\xb8\xb8\xe6\xa8\x99  |  "
              "\xe2\x86\x91\xe2\x86\x93=\xe7\xbf\xbb\xe9\xa0\x81  |  "
              "[=\xef\xbc\x8c/, (\xe9\x95\xb7\xe6\x8c\x89 picker)  |  "
              "]=\xe3\x80\x82\xef\xbc\x9f/.?!  |  "
              "BS/Del=\xe5\x88\xaa\xe5\xad\x97  |  Spc=\xe7\xa9\xba\xe6\xa0\xbc/\xe4\xb8\x80\xe8\x81\xb2");
    std::fflush(stdout);
}

// ── Arg parse ───────────────────────────────────────────────────────────────

static const char* find_arg(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    return nullptr;
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    log_open();
    LOG("main: startup\n");

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    const char* zh_dat = find_arg(argc, argv, "--dat");
    const char* zh_val = find_arg(argc, argv, "--val");
    const char* en_dat = find_arg(argc, argv, "--en-dat");
    const char* en_val = find_arg(argc, argv, "--en-val");

    mie::TrieSearcher zh_searcher;
    if (zh_dat && zh_val) {
        if (zh_searcher.load_from_file(zh_dat, zh_val))
            std::printf("\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe5\x85\xb8: %u keys (v%u)\n",
                        zh_searcher.key_count(), zh_searcher.dict_version());
        else
            std::fprintf(stderr, "WARNING: failed to load Chinese dict '%s'/'%s'\n", zh_dat, zh_val);
    }

    mie::TrieSearcher en_searcher;
    bool en_loaded = false;
    if (en_dat && en_val) {
        if (en_searcher.load_from_file(en_dat, en_val)) {
            std::printf("English dict: %u keys\n", en_searcher.key_count());
            en_loaded = true;
        } else {
            std::fprintf(stderr, "WARNING: failed to load English dict '%s'/'%s'\n", en_dat, en_val);
        }
    }

    mie::ImeLogic ime(zh_searcher, en_loaded ? &en_searcher : nullptr);
    ReplListener listener;
    listener.ime = &ime;       // enable set_text_context sync after edits
    ime.set_listener(&listener);
    LOG("main: ime + listener wired\n");

    mie::pc::HalPcStdin hal;
    auto t0 = std::chrono::steady_clock::now();
    auto now_ms = [&]() -> uint32_t {
        auto d = std::chrono::steady_clock::now() - t0;
        return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    };

    render(ime);
    g_dirty = false;
    LOG("main: entering event loop\n");

    for (;;) {
        uint32_t t = now_ms();
        mie::KeyEvent ev;
        if (hal.poll(ev, t)) {
            // ESC → quit.
            if (ev.keycode == MOKYA_KEY_NONE) break;
            // BACK is reserved for UI layer; use it to exit the REPL.
            if (ev.keycode == MOKYA_KEY_BACK) break;

            LOG("key: kc=0x%02X pressed=%d t=%u\n",
                (unsigned)ev.keycode, (int)ev.pressed, t);
            ime.process_key(ev);

            // PC terminals only report key-down via _getch / termios raw
            // reads — there is no natural key-release event. Synthesize
            // one here so MIE handlers that rely on the release edge
            // (SYM1 short-press vs long-press) actually fire. 1 ms offset
            // keeps now_ms monotonic across the pair. Long-press testing
            // is not reachable from the PC terminal by design (the press
            // is already "released" by the time tick() could see it); the
            // full long-press path is intended for real hardware input.
            mie::KeyEvent release = ev;
            release.pressed = false;
            release.now_ms  = t + 1;
            ime.process_key(release);
        }

        // Drive timers (multi-tap timeout / SYM1 long-press).
        ime.tick(now_ms());

        if (g_dirty) {
            render(ime);
            g_dirty = false;
        }

#ifdef _WIN32
        Sleep(10);
#else
        struct timeval tv = {0, 10000};
        select(0, nullptr, nullptr, nullptr, &tv);
#endif
    }

    LOG("main: exit\n");
    log_close();
    return 0;
}
