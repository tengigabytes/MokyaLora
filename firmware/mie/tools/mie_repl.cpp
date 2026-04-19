// SPDX-License-Identifier: MIT
// mie_repl.cpp — MokyaInput Engine interactive REPL (PC host only).
//
// Renders a virtual MokyaLora half-keyboard in the terminal and drives
// ImeLogic via its listener API. The listener inserts committed text into
// a local buffer, moves a cursor on DPAD moves when no candidates are
// showing, and deletes before cursor on DEL when pending is empty.
//
// Layout mirrors the physical PCB grouping (see keymap_matrix.h):
//
//   [FUNC]            [ UP   ]                     [SET ] [VOL+]
//   [BACK]  [LEFT ][ OK   ][RIGHT]                 [DEL ] [VOL-]
//                     [DOWN  ]
//
//   [ㄅㄉ ][ˇˋ  ][ㄓˊ ][˙ㄚ ][ㄞㄢㄦ]      Row 0 (1 3 5 7 9)
//   [ㄆㄊ ][ㄍㄐ ][ㄔㄗ ][ㄧㄛ][ㄟㄣ  ]      Row 1 (q e t u o)
//   [ㄇㄋ ][ㄎㄑ ][ㄕㄘ ][ㄨㄜ][ㄠㄤ  ]      Row 2 (a d g j l)
//   [ㄈㄌ ][ㄏㄒ ][ㄖㄙ ][ㄩㄝ][ㄡㄥ  ]      Row 3 (z c b m \)
//   [MODE][TAB  ][SPACE][，  ][。？   ]      Row 4 (` Tab Space [ ])
//
// Two integrated display rows:
//   文字 │  committed-left  [pending]  ▍  committed-right
//   候選 │  1.今  2.金  3.巾 ...                        [1/3]
//
// Pressed keys flash ~180 ms (reverse video) so the user can see which
// hardware key each PC press corresponds to.

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
    const char*     label_zh;       // SmartZh
    const char*     label_en;       // SmartEn (nullptr → fall back to label_zh)
    const char*     label_direct;   // Direct  (nullptr → fall back to label_en / label_zh)
};

// clang-format off
static const KeyLabel kLabels[] = {
    // Row 0 — digits / tone / 注音
    {MOKYA_KEY_1, "ㄅㄉ",    "1/2",  "1/2" },
    {MOKYA_KEY_3, "ˇˋ",     "3/4",  "3/4" },
    {MOKYA_KEY_5, "ㄓˊ",    "5/6",  "5/6" },
    {MOKYA_KEY_7, "˙ㄚ",    "7/8",  "7/8" },
    {MOKYA_KEY_9, "ㄞㄢㄦ", "9/0",  "9/0" },
    // Row 1
    {MOKYA_KEY_Q, "ㄆㄊ", "Q/W", "q/w"},
    {MOKYA_KEY_E, "ㄍㄐ", "E/R", "e/r"},
    {MOKYA_KEY_T, "ㄔㄗ", "T/Y", "t/y"},
    {MOKYA_KEY_U, "ㄧㄛ", "U/I", "u/i"},
    {MOKYA_KEY_O, "ㄟㄣ", "O/P", "o/p"},
    // Row 2
    {MOKYA_KEY_A, "ㄇㄋ", "A/S", "a/s"},
    {MOKYA_KEY_D, "ㄎㄑ", "D/F", "d/f"},
    {MOKYA_KEY_G, "ㄕㄘ", "G/H", "g/h"},
    {MOKYA_KEY_J, "ㄨㄜ", "J/K", "j/k"},
    {MOKYA_KEY_L, "ㄠㄤ", "L",   "l"  },
    // Row 3
    {MOKYA_KEY_Z,         "ㄈㄌ", "Z/X", "z/x"},
    {MOKYA_KEY_C,         "ㄏㄒ", "C/V", "c/v"},
    {MOKYA_KEY_B,         "ㄖㄙ", "B/N", "b/n"},
    {MOKYA_KEY_M,         "ㄩㄝ", "M",   "m"  },
    {MOKYA_KEY_BACKSLASH, "ㄡㄥ", "\\",  "\\" },
    // Row 4 — function bar
    {MOKYA_KEY_MODE,  "MODE",  nullptr, nullptr},
    {MOKYA_KEY_TAB,   "TAB",   nullptr, nullptr},
    {MOKYA_KEY_SPACE, "SPACE", nullptr, nullptr},
    {MOKYA_KEY_SYM1,  "，",    nullptr, nullptr},
    {MOKYA_KEY_SYM2,  "。？",  nullptr, nullptr},
    // Navigation & edit cluster
    {MOKYA_KEY_FUNC, "FUNC", nullptr, nullptr},
    {MOKYA_KEY_BACK, "BACK", nullptr, nullptr},
    {MOKYA_KEY_SET,  "SET",  nullptr, nullptr},
    {MOKYA_KEY_DEL,  "DEL",  nullptr, nullptr},
    // DPAD uses ASCII labels — the Unicode arrows (U+2190..U+2193) have
    // East-Asian-Width "Ambiguous", so different terminals render them at
    // different widths and the cells would drift.
    {MOKYA_KEY_UP,       "UP",   nullptr, nullptr},
    {MOKYA_KEY_DOWN,     "DN",   nullptr, nullptr},
    {MOKYA_KEY_LEFT,     "LT",   nullptr, nullptr},
    {MOKYA_KEY_RIGHT,    "RT",   nullptr, nullptr},
    {MOKYA_KEY_OK,       "OK",   nullptr, nullptr},
    {MOKYA_KEY_VOL_UP,   "VOL+", nullptr, nullptr},
    {MOKYA_KEY_VOL_DOWN, "VOL-", nullptr, nullptr},
};
// clang-format on

// ── Committed text buffer ───────────────────────────────────────────────────

static std::string g_text;
static int         g_cursor = 0;   // byte offset into g_text
static bool        g_dirty  = true;

// ── Key-press highlight state (press flash ≈ 180 ms) ────────────────────────

static constexpr uint32_t kHiliteMs = 180;
static mokya_keycode_t g_hi_kc    = MOKYA_KEY_NONE;
static uint32_t        g_hi_until = 0;

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

    void sync_text_context() {
        if (!ime) return;
        if (g_cursor <= 0 || g_text.empty()) { ime->set_text_context(""); return; }
        int end   = g_cursor;
        int start = end;
        for (int cp = 0; cp < 2 && start > 0; ++cp) {
            --start;
            while (start > 0 && ((unsigned char)g_text[start] & 0xC0) == 0x80) --start;
        }
        std::string suffix = g_text.substr((size_t)start, (size_t)(end - start));
        ime->set_text_context(suffix.c_str());
    }
};

// ── UTF-8 visual width + column-tracking printer ────────────────────────────

static int vw(const char* s) {
    int w = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        if (*p < 0x80)              { w += 1; p += 1; continue; }
        if ((*p & 0xE0) == 0xC0)    { w += 1; p += 2; continue; }   // 2-byte → narrow
        if ((*p & 0xF0) == 0xE0)    { w += 2; p += 3; continue; }   // 3-byte → wide (CJK etc.)
        w += 2; p += 4;                                             // 4-byte → wide
    }
    return w;
}

// Low-level terminal helpers. \033[K clears from cursor to EOL so when a
// frame's content is shorter than the previous frame's, leftover characters
// are erased without a screen-clearing flash.
static void cursor_home() { std::fputs("\033[H", stdout); }
static void erase_eol()   { std::fputs("\033[K", stdout); }

// Track visual column so we can pad cells to fixed positions despite variable
// CJK widths. ANSI escapes are emitted directly and do not count.
static int g_col = 0;
static void put_raw(const char* s) { fputs(s, stdout); g_col += vw(s); }
static void put_ansi(const char* s){ fputs(s, stdout); }              // zero-width
static void pad_to(int target)     { while (g_col < target) { putchar(' '); ++g_col; } }
static void nl()                   { erase_eol(); putchar('\n'); g_col = 0; }

// Print a `[label  ]` cell at visual column `at`, content padded to width
// `content_w`. When `hi` is true the whole bracketed cell is reverse-video.
static void cell(int at, const char* label, int content_w, bool hi) {
    pad_to(at);
    if (hi) put_ansi("\033[7m");
    putchar('['); ++g_col;
    int lw = vw(label);
    fputs(label, stdout); g_col += lw;
    for (int i = lw; i < content_w; ++i) { putchar(' '); ++g_col; }
    putchar(']'); ++g_col;
    if (hi) put_ansi("\033[27m");
}

// ── Label lookup ────────────────────────────────────────────────────────────

static const char* pick_label(const KeyLabel& k, mie::InputMode m) {
    switch (m) {
        case mie::InputMode::SmartEn: return k.label_en ? k.label_en : k.label_zh;
        case mie::InputMode::Direct:
            return k.label_direct ? k.label_direct :
                   (k.label_en    ? k.label_en    : k.label_zh);
        default: return k.label_zh;
    }
}

static const char* label_for(mokya_keycode_t kc, mie::InputMode m) {
    for (const auto& k : kLabels) if (k.keycode == kc) return pick_label(k, m);
    return "?";
}

static bool hi(mokya_keycode_t kc) { return kc == g_hi_kc; }

// ── Rendering ───────────────────────────────────────────────────────────────

// Move cursor home + line-erase helpers. We clear individual lines with
// \033[K as we go and clear any leftover rows below with \033[J at the end
// of render() — that avoids the full-screen blank flash that \033[2J causes
// every frame. (Definitions also appear before nl() above via the header of
// this TU; the duplicate removal happened there so keep this as a comment
// anchor only.)

// Column grid for the nav cluster. 8 vis cols per cell (content 6 + brackets).
//   col  0   : FUNC / BACK                                       (left column)
//   col 12   : LEFT                                              (DPAD west)
//   col 20   : UP / OK / DOWN                                    (DPAD centre)
//   col 28   : RIGHT                                             (DPAD east)
//   col 36   : SET / DEL                                         (edit column)
//   col 44   : VOL+ / VOL-                                       (volume column)
static void render_nav(mie::InputMode mode) {
    g_col = 0;
    // Row A — UP row.
    cell( 0, label_for(MOKYA_KEY_FUNC,   mode), 6, hi(MOKYA_KEY_FUNC));
    cell(20, label_for(MOKYA_KEY_UP,     mode), 6, hi(MOKYA_KEY_UP));
    cell(36, label_for(MOKYA_KEY_SET,    mode), 6, hi(MOKYA_KEY_SET));
    cell(44, label_for(MOKYA_KEY_VOL_UP, mode), 6, hi(MOKYA_KEY_VOL_UP));
    nl();
    // Row B — OK row (full horizontal DPAD).
    cell( 0, label_for(MOKYA_KEY_BACK,     mode), 6, hi(MOKYA_KEY_BACK));
    cell(12, label_for(MOKYA_KEY_LEFT,     mode), 6, hi(MOKYA_KEY_LEFT));
    cell(20, label_for(MOKYA_KEY_OK,       mode), 6, hi(MOKYA_KEY_OK));
    cell(28, label_for(MOKYA_KEY_RIGHT,    mode), 6, hi(MOKYA_KEY_RIGHT));
    cell(36, label_for(MOKYA_KEY_DEL,      mode), 6, hi(MOKYA_KEY_DEL));
    cell(44, label_for(MOKYA_KEY_VOL_DOWN, mode), 6, hi(MOKYA_KEY_VOL_DOWN));
    nl();
    // Row C — DOWN row.
    cell(20, label_for(MOKYA_KEY_DOWN, mode), 6, hi(MOKYA_KEY_DOWN));
    nl();
}

static void render_grid(mie::InputMode mode) {
    static const mokya_keycode_t kGrid[5][5] = {
        {MOKYA_KEY_1,    MOKYA_KEY_3,   MOKYA_KEY_5,     MOKYA_KEY_7,    MOKYA_KEY_9        },
        {MOKYA_KEY_Q,    MOKYA_KEY_E,   MOKYA_KEY_T,     MOKYA_KEY_U,    MOKYA_KEY_O        },
        {MOKYA_KEY_A,    MOKYA_KEY_D,   MOKYA_KEY_G,     MOKYA_KEY_J,    MOKYA_KEY_L        },
        {MOKYA_KEY_Z,    MOKYA_KEY_C,   MOKYA_KEY_B,     MOKYA_KEY_M,    MOKYA_KEY_BACKSLASH},
        {MOKYA_KEY_MODE, MOKYA_KEY_TAB, MOKYA_KEY_SPACE, MOKYA_KEY_SYM1, MOKYA_KEY_SYM2     },
    };
    for (int r = 0; r < 5; ++r) {
        g_col = 0;
        for (int c = 0; c < 5; ++c) {
            cell(c * 8, label_for(kGrid[r][c], mode), 6, hi(kGrid[r][c]));
        }
        nl();
    }
}

// Draws one integrated text line: left-committed + styled pending + cursor
// block + right-committed.  Cursor is an inverse-video space so it's visible
// even at end-of-line.
static void render_text_line(const mie::ImeLogic& ime) {
    mie::PendingView pv = ime.pending_view();

    put_ansi("\033[1m");
    put_raw(" \xe6\x96\x87\xe5\xad\x97 ");   // "文字"
    put_ansi("\033[22m");
    put_raw("\xe2\x94\x82 ");                // "│ "

    // Left committed.
    put_raw(std::string(g_text, 0, (size_t)g_cursor).c_str());

    // Pending, styled.
    if (pv.byte_len > 0 && pv.str) {
        if (pv.style == mie::PendingStyle::Inverted) {
            put_ansi("\033[7m");
            put_raw(pv.str);
            put_ansi("\033[27m");
        } else if (pv.style == mie::PendingStyle::PrefixBold) {
            int mp = pv.matched_prefix_bytes;
            put_ansi("\033[4m");              // underline entire pending
            if (mp > 0 && mp <= pv.byte_len) {
                put_ansi("\033[1m");
                std::string prefix(pv.str, (size_t)mp);
                put_raw(prefix.c_str());
                put_ansi("\033[22m");
                put_raw(pv.str + mp);
            } else {
                put_raw(pv.str);
            }
            put_ansi("\033[24m");
        } else {
            put_raw(pv.str);
        }
    }

    // Cursor block (reverse-video single space).
    put_ansi("\033[7m");
    put_raw(" ");
    put_ansi("\033[27m");

    // Right committed.
    put_raw(std::string(g_text, (size_t)g_cursor).c_str());
    nl();
}

static void render_cand_line(const mie::ImeLogic& ime) {
    put_ansi("\033[1m");
    put_raw(" \xe5\x80\x99\xe9\x81\xb8 ");   // "候選"
    put_ansi("\033[22m");
    put_raw("\xe2\x94\x82 ");                // "│ "

    int cc = ime.candidate_count();
    if (cc == 0) {
        put_ansi("\033[2m");
        put_raw("(\xe6\x8c\x89\xe9\x8d\xb5\xe9\x96\x8b\xe5\xa7\x8b\xe8\xbc\xb8\xe5\x85\xa5)"); // "(按鍵開始輸入)"
        put_ansi("\033[22m");
        nl();
        return;
    }

    int sel      = ime.page_sel();
    int pg       = ime.page();
    int pg_total = ime.page_count();
    int pg_count = ime.page_cand_count();
    for (int i = 0; i < pg_count; ++i) {
        if (i > 0) put_raw("  ");
        bool is_sel = (i == sel);
        if (is_sel) { put_ansi("\033[7m"); put_raw(" "); }
        put_raw(ime.page_cand(i).word);
        if (is_sel) { put_raw(" "); put_ansi("\033[27m"); }
    }
    if (pg_total > 1) {
        put_raw("   ");
        put_ansi("\033[2m");
        char buf[24];
        std::snprintf(buf, sizeof(buf), "[%d/%d]", pg + 1, pg_total);
        put_raw(buf);
        put_ansi("\033[22m");
    }
    nl();
}

static void render_header(const mie::ImeLogic& ime) {
    g_col = 0;
    put_ansi("\033[1m");
    put_raw(" MokyaLora MIE REPL");
    put_ansi("\033[22m");
    pad_to(34);
    put_raw("mode: ");
    put_ansi("\033[7m");
    put_raw(" ");
    put_raw(ime.mode_indicator());
    put_raw(" ");
    put_ansi("\033[27m");
    nl();
}

static void rule(int cols) {
    g_col = 0;
    for (int i = 0; i < cols; ++i) put_raw("\xe2\x94\x80");   // "─"
    nl();
}

// Helper for fixed-text lines: write, clear-to-EOL so leftover chars from an
// older, longer frame are erased, then newline.
static void helpline(const char* s) { std::fputs(s, stdout); erase_eol(); std::fputc('\n', stdout); }

// Static bottom help panel — PC keys → hardware keys.
static void render_help() {
    put_ansi("\033[2m");
    helpline("");
    helpline(" PC \xe9\x8d\xb5\xe7\x9b\xa4\xe7\xb0\xa1\xe5\xaf\xab\xef\xbc\x9a");   // "PC 鍵盤簡寫："
    helpline("   1 3 5 7 9 / q e t u o / a d g j l / z c b m \\   ="
             "   \xe4\xb8\xbb\xe9\x8d\xb5");                                         // "主鍵"
    helpline("   2 4 6 8 0 / w r y i p / s f h k   / x v n , . /  ="
             "   \xe6\xac\xa1\xe9\x8d\xb5\xef\xbc\x88\xe5\x90\x8c\xe6\xa8\xa1\xe7\xb3\x8a\xe5\x8d\x8a\xe9\x8d\xb5\xef\xbc\x89"); // 次鍵（同模糊半鍵）
    helpline("   `  = MODE       Tab = TAB        Space = SPACE       = / _ = VOL+/-");
    helpline("   [  = \xef\xbc\x8cSYM   ]  = \xe3\x80\x82\xef\xbc\x9f   F1 = FUNC   F2 = SET");
    helpline("   Enter = OK       Backspace/Del = DEL     \xe2\x86\x91\xe2\x86\x93\xe2\x86\x90\xe2\x86\x92 = DPAD");
    helpline("   Esc = \xe9\x9b\xa2\xe9\x96\x8b REPL");
    put_ansi("\033[22m");
}

static void render(const mie::ImeLogic& ime) {
    cursor_home();
    render_header(ime);
    rule(56);
    render_nav(ime.mode());
    helpline("");
    render_grid(ime.mode());
    rule(56);
    render_text_line(ime);
    render_cand_line(ime);
    rule(56);
    render_help();
    std::fputs("\033[J", stdout);   // erase anything below the last frame row
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
    // Enable ANSI escape processing on legacy Windows terminals (cmd.exe).
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            SetConsoleMode(hOut, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
        }
    }
#endif
    std::fputs("\033[2J\033[H\033[?25l", stdout);   // one-time clear + hide cursor
    std::fflush(stdout);

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
    listener.ime = &ime;
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
            if (ev.keycode == MOKYA_KEY_NONE) break;   // ESC → quit
            if (ev.keycode == MOKYA_KEY_BACK) break;   // BACK is UI-layer, unreachable on PC

            // Light up the matching hardware cell for a short window.
            g_hi_kc    = ev.keycode;
            g_hi_until = t + kHiliteMs;
            g_dirty    = true;

            LOG("key: kc=0x%02X pressed=%d t=%u\n", (unsigned)ev.keycode, (int)ev.pressed, t);
            ime.process_key(ev);

            // PC stdin emits only key-down; synthesize a release so SYM1 short-
            // press and similar release-edge handlers fire. 1 ms offset keeps
            // the monotonic timestamp strictly increasing across the pair.
            mie::KeyEvent release = ev;
            release.pressed = false;
            release.now_ms  = t + 1;
            ime.process_key(release);
        }

        ime.tick(now_ms());

        // Expire key highlight.
        if (g_hi_kc != MOKYA_KEY_NONE && now_ms() >= g_hi_until) {
            g_hi_kc = MOKYA_KEY_NONE;
            g_dirty = true;
        }

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

    std::fputs("\033[?25h\n", stdout);   // restore cursor
    std::fflush(stdout);

    LOG("main: exit\n");
    log_close();
    return 0;
}
