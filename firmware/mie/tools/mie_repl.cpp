// mie_repl.cpp — MokyaInput Engine interactive REPL (PC host only)
// SPDX-License-Identifier: MIT
//
// Renders a virtual MokyaLora half-keyboard in the terminal and passes key
// events through ImeLogic → TrieSearcher, displaying candidates in real time.
//
// Usage:
//   ./mie_repl [--dat dict_dat.bin] [--val dict_values.bin]
//              [--en-dat en_dat.bin] [--en-val en_values.bin]
//
// Without dictionary arguments the REPL runs but shows no candidates.
//
// Controls (PC keys shown in the virtual keyboard grid):
//   Input keys  — append to key sequence (SmartZh/SmartEn) / cycle label (Direct)
//   BACK (BS)   — backspace (Smart modes) / cancel pending (Direct)
//                 When no IME input pending: delete character before cursor
//   DEL         — When no IME input pending: delete character at cursor
//   MODE  (`)   — cycle 中→EN→ABC→abc→ㄅ, clear input
//   ，SYM (,)   — cycle Chinese/English comma/punctuation group
//   。.？ (.)   — cycle Chinese/English period/punctuation group
//   ←/→         — navigate candidates (IME active) / move cursor in committed text
//   SPACE / OK  — commit
//   ESC         — quit

#include "../hal/pc/hal_pc_stdin.h"
#include "../hal/pc/key_map.h"
#include <mie/ime_logic.h>
#include <mie/trie_searcher.h>

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/select.h>
#endif

// ── Simple file logger (writes to mie_repl.log beside the executable) ────────
// Helps diagnose crashes: if the program crashes the log shows the last entry.

static FILE* g_log = nullptr;

static void log_open() {
    g_log = fopen("mie_repl.log", "w");
    if (g_log) { fputs("[mie_repl] log opened\n", g_log); fflush(g_log); }
}

static void log_close() {
    if (g_log) { fputs("[mie_repl] log closed\n", g_log); fclose(g_log); g_log = nullptr; }
}

// LOG(fmt, ...) — only active when g_log is open; always flushes so the last
// entry is visible even if the process crashes immediately after.
#define LOG(fmt, ...) \
    do { if (g_log) { fprintf(g_log, fmt, ##__VA_ARGS__); fflush(g_log); } } while(0)


// label_zh:    shown in SmartZh / DirectBopomofo mode.
// label_en:    shown in SmartEn / DirectUpper mode (nullptr = same as label_zh).
// label_lower: shown in DirectLower mode (nullptr = same as label_en).
struct KeyLabel {
    uint8_t row; uint8_t col;
    const char* pc_hint;
    const char* label_zh;
    const char* label_en;
    const char* label_lower;
};

// clang-format off
static const KeyLabel kLabels[] = {
    // Row 0: tone/digit keys — digits in all direct modes
    {0,0,"1",  "ㄅㄉ","1/2","1/2" },{0,1,"3",  "ˇˋ", "3/4","3/4" },{0,2,"5",  "ㄓˊ","5/6","5/6" },
    {0,3,"7",  "˙ㄚ", "7/8","7/8" },{0,4,"9",  "ㄞㄢ","9/0","9/0" },{0,5,"F1", "FUNC",nullptr,nullptr},
    // Row 1: ㄆㄊ=QW  ㄍㄐ=ER  ㄔㄗ=TY  ㄧㄛ=UI  ㄟㄣ=OP
    {1,0,"q",  "ㄆㄊ","Q/W","q/w"},{1,1,"e",  "ㄍㄐ","E/R","e/r"},{1,2,"t",  "ㄔㄗ","T/Y","t/y"},
    {1,3,"u",  "ㄧㄛ","U/I","u/i"},{1,4,"o",  "ㄟㄣ","O/P","o/p"},{1,5,"F2", "SET", nullptr,nullptr},
    // Row 2: ㄇㄋ=AS  ㄎㄑ=DF  ㄕㄘ=GH  ㄨㄜ=JK  ㄠㄤ=L
    {2,0,"a",  "ㄇㄋ","A/S","a/s"},{2,1,"d",  "ㄎㄑ","D/F","d/f"},{2,2,"g",  "ㄕㄘ","G/H","g/h"},
    {2,3,"j",  "ㄨㄜ","J/K","j/k"},{2,4,"l",  "ㄠㄤ","L",  "l"  },{2,5,"BS", "BACK",nullptr,nullptr},
    // Row 3: ㄈㄌ=ZX  ㄏㄒ=CV  ㄖㄙ=BN  ㄩㄝ=M  ㄡㄥ=—
    {3,0,"z",  "ㄈㄌ","Z/X","z/x"},{3,1,"c",  "ㄏㄒ","C/V","c/v"},{3,2,"b",  "ㄖㄙ","B/N","b/n"},
    {3,3,"m",  "ㄩㄝ","M",  "m"  },{3,4,"\\", "ㄡㄥ","—",  "—"  },{3,5,"Del","DEL", nullptr,nullptr},
    // Rows 4-5: same in all modes
    {4,0,"`",  "MODE",nullptr,nullptr},{4,1,"Tab","TAB",nullptr,nullptr},{4,2,"Spc","SPACE",nullptr,nullptr},
    {4,3,",",  "，SYM",nullptr,nullptr},{4,4,".","。？",nullptr,nullptr},{4,5,"=","VOL+",nullptr,nullptr},
    {5,0,"\xe2\x86\x91","UP",  nullptr,nullptr},{5,1,"\xe2\x86\x93","DOWN", nullptr,nullptr},
    {5,2,"\xe2\x86\x90","LEFT",nullptr,nullptr},{5,3,"\xe2\x86\x92","RIGHT",nullptr,nullptr},
    {5,4,"\xe2\x8f\x8e","OK",  nullptr,nullptr},{5,5,"-","VOL-",nullptr,nullptr},
};
// clang-format on


// ── Committed output buffer & cursor ─────────────────────────────────────

static std::string g_committed;
static int         g_cursor = 0;  // byte offset into g_committed

static void on_commit(const char* utf8, void* /*ctx*/) {
    if (utf8 && *utf8) {
        int len = (int)strlen(utf8);
        g_committed.insert((size_t)g_cursor, utf8, (size_t)len);
        g_cursor += len;
    }
}

// UTF-8 helpers for cursor movement.
// Move cursor left by one codepoint (skip over continuation bytes 0x80-0xBF).
static void cursor_move_left() {
    if (g_cursor <= 0) return;
    --g_cursor;
    while (g_cursor > 0 && ((unsigned char)g_committed[g_cursor] & 0xC0) == 0x80)
        --g_cursor;
}

// Move cursor right by one codepoint.
static void cursor_move_right() {
    int n = (int)g_committed.size();
    if (g_cursor >= n) return;
    ++g_cursor;
    while (g_cursor < n && ((unsigned char)g_committed[g_cursor] & 0xC0) == 0x80)
        ++g_cursor;
}

// Delete codepoint before the cursor (backspace).
static void cursor_backspace() {
    if (g_cursor <= 0) return;
    int end = g_cursor;
    cursor_move_left();
    g_committed.erase((size_t)g_cursor, (size_t)(end - g_cursor));
}

// Delete codepoint at the cursor (forward delete).
static void cursor_delete() {
    int n = (int)g_committed.size();
    if (g_cursor >= n) return;
    int start = g_cursor;
    int pos = start + 1;
    while (pos < n && ((unsigned char)g_committed[pos] & 0xC0) == 0x80) ++pos;
    g_committed.erase((size_t)start, (size_t)(pos - start));
}

// ── Rendering ────────────────────────────────────────────────────────────

static void clear_screen() { fputs("\033[2J\033[H", stdout); }

static const char* kCircle[] = {
    "\xe2\x91\xa0","\xe2\x91\xa1","\xe2\x91\xa2","\xe2\x91\xa3","\xe2\x91\xa4",
};
static constexpr int kNumCircles = 5;  // kCircle array size — never index beyond this

static void render(const mie::ImeLogic& ime) {
    LOG("render: mode=%d zh=%d en=%d merged=%d mc_sel=%d prefix=%d input_bytes=%d\n",
        (int)ime.mode(), ime.zh_candidate_count(), ime.en_candidate_count(),
        ime.merged_candidate_count(), ime.candidate_index(),
        ime.matched_prefix_len(), ime.input_bytes());
    clear_screen();

    // ── Keyboard grid ──────────────────────────────────────────────────────
    puts("\xe2\x94\x8c\xe2\x94\x80 MokyaLora REPL \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90");
    for (int row = 0; row < 6; ++row) {
        fputs("\xe2\x94\x82 ", stdout);
        for (int col = 0; col < 6; ++col) {
            const char* pc = "?"; const char* lb = "?";
            for (const auto& k : kLabels) {
                if (k.row == row && k.col == col) {
                    pc = k.pc_hint;
                    switch (ime.mode()) {
                        case mie::InputMode::SmartEn:
                        case mie::InputMode::DirectUpper:
                            lb = k.label_en ? k.label_en : k.label_zh;
                            break;
                        case mie::InputMode::DirectLower:
                            if (k.label_lower)      lb = k.label_lower;
                            else if (k.label_en)    lb = k.label_en;
                            else                    lb = k.label_zh;
                            break;
                        default:
                            lb = k.label_zh;
                            break;
                    }
                    break;
                }
            }
            printf("[%3s:%-5s]", pc, lb);
        }
        puts(" \xe2\x94\x82");
    }
    puts("\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xa4");

    // ── Mode & input: show [matched_prefix]remaining split ─────────────────
    // If there's a matched prefix, bracket it so the user can see which part
    // of the key buffer is "resolved" vs which keys still follow.
    char input_display[320] = {};
    if (ime.input_bytes() == 0) {
        // (按下鍵入)
        snprintf(input_display, sizeof(input_display), "(\xe6\x8c\x89\xe4\xb8\x8b\xe9\x8d\xb5\xe5\x85\xa5)");
    } else {
        int split = ime.matched_prefix_display_bytes();
        const char* s = ime.input_str();
        int total = ime.input_bytes();
        if (split > 0 && split < total) {
            // [matched_prefix_phonemes]remaining_phonemes
            snprintf(input_display, sizeof(input_display), "[%.*s]%s",
                     split, s, s + split);
        } else if (split > 0) {
            // All keys matched: [full_phonemes]
            snprintf(input_display, sizeof(input_display), "[%s]", s);
        } else {
            // No match at all
            snprintf(input_display, sizeof(input_display), "%s", s);
        }
    }
    printf("\xe2\x94\x82 \xe6\xa8\xa1\xe5\xbc\x8f: %s  \xe8\xbc\xb8\xe5\x85\xa5: %-40s \xe2\x94\x82\n",
           ime.mode_indicator(), input_display);

    // ── Candidates — merged list with merged_sel_ highlight ──────────────
    // candidate_index() returns merged_sel_.
    // LEFT/RIGHT/UP/DOWN all navigate within the merged list.
    int ci = ime.candidate_index();  // merged_sel_

    if (ime.mode() == mie::InputMode::SmartZh || ime.mode() == mie::InputMode::SmartEn) {
        int mc = ime.merged_candidate_count();

        if (mc > 0) {
            char cand_buf[512] = {}; int pos = 0;
            int show = (mc < kNumCircles) ? mc : kNumCircles;
            LOG("render: mc=%d show=%d ci=%d\n", mc, show, ci);
            for (int i = 0; i < show && pos < 460; ++i) {
                bool sel = (i == ci);
                int n = snprintf(cand_buf + pos, (int)sizeof(cand_buf) - pos,
                                 "%s%s%s ",
                                 sel ? ">" : " ", kCircle[i],
                                 ime.merged_candidate(i).word);
                if (n > 0) pos += n;
            }
            // Label: 中文 for SmartZh, English for SmartEn
            const char* lbl = (ime.mode() == mie::InputMode::SmartZh)
                              ? "[\xe4\xb8\xad\xe6\x96\x87]"  // [中文]
                              : "[English]";
            printf("\xe2\x94\x82 %s %-46s \xe2\x94\x82\n", lbl, cand_buf);
        } else if (ime.input_bytes() > 0) {
            printf("\xe2\x94\x82   (\xe7\x84\xa1\xe5\x80\x99\xe9\x81\xb8\xe5\xad\x97) %-46s \xe2\x94\x82\n", "");  // (無候選字)
        } else {
            printf("\xe2\x94\x82   %-56s \xe2\x94\x82\n", "");
        }
    } else {
        // Direct Mode: show pending label.
        printf("\xe2\x94\x82 \xe7\x9b\xb4\xe6\x8e\xa5\xe8\xbc\xb8\xe5\x85\xa5: %-48s \xe2\x94\x82\n",  // 直接輸入:
               ime.input_bytes() > 0 ? ime.input_str() : "");
    }

    // ── Committed output (with cursor position shown as |) ───────────────
    {
        std::string disp;
        disp.reserve(g_committed.size() + 3);
        disp.append(g_committed, 0, (size_t)g_cursor);
        disp += '|';
        disp.append(g_committed, (size_t)g_cursor);
        printf("\xe2\x94\x82 \xe5\xb7\xb2\xe7\xa2\xba\xe8\xaa\x8d: %-50s \xe2\x94\x82\n",   // 已確認:
               disp.c_str());
    }

    puts("\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x98");
    // ESC離開 | `=MODE(中→EN→ABC→abc→ㄅ) | ←→=候選/游標 | BS/Del=刪除 | Spc=快選 | ↩=確認
    // Note: string split at \x92 boundaries to avoid hex-escape-out-of-range warnings.
    puts("  ESC \xe9\x9b\xa2\xe9\x96\x8b  |  `=MODE("
         "\xe4\xb8\xad\xe2\x86\x92" "EN"
         "\xe2\x86\x92" "ABC"
         "\xe2\x86\x92" "abc"
         "\xe2\x86\x92\xe3\x84\x85)  |  \xe2\x86\x90\xe2\x86\x92="
         "\xe5\x80\x99\xe9\x81\xb8/\xe6\xb8\xb8\xe6\xa8\x99  |  "
         "BS/Del=\xe5\x88\xaa\xe9\x99\xa4  |  "
         "Spc=\xe5\xbf\xab\xe9\x81\xb8  |  \xe2\x8f\x8e=\xe7\xa2\xba\xe8\xaa\x8d");
    fflush(stdout);
}

// ── Argument parsing ─────────────────────────────────────────────────────

static const char* find_arg(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i)
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    return nullptr;
}

// ── Main ─────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    log_open();
    LOG("main: startup\n");

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    LOG("main: SetConsoleOutputCP(CP_UTF8) done\n");
#endif

    const char* zh_dat = find_arg(argc, argv, "--dat");
    const char* zh_val = find_arg(argc, argv, "--val");
    const char* en_dat = find_arg(argc, argv, "--en-dat");
    const char* en_val = find_arg(argc, argv, "--en-val");

    mie::TrieSearcher zh_searcher;
    if (zh_dat && zh_val) {
        if (zh_searcher.load_from_file(zh_dat, zh_val))
            printf("\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe5\x85\xb8: %u keys\n", zh_searcher.key_count());
        else
            fprintf(stderr, "WARNING: failed to load Chinese dict '%s'/'%s'\n", zh_dat, zh_val);
    }

    mie::TrieSearcher en_searcher;
    bool en_loaded = false;
    if (en_dat && en_val) {
        if (en_searcher.load_from_file(en_dat, en_val)) {
            printf("English dict: %u keys\n", en_searcher.key_count());
            en_loaded = true;
        } else {
            fprintf(stderr, "WARNING: failed to load English dict '%s'/'%s'\n", en_dat, en_val);
        }
    }

    mie::ImeLogic ime(zh_searcher, en_loaded ? &en_searcher : nullptr);
    ime.set_commit_callback(on_commit, nullptr);
    LOG("main: ImeLogic created\n");

    mie::pc::HalPcStdin hal;
    mie::KeyEvent ev;
    LOG("main: HalPcStdin created, entering render\n");

    render(ime);
    LOG("main: initial render done, entering event loop\n");

    while (true) {
        if (!hal.poll(ev)) {
#ifdef _WIN32
            Sleep(10);
#else
            struct timeval tv = { 0, 10000 };
            select(0, nullptr, nullptr, nullptr, &tv);
#endif
            continue;
        }

        // ESC to quit (HalPcStdin maps ESC to row=0xFF signal)
        if (ev.row == 0xFF) break;

        LOG("key: row=%u col=%u\n", (unsigned)ev.row, (unsigned)ev.col);

        // When no IME input is pending, intercept navigation/edit keys for the
        // committed text cursor instead of passing them to ImeLogic.
        bool cursor_handled = false;
        if (ime.input_bytes() == 0) {
            if (ev.row == 5 && ev.col == 2) {  // LEFT → cursor left
                cursor_move_left();
                cursor_handled = true;
            } else if (ev.row == 5 && ev.col == 3) {  // RIGHT → cursor right
                cursor_move_right();
                cursor_handled = true;
            } else if (ev.row == 2 && ev.col == 5) {  // BACK → delete before cursor
                cursor_backspace();
                cursor_handled = true;
            } else if (ev.row == 3 && ev.col == 5) {  // DEL → delete at cursor
                cursor_delete();
                cursor_handled = true;
            }
        }

        bool refresh;
        if (cursor_handled) {
            refresh = true;  // always re-render on cursor edit
        } else {
            refresh = ime.process_key(ev);
        }

        LOG("key: process_key -> refresh=%d zh=%d en=%d merged=%d\n",
            refresh, ime.zh_candidate_count(), ime.en_candidate_count(),
            ime.merged_candidate_count());
        if (refresh) render(ime);
    }

    LOG("main: exit\n");
    log_close();
    return 0;
}
