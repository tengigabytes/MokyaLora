// mie_gui.cpp — MokyaInput Engine graphical test tool (PC host only)
// SPDX-License-Identifier: MIT
//
// Two-panel GUI:
//   Left  — 6x6 virtual keyboard matching MokyaLora's physical layout.
//            Click buttons or use PC keyboard shortcuts to feed key events.
//   Right — IME status: mode, phoneme input, candidates, committed output.
//
// Build: cmake -DMIE_BUILD_GUI=ON -S firmware/mie -B build/mie-gui
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

static const ImWchar kGlyphRanges[] = {
    0x0020, 0x00FF,  // Basic Latin + Latin-1 Supplement
    0x02B0, 0x02FF,  // Spacing Modifier Letters (Bopomofo tones: ˊˇˋ˙)
    0x2460, 0x2473,  // Circled numbers ①–⑳
    0x3100, 0x312F,  // Bopomofo
    0x31A0, 0x31BF,  // Bopomofo Extended
    0x4E00, 0x9FFF,  // CJK Unified Ideographs
    0xFF00, 0xFFEF,  // Halfwidth/Fullwidth Forms
    0,
};

// ── GUI key descriptor ────────────────────────────────────────────────────

struct GuiKey {
    uint8_t      row;
    uint8_t      col;
    const char*  hint;   // PC key hint shown on top line of button
    const char*  label;  // Bopomofo / function label on bottom line
    SDL_Keycode  sdl_key;
};

// clang-format off
static const GuiKey kGuiKeys[] = {
    // Row 0
    {0,0,"1",  "ㄅㄉ",  SDLK_1},         {0,1,"3",  "ˇˋ",    SDLK_3},
    {0,2,"5",  "ㄓˊ",   SDLK_5},         {0,3,"7",  "˙ㄚ",   SDLK_7},
    {0,4,"9",  "ㄞㄢ",  SDLK_9},         {0,5,"F1", "FUNC",   SDLK_F1},
    // Row 1
    {1,0,"q",  "ㄆㄊ",  SDLK_q},         {1,1,"e",  "ㄍㄐ",  SDLK_e},
    {1,2,"t",  "ㄔㄗ",  SDLK_t},         {1,3,"u",  "ㄧㄛ",  SDLK_u},
    {1,4,"o",  "ㄟㄣ",  SDLK_o},         {1,5,"F2", "SET",    SDLK_F2},
    // Row 2
    {2,0,"a",  "ㄇㄋ",  SDLK_a},         {2,1,"d",  "ㄎㄑ",  SDLK_d},
    {2,2,"g",  "ㄕㄘ",  SDLK_g},         {2,3,"j",  "ㄨㄜ",  SDLK_j},
    {2,4,"l",  "ㄠㄤ",  SDLK_l},         {2,5,"BS", "BACK",   SDLK_BACKSPACE},
    // Row 3
    {3,0,"z",  "ㄈㄌ",  SDLK_z},         {3,1,"c",  "ㄏㄒ",  SDLK_c},
    {3,2,"b",  "ㄖㄙ",  SDLK_b},         {3,3,"m",  "ㄩㄝ",  SDLK_m},
    {3,4,"\\", "ㄡㄥ",  SDLK_BACKSLASH}, {3,5,"Del","DEL",    SDLK_DELETE},
    // Row 4
    {4,0,"`",  "MODE",  SDLK_BACKQUOTE}, {4,1,"Tab","TAB",    SDLK_TAB},
    {4,2,"Spc","SPACE", SDLK_SPACE},     {4,3,",",  "SYM",    SDLK_COMMA},
    {4,4,".",  "。？",  SDLK_PERIOD},    {4,5,"=",  "VOL+",   SDLK_EQUALS},
    // Row 5
    {5,0,"Up", "UP",    SDLK_UP},        {5,1,"Dn", "DOWN",   SDLK_DOWN},
    {5,2,"Lt", "LEFT",  SDLK_LEFT},      {5,3,"Rt", "RIGHT",  SDLK_RIGHT},
    {5,4,"Ent","OK",    SDLK_RETURN},    {5,5,"-",  "VOL-",   SDLK_MINUS},
};
// clang-format on

static const int kNumGuiKeys = (int)(sizeof(kGuiKeys) / sizeof(kGuiKeys[0]));

// ── SDL_Keycode → pc_key mapping ──────────────────────────────────────────

static int sdl_to_pc_key(SDL_Keycode sym) {
    if (sym >= SDLK_a && sym <= SDLK_z) return (int)sym;  // SDL lowercase = ASCII
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

// ── pc_key → KeyEvent row/col ─────────────────────────────────────────────

static bool pc_key_to_event(int pc_key, mie::KeyEvent& ev_out) {
    for (const auto& entry : mie::pc::kPcKeyMap) {
        if (entry.pc_key == -1) break;
        if (entry.pc_key == pc_key) {
            ev_out.row     = entry.row;
            ev_out.col     = entry.col;
            ev_out.pressed = true;
            return true;
        }
    }
    return false;
}

// ── Argument parsing ───────────────────────────────────────────────────────

static const char* find_arg(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    }
    return nullptr;
}

// ── Mode name + color ─────────────────────────────────────────────────────

static const char* mode_name(mie::InputMode m) {
    switch (m) {
        case mie::InputMode::Bopomofo:     return "\xe6\xb3\xa8\xe9\x9f\xb3";  // 注音
        case mie::InputMode::English:      return "EN";
        case mie::InputMode::Alphanumeric: return "ABC";
    }
    return "?";
}

static ImVec4 mode_color(mie::InputMode m) {
    switch (m) {
        case mie::InputMode::Bopomofo:     return ImVec4(0.40f, 0.85f, 1.00f, 1.0f);  // cyan
        case mie::InputMode::English:      return ImVec4(0.45f, 1.00f, 0.45f, 1.0f);  // green
        case mie::InputMode::Alphanumeric: return ImVec4(1.00f, 0.80f, 0.20f, 1.0f);  // amber
    }
    return ImVec4(1, 1, 1, 1);
}

// ── Key-flash state ───────────────────────────────────────────────────────

struct FlashState {
    uint8_t  row = 0xFF;
    uint8_t  col = 0xFF;
    Uint32   until_ms = 0;
};

static bool is_flashing(const FlashState& fs, uint8_t row, uint8_t col) {
    return fs.row == row && fs.col == col && SDL_GetTicks() < fs.until_ms;
}

// ── Main ──────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // Parse arguments
    const char* dat_path = find_arg(argc, argv, "--dat");
    const char* val_path = find_arg(argc, argv, "--val");

    // Load dictionary
    mie::TrieSearcher searcher;
    bool dict_loaded = false;
    if (dat_path && val_path) {
        dict_loaded = searcher.load_from_file(dat_path, val_path);
        if (!dict_loaded) {
            fprintf(stderr, "WARNING: failed to load dictionary '%s' / '%s'\n",
                    dat_path, val_path);
        }
    }

    mie::ImeLogic ime(searcher);

    // ── SDL + OpenGL setup ────────────────────────────────────────────────

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    const int win_w = 860;
    const int win_h = 440;
    SDL_Window* window = SDL_CreateWindow(
        "MIE GUI — MokyaInput Engine Test Tool",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);  // vsync

    // ── Dear ImGui setup ──────────────────────────────────────────────────

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // Disable ImGui keyboard navigation — all keyboard input goes to IME
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 130");

    // ── Font loading (CJK) ────────────────────────────────────────────────

    const char* cjk_fonts[] = {
        "C:/Windows/Fonts/msjh.ttc",   // Microsoft JhengHei (Traditional)
        "C:/Windows/Fonts/msyh.ttc",   // Microsoft YaHei
        "C:/Windows/Fonts/simsun.ttc", // SimSun
        nullptr
    };

    ImFontConfig font_cfg;
    font_cfg.OversampleH    = 1;
    font_cfg.OversampleV    = 1;
    font_cfg.PixelSnapH     = true;
    bool cjk_loaded = false;
    for (int fi = 0; cjk_fonts[fi] != nullptr; ++fi) {
        FILE* f = fopen(cjk_fonts[fi], "rb");
        if (!f) continue;
        fclose(f);
        io.Fonts->AddFontFromFileTTF(cjk_fonts[fi], 16.0f, &font_cfg, kGlyphRanges);
        cjk_loaded = true;
        break;
    }
    if (!cjk_loaded) {
        io.Fonts->AddFontDefault();
    }
    io.Fonts->Build();

    // ── Application state ─────────────────────────────────────────────────

    FlashState  flash;
    std::string committed;       // accumulated committed text
    bool        scroll_to_bottom = false;

    // Helper: fire a key event through the IME, handle commit side-effect
    auto fire_key = [&](uint8_t row, uint8_t col) {
        mie::KeyEvent ev;
        ev.row     = row;
        ev.col     = col;
        ev.pressed = true;

        // Capture first candidate BEFORE commit (SPACE=4,2 or OK=5,4)
        bool is_commit = (row == 4 && col == 2) || (row == 5 && col == 4);
        if (is_commit && ime.candidate_count() > 0) {
            committed += ime.candidate(0).word;
            scroll_to_bottom = true;
        }

        ime.process_key(ev);

        // Flash the pressed button
        flash.row      = row;
        flash.col      = col;
        flash.until_ms = SDL_GetTicks() + 150;
    };

    // ── Main loop ─────────────────────────────────────────────────────────

    bool running = true;
    while (running) {
        // Process events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                // Route physical keyboard → IME (bypassing ImGui nav)
                int pc_key = sdl_to_pc_key(event.key.keysym.sym);
                if (pc_key == mie::pc::KEY_ESCAPE) {
                    running = false;
                } else if (pc_key != -1) {
                    mie::KeyEvent ev;
                    if (pc_key_to_event(pc_key, ev)) {
                        fire_key(ev.row, ev.col);
                    }
                }
            }
        }

        // ── ImGui frame ───────────────────────────────────────────────────

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Full-window main panel
        int draw_w, draw_h;
        SDL_GetWindowSize(window, &draw_w, &draw_h);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)draw_w, (float)draw_h));

        ImGuiWindowFlags main_flags =
            ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize  |
            ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
        ImGui::Begin("##main", nullptr, main_flags);

        // ── Left panel: virtual keyboard ──────────────────────────────────

        const float btn_w   = 70.0f;
        const float btn_h   = 56.0f;
        const float btn_gap = 4.0f;
        const float kbd_w   = 6 * btn_w + 5 * btn_gap + 2.0f;  // 442

        ImGui::BeginChild("##kb", ImVec2(kbd_w, 0), false, ImGuiWindowFlags_NoScrollbar);

        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Virtual Keyboard");
        ImGui::Spacing();

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(btn_gap, btn_gap));

        for (int row = 0; row < 6; ++row) {
            for (int col = 0; col < 6; ++col) {
                // Find the descriptor for this cell
                const GuiKey* gk = nullptr;
                for (int ki = 0; ki < kNumGuiKeys; ++ki) {
                    if (kGuiKeys[ki].row == row && kGuiKeys[ki].col == col) {
                        gk = &kGuiKeys[ki];
                        break;
                    }
                }

                if (col > 0) ImGui::SameLine(0.0f, btn_gap);

                if (!gk) {
                    // Empty placeholder
                    ImGui::Dummy(ImVec2(btn_w, btn_h));
                    continue;
                }

                // Button colour: green flash when active
                bool flash_active = is_flashing(flash, (uint8_t)row, (uint8_t)col);
                if (flash_active) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f, 0.65f, 0.10f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.80f, 0.15f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.05f, 0.50f, 0.05f, 1.0f));
                }

                // Build label string: hint \n label ## row_col
                char btn_label[128];
                snprintf(btn_label, sizeof(btn_label), "%s\n%s##%d_%d",
                         gk->hint, gk->label, row, col);

                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 4.0f));
                bool clicked = ImGui::Button(btn_label, ImVec2(btn_w, btn_h));
                ImGui::PopStyleVar();

                if (flash_active) {
                    ImGui::PopStyleColor(3);
                }

                if (clicked) {
                    fire_key((uint8_t)row, (uint8_t)col);
                }
            }
        }

        ImGui::PopStyleVar();  // ItemSpacing
        ImGui::EndChild();     // ##kb

        // ── Right panel: IME status ───────────────────────────────────────

        ImGui::SameLine(0.0f, 16.0f);
        ImGui::BeginChild("##ime", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);

        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "IME Status");
        ImGui::Separator();
        ImGui::Spacing();

        // Mode
        ImGui::Text("Mode:");
        ImGui::SameLine();
        ImGui::TextColored(mode_color(ime.mode()), "%s", mode_name(ime.mode()));
        ImGui::Spacing();

        // Input phoneme sequence
        ImGui::Text("Input:");
        ImGui::SameLine();
        if (ime.input_bytes() > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.95f, 0.20f, 1.0f), "%s", ime.input_str());
        } else {
            ImGui::TextDisabled("(empty)");
        }
        ImGui::Spacing();

        // Candidates
        ImGui::Text("Candidates:");
        ImGui::Spacing();

        int cand_count = ime.candidate_count();
        for (int i = 0; i < cand_count; ++i) {
            // Build label: ① word ##cand_i
            char cand_label[128];
            snprintf(cand_label, sizeof(cand_label), "%s %s##cand_%d",
                     kCircleNums[i], ime.candidate(i).word, i);

            if (ImGui::Button(cand_label)) {
                // Clicking a candidate commits it
                committed += ime.candidate(i).word;
                scroll_to_bottom = true;
                ime.clear_input();
            }
            if (i < cand_count - 1) ImGui::SameLine();
        }
        if (cand_count == 0) {
            ImGui::TextDisabled("(no candidates)");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Committed text area
        ImGui::Text("Committed text:");
        ImGui::BeginChild("##committed", ImVec2(0, 110), true);
        if (committed.empty()) {
            ImGui::TextDisabled("(nothing committed yet)");
        } else {
            ImGui::TextWrapped("%s", committed.c_str());
        }
        if (scroll_to_bottom) {
            ImGui::SetScrollHereY(1.0f);
            scroll_to_bottom = false;
        }
        ImGui::EndChild();  // ##committed

        ImGui::Spacing();

        // Clear button
        if (ImGui::Button("Clear committed")) {
            committed.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear input")) {
            ime.clear_input();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Dictionary status
        ImGui::Text("Dictionary:");
        ImGui::SameLine();
        if (dict_loaded && searcher.is_loaded()) {
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f),
                               "loaded  (%u keys)", searcher.key_count());
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f),
                               "not loaded  (run with --dat / --val)");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("ESC = quit   |   SPACE/Enter = commit   |   ` = cycle mode");

        ImGui::EndChild();  // ##ime

        ImGui::End();
        ImGui::PopStyleVar();  // WindowPadding

        // ── Render ────────────────────────────────────────────────────────

        ImGui::Render();

        int vp_w, vp_h;
        SDL_GL_GetDrawableSize(window, &vp_w, &vp_h);
        glViewport(0, 0, vp_w, vp_h);
        glClearColor(0.13f, 0.13f, 0.13f, 1.0f);
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
