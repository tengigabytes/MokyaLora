// mie_gui.cpp — MokyaInput Engine graphical test tool (PC host only)
// SPDX-License-Identifier: MIT
//
// Two-panel GUI:
//   Left  — 6×6 virtual keyboard matching MokyaLora's physical matrix layout.
//            Each input key (rows 0–3, cols 0–4) shows all three label layers:
//              Line 1: English letters  (e.g. "Q  W")
//              Line 2: Bopomofo         (e.g. "ㄆ  ㄊ")
//              Line 3: Calculator layer (e.g. "(")
//            Function / navigation keys show their label only.
//            PC keyboard shortcut is shown in a tooltip on hover.
//   Right — IME status: mode, phoneme input, candidates, committed output.
//
// Reference: docs/requirements/hardware-requirements.md §8.1
//
// Build: cmake -DMIE_BUILD_GUI=ON -S firmware/mie -B build/mie-gui -G Ninja
//        cmake --build build/mie-gui --target mie_gui
// Run:   mie_gui [--dat dict_dat.bin] [--val dict_values.bin]

#define SDL_MAIN_HANDLED

#ifdef _WIN32
#  include <windows.h>
#  include <GL/gl.h>
#endif

#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#include <mie/ime_logic.h>
#include <mie/trie_searcher.h>
#include "pc/key_map.h"

#include <cstdio>
#include <cstring>
#include <string>

// ── Circled numbers (UTF-8) ───────────────────────────────────────────────

static const char* const kCircleNums[] = {
    "\xe2\x91\xa0","\xe2\x91\xa1","\xe2\x91\xa2","\xe2\x91\xa3","\xe2\x91\xa4",
    "\xe2\x91\xa5","\xe2\x91\xa6","\xe2\x91\xa7","\xe2\x91\xa8","\xe2\x91\xa9",
};

// ── CJK glyph ranges (must outlive font building) ─────────────────────────
// Covers everything printed on the physical keyboard plus UI text.

static const ImWchar kGlyphRanges[] = {
    0x0020, 0x00FF,  // Basic Latin + Latin-1 (×÷ etc.)
    0x02B0, 0x02FF,  // Spacing Modifier Letters  (ˣ ˇ ˋ ˊ ˙)
    0x2010, 0x2027,  // General Punctuation       (— em-dash)
    0x2190, 0x21FF,  // Arrows                    (↵)
    0x2460, 0x2473,  // Enclosed Numbers          (①–⑳)
    0x25A0, 0x25FF,  // Geometric Shapes          (▲▼◄►)
    0x3000, 0x303F,  // CJK Symbols & Punctuation (。)
    0x3100, 0x312F,  // Bopomofo
    0x31A0, 0x31BF,  // Bopomofo Extended
    0x4E00, 0x9FFF,  // CJK Unified Ideographs
    0xFF00, 0xFFEF,  // Halfwidth/Fullwidth Forms  (，？)
    0,
};

// ── GUI key descriptor ────────────────────────────────────────────────────
// Each physical key carries up to three label layers.
// For input keys (rows 0–3, cols 0–4):
//   letters = English characters printed on the key  ("Q  W", "1  2")
//   bpmf    = Bopomofo phonemes                       ("ㄆ  ㄊ", "ㄅ  ㄉ")
//   calc    = Calculator / numeric layer               ("(", "ANS", "7")
// For function / navigation keys: only letters is set; bpmf and calc are "".

struct GuiKey {
    uint8_t     row, col;
    const char* letters;   // top label line
    const char* bpmf;      // middle label line (Bopomofo)
    const char* calc;      // bottom label line (calculator layer)
    const char* pc_hint;   // PC keyboard shortcut shown in tooltip
    SDL_Keycode sdl_key;
};

// clang-format off
static const GuiKey kGuiKeys[] = {
    // ── Rows 0–3 : core input area (5×4 keys + col-5 function) ──────────
    //  row,col   letters      bpmf            calc      pc    sdl
    // Row 0
    { 0, 0,  "1  2",      "ㄅ  ㄉ",      "ANS",   "1",   SDLK_1        },
    { 0, 1,  "3  4",      "ˇ    ˋ",      "7",     "3",   SDLK_3        },
    { 0, 2,  "5  6",      "ㄓ  ˊ",       "8",     "5",   SDLK_5        },
    { 0, 3,  "7  8",      "˙  ㄚ",       "9",     "7",   SDLK_7        },
    { 0, 4,  "9  0",      "ㄞ ㄢ ㄦ",    "\xc3\xb7",    "9",   SDLK_9  },  // ÷
    { 0, 5,  "FUNC",      "",             "",      "F1",  SDLK_F1       },
    // Row 1
    { 1, 0,  "Q  W",      "ㄆ  ㄊ",      "(",     "q",   SDLK_q        },
    { 1, 1,  "E  R",      "ㄍ  ㄐ",      "4",     "e",   SDLK_e        },
    { 1, 2,  "T  Y",      "ㄔ  ㄗ",      "5",     "t",   SDLK_t        },
    { 1, 3,  "U  I",      "ㄧ  ㄛ",      "6",     "u",   SDLK_u        },
    { 1, 4,  "O  P",      "ㄟ  ㄣ",      "\xc3\x97",    "o",   SDLK_o  },  // ×
    { 1, 5,  "SET",       "",             "",      "F2",  SDLK_F2       },
    // Row 2
    { 2, 0,  "A  S",      "ㄇ  ㄋ",      ")",     "a",   SDLK_a        },
    { 2, 1,  "D  F",      "ㄎ  ㄑ",      "1",     "d",   SDLK_d        },
    { 2, 2,  "G  H",      "ㄕ  ㄘ",      "2",     "g",   SDLK_g        },
    { 2, 3,  "J  K",      "ㄨ  ㄜ",      "3",     "j",   SDLK_j        },
    { 2, 4,  "L",         "ㄠ  ㄤ",      "-",     "l",   SDLK_l        },
    { 2, 5,  "BACK",      "",             "",      "BS",  SDLK_BACKSPACE},
    // Row 3
    { 3, 0,  "Z  X",      "ㄈ  ㄌ",      "AC",    "z",   SDLK_z        },
    { 3, 1,  "C  V",      "ㄏ  ㄒ",      "0",     "c",   SDLK_c        },
    { 3, 2,  "B  N",      "ㄖ  ㄙ",      ".",     "b",   SDLK_b        },
    { 3, 3,  "M",         "ㄩ  ㄝ",      "x10\xcb\xa3", "m", SDLK_m   },  // x10ˣ
    { 3, 4,  "\xe2\x80\x94", "ㄡ  ㄥ",  "+",     "\\",  SDLK_BACKSLASH},  // — em-dash
    { 3, 5,  "DEL",       "",             "",      "Del", SDLK_DELETE   },

    // ── Row 4 : function bar ──────────────────────────────────────────────
    { 4, 0,  "MODE",      "",  "",  "`",   SDLK_BACKQUOTE },
    { 4, 1,  "TAB",       "",  "",  "Tab", SDLK_TAB       },
    { 4, 2,  "SPACE",     "",  "",  "Spc", SDLK_SPACE     },
    { 4, 3,  "\xef\xbc\x8c SYM", "", "", ",", SDLK_COMMA  },  // ，SYM
    { 4, 4,  "\xe3\x80\x82 . \xef\xbc\x9f", "", "", ".", SDLK_PERIOD }, // 。.？
    { 4, 5,  "VOL+",      "",  "",  "=",   SDLK_EQUALS    },

    // ── Row 5 : navigation / D-pad ────────────────────────────────────────
    { 5, 0,  "\xe2\x96\xb2 UP",    "", "", "\xe2\x86\x91", SDLK_UP    }, // ▲
    { 5, 1,  "\xe2\x96\xbc DN",    "", "", "\xe2\x86\x93", SDLK_DOWN  }, // ▼
    { 5, 2,  "\xe2\x97\x84 LEFT",  "", "", "\xe2\x86\x90", SDLK_LEFT  }, // ◄
    { 5, 3,  "RIGHT \xe2\x96\xba", "", "", "\xe2\x86\x92", SDLK_RIGHT }, // ►
    { 5, 4,  "\xe2\x86\xb5 OK",    "", "", "Ent",           SDLK_RETURN}, // ↵
    { 5, 5,  "VOL-",               "", "", "-",             SDLK_MINUS },
};
// clang-format on

static const int kNumGuiKeys = (int)(sizeof(kGuiKeys) / sizeof(kGuiKeys[0]));

// ── SDL_Keycode → pc_key mapping ──────────────────────────────────────────

static int sdl_to_pc_key(SDL_Keycode sym) {
    if (sym >= SDLK_a && sym <= SDLK_z) return (int)sym;
    if (sym >= SDLK_0 && sym <= SDLK_9) return (int)sym;
    switch (sym) {
        case SDLK_BACKSPACE:  return mie::pc::KEY_BACKSPACE;
        case SDLK_TAB:        return mie::pc::KEY_TAB;
        case SDLK_RETURN:     return mie::pc::KEY_ENTER;
        case SDLK_ESCAPE:     return mie::pc::KEY_ESCAPE;
        case SDLK_DELETE:     return mie::pc::KEY_DELETE;
        case SDLK_F1:         return mie::pc::KEY_F1;
        case SDLK_F2:         return mie::pc::KEY_F2;
        case SDLK_UP:         return mie::pc::KEY_UP;
        case SDLK_DOWN:       return mie::pc::KEY_DOWN;
        case SDLK_LEFT:       return mie::pc::KEY_LEFT;
        case SDLK_RIGHT:      return mie::pc::KEY_RIGHT;
        case SDLK_BACKQUOTE:  return '`';
        case SDLK_SPACE:      return ' ';
        case SDLK_COMMA:      return ',';
        case SDLK_PERIOD:     return '.';
        case SDLK_EQUALS:     return '=';
        case SDLK_MINUS:      return '-';
        case SDLK_BACKSLASH:  return '\\';
        default:              return -1;
    }
}

static bool pc_key_to_event(int pc_key, mie::KeyEvent& ev_out) {
    for (const auto& e : mie::pc::kPcKeyMap) {
        if (e.pc_key == -1) break;
        if (e.pc_key == pc_key) {
            ev_out.row = e.row; ev_out.col = e.col; ev_out.pressed = true;
            return true;
        }
    }
    return false;
}

// ── Misc helpers ──────────────────────────────────────────────────────────

static const char* find_arg(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i)
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    return nullptr;
}

static const char* mode_name(mie::InputMode m) {
    switch (m) {
        case mie::InputMode::Bopomofo:     return "\xe6\xb3\xa8\xe9\x9f\xb3";  // 注音
        case mie::InputMode::English:      return "English";
        case mie::InputMode::Alphanumeric: return "ABC";
    }
    return "?";
}

static ImVec4 mode_color(mie::InputMode m) {
    switch (m) {
        case mie::InputMode::Bopomofo:     return ImVec4(0.40f, 0.85f, 1.00f, 1.0f);
        case mie::InputMode::English:      return ImVec4(0.45f, 1.00f, 0.45f, 1.0f);
        case mie::InputMode::Alphanumeric: return ImVec4(1.00f, 0.80f, 0.20f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

// ── Key flash state ───────────────────────────────────────────────────────

struct FlashState { uint8_t row = 0xFF; uint8_t col = 0xFF; Uint32 until_ms = 0; };

static bool is_flashing(const FlashState& fs, uint8_t r, uint8_t c) {
    return fs.row == r && fs.col == c && SDL_GetTicks() < fs.until_ms;
}

// ── Build ImGui button label from GuiKey ──────────────────────────────────

static void build_btn_label(char* buf, int sz, const GuiKey* gk) {
    if (gk->bpmf[0] && gk->calc[0])
        snprintf(buf, sz, "%s\n%s\n%s##%d_%d",
                 gk->letters, gk->bpmf, gk->calc, gk->row, gk->col);
    else if (gk->bpmf[0])
        snprintf(buf, sz, "%s\n%s##%d_%d",
                 gk->letters, gk->bpmf, gk->row, gk->col);
    else
        snprintf(buf, sz, "%s##%d_%d", gk->letters, gk->row, gk->col);
}

// ── Main ──────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    const char* dat_path = find_arg(argc, argv, "--dat");
    const char* val_path = find_arg(argc, argv, "--val");

    mie::TrieSearcher searcher;
    bool dict_loaded = false;
    if (dat_path && val_path) {
        dict_loaded = searcher.load_from_file(dat_path, val_path);
        if (!dict_loaded)
            fprintf(stderr, "WARNING: failed to load dictionary '%s' / '%s'\n",
                    dat_path, val_path);
    }

    mie::ImeLogic ime(searcher);

    // ── SDL + OpenGL ───────────────────────────────────────────────────────

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    SDL_Window* window = SDL_CreateWindow(
        "MIE GUI — MokyaInput Engine Test Tool",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        980, 530,
        SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    if (!window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);

    // ── Dear ImGui ────────────────────────────────────────────────────────

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;  // keyboard → IME only
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 130");

    // ── CJK font ───────────────────────────────────────────────────────────

    const char* cjk_fonts[] = {
        "C:/Windows/Fonts/msjh.ttc",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simsun.ttc",
        nullptr
    };
    bool cjk_loaded = false;
    ImFontConfig fc; fc.OversampleH = 1; fc.OversampleV = 1; fc.PixelSnapH = true;
    for (int fi = 0; cjk_fonts[fi] && !cjk_loaded; ++fi) {
        if (FILE* f = fopen(cjk_fonts[fi], "rb")) {
            fclose(f);
            io.Fonts->AddFontFromFileTTF(cjk_fonts[fi], 15.0f, &fc, kGlyphRanges);
            cjk_loaded = true;
        }
    }
    if (!cjk_loaded) {
        io.Fonts->AddFontDefault();
        fprintf(stderr, "WARNING: CJK font not found; Bopomofo may not render.\n");
    }

    // ── Application state ─────────────────────────────────────────────────

    FlashState  flash;
    std::string committed;
    bool        scroll_bottom = false;

    auto fire_key = [&](uint8_t row, uint8_t col) {
        mie::KeyEvent ev; ev.row = row; ev.col = col; ev.pressed = true;
        bool is_commit = (row == 4 && col == 2) || (row == 5 && col == 4);
        if (is_commit && ime.candidate_count() > 0) {
            committed += ime.candidate(0).word;
            scroll_bottom = true;
        }
        ime.process_key(ev);
        flash.row = row; flash.col = col; flash.until_ms = SDL_GetTicks() + 150;
    };

    // ── Main loop ─────────────────────────────────────────────────────────

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN) {
                int pc_key = sdl_to_pc_key(event.key.keysym.sym);
                if (pc_key == mie::pc::KEY_ESCAPE) {
                    running = false;
                } else if (pc_key != -1) {
                    mie::KeyEvent ev;
                    if (pc_key_to_event(pc_key, ev)) fire_key(ev.row, ev.col);
                }
            }
        }

        // ── ImGui frame ───────────────────────────────────────────────────

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        int draw_w, draw_h;
        SDL_GetWindowSize(window, &draw_w, &draw_h);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)draw_w, (float)draw_h));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
        ImGui::Begin("##main", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ── Left panel: virtual keyboard ──────────────────────────────────

        // Button dimensions: input keys are 3-line tall; function keys are 1-line.
        // Use uniform size so the grid aligns cleanly.
        const float btn_w   = 74.0f;
        const float btn_h   = 72.0f;
        const float btn_gap = 3.0f;
        const float kbd_w   = 6.0f * btn_w + 5.0f * btn_gap + 1.0f;  // ≈447

        // Colour scheme for different key zones
        const ImVec4 col_input_bg  (0.22f, 0.22f, 0.28f, 1.0f);  // input keys
        const ImVec4 col_fn_bg     (0.18f, 0.28f, 0.18f, 1.0f);  // col-5 function
        const ImVec4 col_bar_bg    (0.28f, 0.22f, 0.22f, 1.0f);  // row-4 function bar
        const ImVec4 col_nav_bg    (0.22f, 0.22f, 0.35f, 1.0f);  // row-5 navigation
        const ImVec4 col_flash_bg  (0.15f, 0.65f, 0.15f, 1.0f);  // active flash

        ImGui::BeginChild("##kb", ImVec2(kbd_w, 0.0f), false, ImGuiWindowFlags_NoScrollbar);

        ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1.0f),
            "Virtual Keyboard  (click or use PC key | hover for shortcut)");
        ImGui::Spacing();

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(btn_gap, btn_gap));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f, 4.0f));

        for (int row = 0; row < 6; ++row) {
            for (int col = 0; col < 6; ++col) {
                if (col > 0) ImGui::SameLine(0.0f, btn_gap);

                const GuiKey* gk = nullptr;
                for (int ki = 0; ki < kNumGuiKeys; ++ki)
                    if (kGuiKeys[ki].row == row && kGuiKeys[ki].col == col) { gk = &kGuiKeys[ki]; break; }

                if (!gk) { ImGui::Dummy(ImVec2(btn_w, btn_h)); continue; }

                // Pick background colour
                ImVec4 bg = col_input_bg;
                if (col == 5 && row <= 3)      bg = col_fn_bg;
                else if (row == 4)             bg = col_bar_bg;
                else if (row == 5)             bg = col_nav_bg;

                bool flashing = is_flashing(flash, (uint8_t)row, (uint8_t)col);
                if (flashing) bg = col_flash_bg;

                ImGui::PushStyleColor(ImGuiCol_Button,        bg);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImVec4(bg.x+0.10f, bg.y+0.10f, bg.z+0.10f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                    ImVec4(bg.x-0.05f, bg.y-0.05f, bg.z-0.05f, 1.0f));

                char btn_label[192];
                build_btn_label(btn_label, sizeof(btn_label), gk);

                bool clicked = ImGui::Button(btn_label, ImVec2(btn_w, btn_h));
                ImGui::PopStyleColor(3);

                // Tooltip: PC keyboard shortcut
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("PC key: %s", gk->pc_hint);

                if (clicked) fire_key((uint8_t)row, (uint8_t)col);
            }
        }

        ImGui::PopStyleVar(2);  // ItemSpacing + FramePadding
        ImGui::EndChild();      // ##kb

        // ── Right panel: IME status ───────────────────────────────────────

        ImGui::SameLine(0.0f, 14.0f);
        ImGui::BeginChild("##ime", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoScrollbar);

        ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1.0f), "IME Status");
        ImGui::Separator();
        ImGui::Spacing();

        // Mode
        ImGui::Text("Mode:");
        ImGui::SameLine();
        ImGui::TextColored(mode_color(ime.mode()), "%s", mode_name(ime.mode()));
        ImGui::SameLine();
        ImGui::TextDisabled("  [` to cycle]");
        ImGui::Spacing();

        // Input phoneme sequence
        ImGui::Text("Input:");
        ImGui::SameLine();
        if (ime.input_bytes() > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.95f, 0.20f, 1.0f), "%s", ime.input_str());
        else
            ImGui::TextDisabled("(empty)");
        ImGui::Spacing();

        // Candidates
        ImGui::Text("Candidates:");
        ImGui::Spacing();
        int cand_n = ime.candidate_count();
        if (cand_n == 0) {
            ImGui::TextDisabled(ime.input_bytes() > 0 ? "no match" : "(none)");
        }
        for (int i = 0; i < cand_n; ++i) {
            char cl[128];
            snprintf(cl, sizeof(cl), "%s %s##c%d", kCircleNums[i], ime.candidate(i).word, i);
            if (ImGui::Button(cl)) {
                committed += ime.candidate(i).word;
                scroll_bottom = true;
                ime.clear_input();
            }
            if (i < cand_n - 1) ImGui::SameLine();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Committed text
        ImGui::Text("Committed:");
        ImGui::BeginChild("##committed", ImVec2(0.0f, 110.0f), true);
        if (committed.empty())
            ImGui::TextDisabled("(nothing committed yet)");
        else
            ImGui::TextWrapped("%s", committed.c_str());
        if (scroll_bottom) { ImGui::SetScrollHereY(1.0f); scroll_bottom = false; }
        ImGui::EndChild();

        ImGui::Spacing();
        if (ImGui::Button("Clear committed")) committed.clear();
        ImGui::SameLine();
        if (ImGui::Button("Clear input"))     ime.clear_input();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Dictionary status
        ImGui::Text("Dictionary:");
        ImGui::SameLine();
        if (dict_loaded && searcher.is_loaded())
            ImGui::TextColored(ImVec4(0.2f,0.9f,0.2f,1.0f),
                "loaded  (%u keys)", searcher.key_count());
        else
            ImGui::TextColored(ImVec4(0.9f,0.2f,0.2f,1.0f),
                "not loaded  (--dat / --val)");

        ImGui::Spacing();
        ImGui::TextDisabled("ESC=quit  SPACE/Enter=commit  `=cycle mode");

        ImGui::EndChild();  // ##ime

        ImGui::End();
        ImGui::PopStyleVar();  // WindowPadding

        // ── Render ────────────────────────────────────────────────────────

        ImGui::Render();
        int vp_w, vp_h;
        SDL_GL_GetDrawableSize(window, &vp_w, &vp_h);
        glViewport(0, 0, vp_w, vp_h);
        glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
