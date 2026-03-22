// mie_repl.cpp — MokyaInput Engine interactive REPL (PC host only)
// SPDX-License-Identifier: MIT
//
// Renders a virtual MokyaLora half-keyboard in the terminal and feeds
// key events into the MIE engine (stubbed until IME-Logic is implemented).
//
// Build: part of the mie CMake host build (not cross-compiled for RP2350).
// Run:   ./build/mie-host/mie_repl
//
// Controls
//   Mapped keys  — as shown in the virtual keyboard display
//   Ctrl+C / ESC — quit

#include "../hal/pc/hal_pc_stdin.h"
#include "../hal/pc/key_map.h"
#include <cstdio>
#include <cstring>

// ── Virtual keyboard label table ─────────────────────────────────────────

struct KeyLabel {
    uint8_t     row;
    uint8_t     col;
    const char* pc_hint;   ///< PC key shown to the left
    const char* label;     ///< Content label (Bopomofo or function name)
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
    {4, 3, ",",   "，SYM"}, {4, 4, ".",   "。？" }, {4, 5, "=",   "VOL+" },
    // Row 5
    {5, 0, "↑",   "UP"   }, {5, 1, "↓",   "DOWN" }, {5, 2, "←",   "LEFT" },
    {5, 3, "→",   "RIGHT"}, {5, 4, "↵",   "OK"   }, {5, 5, "-",   "VOL-" },
};
// clang-format on

// ── Input buffer ──────────────────────────────────────────────────────────

static char s_input_buf[128] = {};
static int  s_input_len      = 0;

static void input_append(const char* label) {
    int space = (int)sizeof(s_input_buf) - s_input_len - 1;
    if (space <= 0) return;
    int n = snprintf(s_input_buf + s_input_len, (size_t)space, "%s ", label);
    if (n > 0) s_input_len += n;
}

static void input_backspace() {
    // Remove last space-delimited token
    if (s_input_len == 0) return;
    // Step back past trailing space
    if (s_input_buf[s_input_len - 1] == ' ') s_input_len--;
    while (s_input_len > 0 && s_input_buf[s_input_len - 1] != ' ') s_input_len--;
    s_input_buf[s_input_len] = '\0';
}

static void input_clear() {
    s_input_len    = 0;
    s_input_buf[0] = '\0';
}

// ── Rendering ────────────────────────────────────────────────────────────

static void clear_screen() {
    fputs("\033[2J\033[H", stdout);
}

static void render() {
    clear_screen();
    puts("┌─ MokyaLora Virtual Keyboard (mie_repl) ──────────────────────┐");

    for (int row = 0; row < 6; ++row) {
        fputs("│ ", stdout);
        for (int col = 0; col < 6; ++col) {
            // Find label for this cell
            const char* pc    = "?";
            const char* label = "?";
            for (const auto& k : kLabels) {
                if (k.row == row && k.col == col) {
                    pc    = k.pc_hint;
                    label = k.label;
                    break;
                }
            }
            printf("[%2s:%-4s]", pc, label);
        }
        puts(" │");
    }

    puts("├────────────────────────────────────────────────────────────────┤");
    printf("│ Input:      %-50s │\n",
           s_input_len ? s_input_buf : "(press a mapped key)");
    // Candidates placeholder — will be replaced by Trie-Searcher output
    puts("│ Candidates: (IME not yet connected)                            │");
    puts("└────────────────────────────────────────────────────────────────┘");
    puts("  ESC or Ctrl+C to quit  |  BACK = backspace token  |  MODE = clear");
    fflush(stdout);
}

// ── Main loop ────────────────────────────────────────────────────────────

int main() {
    mie::pc::HalPcStdin hal;
    mie::KeyEvent ev;

    render();

    while (true) {
        if (!hal.poll(ev)) {
            // No event — sleep a bit to avoid busy-looping
#ifdef _WIN32
            Sleep(10);
#else
            struct timeval tv = {0, 10000};
            select(0, nullptr, nullptr, nullptr, &tv);
#endif
            continue;
        }

        // ESC (row/col not mapped — check raw key separately)
        // ESC arrives as pc_key 0x1B which is not in the map → ev not set,
        // so handle via a dedicated escape check in the HAL.
        // For now treat OK (row 5, col 4) as submit and MODE (row 4, col 0) as clear.

        if (ev.row == 4 && ev.col == 0) {
            // MODE — clear input
            input_clear();
        } else if (ev.row == 2 && ev.col == 5) {
            // BACK — remove last token
            input_backspace();
        } else if (ev.row == 3 && ev.col == 5) {
            // DEL — same as BACK for now
            input_backspace();
        } else if (ev.row == 5 && ev.col == 4) {
            // OK / Enter — submit (no-op until IME connected)
            input_clear();
        } else {
            // Find the label for this cell and append to input buffer
            for (const auto& k : kLabels) {
                if (k.row == ev.row && k.col == ev.col) {
                    input_append(k.label);
                    break;
                }
            }
        }

        render();
    }

    return 0;
}
