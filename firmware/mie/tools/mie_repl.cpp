// mie_repl.cpp — MokyaInput Engine interactive REPL (PC host only)
// SPDX-License-Identifier: MIT
//
// Renders a virtual MokyaLora half-keyboard in the terminal and passes key
// events through ImeLogic → TrieSearcher, displaying the current phoneme
// input sequence and candidate words in real time.
//
// Build: part of the mie CMake host build (not cross-compiled for RP2350).
// Run:   ./build/mie-host/mie_repl [--dat dict_dat.bin] [--val dict_values.bin]
//
// Without --dat / --val the REPL still runs but shows no candidates.
//
// Controls (mapped keys shown in the virtual keyboard display):
//   Phoneme keys  — append Bopomofo phoneme to input sequence
//   BACK / DEL    — remove last phoneme
//   MODE (`)      — cycle input mode, clear input
//   SPACE / OK    — commit first candidate (clears input)
//   ESC / Ctrl-C  — quit

#include "../hal/pc/hal_pc_stdin.h"
#include "../hal/pc/key_map.h"
#include <mie/ime_logic.h>
#include <mie/trie_searcher.h>

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/select.h>
#endif

// ── Virtual keyboard label table ─────────────────────────────────────────

struct KeyLabel {
    uint8_t     row;
    uint8_t     col;
    const char* pc_hint;
    const char* label;
};

// clang-format off
static const KeyLabel kLabels[] = {
    // Row 0
    {0, 0, "1",   "ㄅㄉ"  }, {0, 1, "3",   "ˇˋ"   }, {0, 2, "5",   "ㄓˊ"  },
    {0, 3, "7",   "˙ㄚ"  }, {0, 4, "9",   "ㄞㄢ" }, {0, 5, "F1",  "FUNC" },
    // Row 1
    {1, 0, "q",   "ㄆㄊ"  }, {1, 1, "e",   "ㄍㄐ" }, {1, 2, "t",   "ㄔㄗ" },
    {1, 3, "u",   "ㄧㄛ" }, {1, 4, "o",   "ㄟㄣ" }, {1, 5, "F2",  "SET"  },
    // Row 2
    {2, 0, "a",   "ㄇㄋ"  }, {2, 1, "d",   "ㄎㄑ" }, {2, 2, "g",   "ㄕㄘ" },
    {2, 3, "j",   "ㄨㄜ" }, {2, 4, "l",   "ㄠㄤ" }, {2, 5, "BS",  "BACK" },
    // Row 3
    {3, 0, "z",   "ㄈㄌ"  }, {3, 1, "c",   "ㄏㄒ" }, {3, 2, "b",   "ㄖㄙ" },
    {3, 3, "m",   "ㄩㄝ" }, {3, 4, "\\",  "ㄡㄥ" }, {3, 5, "Del", "DEL"  },
    // Row 4
    {4, 0, "`",   "MODE" }, {4, 1, "Tab", "TAB"  }, {4, 2, "Spc", "SPACE"},
    {4, 3, ",",   "\xef\xbc\x8cSYM"}, {4, 4, ".",   "\xe3\x80\x82\xef\xbc\x9f"}, {4, 5, "=",   "VOL+" },
    // Row 5
    {5, 0, "\xe2\x86\x91",   "UP"   }, {5, 1, "\xe2\x86\x93",   "DOWN" }, {5, 2, "\xe2\x86\x90",   "LEFT" },
    {5, 3, "\xe2\x86\x92",   "RIGHT"}, {5, 4, "\xe2\x8f\x8e",   "OK"   }, {5, 5, "-",   "VOL-" },
};
// clang-format on

static const char* label_for(uint8_t row, uint8_t col) {
    for (const auto& k : kLabels) {
        if (k.row == row && k.col == col) return k.label;
    }
    return "?";
}

static const char* mode_name(mie::InputMode m) {
    switch (m) {
        case mie::InputMode::Bopomofo:     return "注音";
        case mie::InputMode::English:      return "EN";
        case mie::InputMode::Alphanumeric: return "ABC";
    }
    return "?";
}

// ── Rendering ────────────────────────────────────────────────────────────

static void clear_screen() { fputs("\033[2J\033[H", stdout); }

static void render(const mie::ImeLogic& ime) {
    clear_screen();
    puts("\xe2\x94\x8c\xe2\x94\x80 MokyaLora Virtual Keyboard (mie_repl) \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90");

    for (int row = 0; row < 6; ++row) {
        fputs("\xe2\x94\x82 ", stdout);
        for (int col = 0; col < 6; ++col) {
            const char* pc    = "?";
            const char* label = "?";
            for (const auto& k : kLabels) {
                if (k.row == row && k.col == col) { pc = k.pc_hint; label = k.label; break; }
            }
            printf("[%2s:%-4s]", pc, label);
        }
        puts(" \xe2\x94\x82");
    }

    puts("\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xa4");

    // Mode indicator and input line
    printf("\xe2\x94\x82 \xe6\xa8\xa1\xe5\xbc\x8f: %-4s  \xe8\xbc\xb8\xe5\x85\xa5: %-46s \xe2\x94\x82\n",
           mode_name(ime.mode()),
           ime.input_bytes() > 0 ? ime.input_str() : "(\xe6\x8c\x89\xe4\xb8\x8b\xe6\xb3\xa8\xe9\x9f\xb3\xe9\x8d\xb5)");

    // Candidates line
    char cand_buf[256] = {};
    int  pos           = 0;
    if (ime.candidate_count() > 0) {
        for (int i = 0; i < ime.candidate_count() && pos < 200; ++i) {
            const char* circle_nums[] = {
                "\xe2\x91\xa0","\xe2\x91\xa1","\xe2\x91\xa2","\xe2\x91\xa3","\xe2\x91\xa4",
                "\xe2\x91\xa5","\xe2\x91\xa6","\xe2\x91\xa7","\xe2\x91\xa8","\xe2\x91\xa9",
            };
            int n = snprintf(cand_buf + pos, sizeof(cand_buf) - pos - 1,
                             "%s%s ", circle_nums[i], ime.candidate(i).word);
            if (n > 0) pos += n;
        }
    } else if (ime.input_bytes() > 0) {
        snprintf(cand_buf, sizeof(cand_buf), "(\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0\xe5\x80\x99\xe9\x81\xb8\xe5\xad\x97)");
    } else {
        snprintf(cand_buf, sizeof(cand_buf), "(\xe7\x84\xa1\xe5\xad\x97\xe5\x85\xb8\xe6\x99\x82\xe9\xa1\xaf\xe7\xa4\xba\xe9\x8d\xb5\xe5\x90\x8d)");
    }
    printf("\xe2\x94\x82 \xe5\x80\x99\xe9\x81\xb8: %-55s \xe2\x94\x82\n", cand_buf);

    puts("\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x98");
    puts("  ESC/Ctrl-C \xe9\x9b\xa2\xe9\x96\x8b  |  BACK=\xe5\x88\xaa\xe9\x99\xa4  |  MODE=\xe5\x88\x87\xe6\xa8\xa1\xe5\xbc\x8f  |  SPACE/OK=\xe6\xa7\x8b\xe8\xa9\x9e");
    fflush(stdout);
}

// ── Argument parsing (minimal) ────────────────────────────────────────────

static const char* find_arg(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    }
    return nullptr;
}

// ── Main loop ────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
#ifdef _WIN32
    // Enable UTF-8 console output on Windows.
    SetConsoleOutputCP(CP_UTF8);
#endif

    const char* dat_path = find_arg(argc, argv, "--dat");
    const char* val_path = find_arg(argc, argv, "--val");

    mie::TrieSearcher searcher;
    if (dat_path && val_path) {
        if (searcher.load_from_file(dat_path, val_path)) {
            printf("Dictionary loaded: %u keys.\n", searcher.key_count());
        } else {
            fprintf(stderr, "WARNING: failed to load dictionary from '%s' / '%s'.\n",
                    dat_path, val_path);
        }
    }

    mie::ImeLogic     ime(searcher);
    mie::pc::HalPcStdin hal;
    mie::KeyEvent     ev;

    render(ime);

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

        const bool refresh = ime.process_key(ev);
        if (refresh) render(ime);
    }

    return 0;
}
