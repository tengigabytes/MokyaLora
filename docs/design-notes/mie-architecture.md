# MokyaInput Engine (MIE) — Architecture Design Note

**Sub-project:** MokyaInput Engine (MIE)
**Location:** `firmware/mie/`
**Language:** C++11 (core), Python 3 (data pipeline tools)
**Status:** Phase 1 — environment setup & PC validation

---

## 1. Positioning & Goals

MIE is the input method engine for MokyaLora, developed as a **self-contained library**
within the main firmware tree. Its design allows it to be extracted into an independent
open-source repository in the future with minimal refactoring.

| Goal                  | Approach                                                        |
|-----------------------|-----------------------------------------------------------------|
| Cross-platform core   | Pure C++11, zero hardware-register dependencies                 |
| Taiwan localisation   | MoE standard character list + Academia Sinica corpus            |
| PSRAM-optimised search| Double-Array Trie (DAT) — 4 MB budget, sub-second lookup        |
| Portable extraction   | `git subtree split` ready — clean boundary at `firmware/mie/`  |

---

## 2. Software Stack

```
┌─────────────────────────────────────────────────────┐
│  Application (MokyaLora Core 1 / PC test harness)   │
├─────────────────────────────────────────────────────┤
│  IME-Logic         Key de-ambiguation, candidate     │  Partial reuse
│                    ranking, mode switching            │
├─────────────────────────────────────────────────────┤
│  Trie-Searcher     DAT binary search, frequency      │  Full reuse
│                    sort, fuzzy correction             │
├─────────────────────────────────────────────────────┤
│  Data (.bin files) font_glyphs, font_index,          │  Independent tool
│                    dict_dat, dict_values              │
├──────────────────────────┬──────────────────────────┤
│  HAL — RP2350            │  HAL — PC stub            │  Platform-specific
│  (PIO scan → KeyEvent)   │  (stdin → KeyEvent)       │
└──────────────────────────┴──────────────────────────┘
```

### Layer Responsibilities

| Layer          | Module          | Reusability    | Description                                        |
|----------------|-----------------|----------------|----------------------------------------------------|
| Data           | Data Pipeline   | Standalone tool| Python scripts convert Unifont + MoE dict to `.bin`|
| Core           | Trie-Searcher   | Full           | DAT binary search, frequency-ranked candidates     |
| Logic          | IME-Logic       | Partial        | 8104-layout key de-ambiguation, mode FSM           |
| Adaptation     | HAL-Port        | Platform-specific | Converts platform key events to `mie::KeyEvent` |

---

## 3. Directory Structure

```
firmware/mie/
├── CMakeLists.txt              # Builds as static library; no Pico SDK dependency
├── include/
│   └── mie/                    # All public headers — consumers include <mie/...>
│       ├── hal_port.h          # IHalPort interface + KeyEvent struct
│       ├── trie_searcher.h     # TrieSearcher public API
│       └── ime_logic.h         # ImeLogic public API
├── src/
│   ├── mie_init.cpp            # Placeholder TU (keeps library target non-empty)
│   ├── trie_searcher.cpp       # Sorted-index binary search implementation
│   └── ime_logic.cpp           # Bopomofo primary-phoneme map + mode FSM
├── hal/
│   ├── hal_port.h              # Shim → redirects to include/mie/hal_port.h
│   ├── rp2350/                 # RP2350 PIO scan → KeyEvent adapter (Phase 2)
│   └── pc/                     # PC keyboard adapters (host build only)
│       ├── key_map.h           #   Static PC key → KeyEvent{row,col} table
│       ├── hal_pc_stdin.h      #   HalPcStdin class declaration
│       └── hal_pc_stdin.cpp    #   IHalPort impl: raw terminal + non-blocking stdin
├── tools/
│   ├── gen_font.py             # Unifont → font_glyphs.bin + font_index.bin
│   ├── gen_dict.py             # MoE CSV → dict_dat.bin + dict_values.bin (MIED format)
│   ├── mie_repl.cpp            # Terminal REPL: IME-connected, keyboard + candidate bar
│   └── mie_gui.cpp             # GUI test tool: graphical keyboard + IME display (planned)
├── data/                       # Generated binary assets — NOT committed to git
│   ├── font_glyphs.bin
│   ├── font_index.bin
│   ├── dict_dat.bin
│   ├── dict_values.bin
│   └── dict_meta.json
└── tests/                      # GoogleTest unit tests (host-only build)
    ├── CMakeLists.txt
    ├── test_trie_stub.cpp       # Build-environment smoke test
    └── test_trie_searcher.cpp   # TrieSearcher unit tests (14 cases)
```

---

## 4. Data Pipeline

### 4.1 Font Compiler — `tools/gen_font.py`

**Input:**
- GNU Unifont `.hex` file (source: https://unifoundry.com/unifont/)
- `charlist_8104.txt` — Unicode code points for the 8,104 Taiwan MoE standard characters

**Output:**
- `font_glyphs.bin` — packed 16×16 monochrome glyph data (32 bytes / glyph)
- `font_index.bin` — `(codepoint: uint32, offset: uint32)` lookup table

**Flash budget:** 8,104 glyphs × 32 bytes = ~260 KB

### 4.2 Dictionary Compiler — `tools/gen_dict.py`

**Input:**
- MoE standard word list CSV (cleaned, Taiwan-standard Bopomofo readings)

**Output:**
- `dict_dat.bin` — MIED-format header + sorted key index + key-string data
- `dict_values.bin` — per-key word list with frequency weights
- `dict_meta.json` — build metadata (source version, entry count, build date)

**PSRAM budget:** target ≤ 4 MB total for DAT + values loaded at runtime

### 4.3 Asset Loading at Runtime

At boot, Core 1 loads `dict_dat.bin` and `dict_values.bin` from Flash into PSRAM:

```
Flash [0x0080_0000 + offset]  →  memcpy  →  PSRAM [base address]
```

Font glyphs remain in Flash and are read on demand (cache-friendly sequential access during rendering).

---

## 5. Input Modes

The MODE key cycles through three modes in order.

| # | Mode | `InputMode` | Description |
|---|------|-------------|-------------|
| 0 | Bopomofo | `Bopomofo` | Bopomofo syllable accumulation → Traditional Chinese candidate prediction |
| 1 | English | `English` | Half-keyboard letter-pair expansion → English word prediction |
| 2 | Alphanumeric | `Alphanumeric` | Multi-tap single character — English letters and digits |

### 5.1 Bopomofo Mode

Bopomofo syllable structure constrains which symbol can appear at each position:

```
[ 聲母 (initial) ] → [ 介音 (medial) ] → [ 韻母 (final) ] → [ 聲調 (tone) ]
```

Each physical key carries two phonemes. Phase 1 uses only the primary (first) phoneme.
Full disambiguation (Phase 3) will use the syllable position state machine to resolve
ambiguity: for example, after a medial `ㄧ`, only consonants compatible with `ㄧ`
are valid initials, eliminating half the candidates automatically.

### 5.2 English Mode

Each half-keyboard key carries two letters (e.g., `Q/W`, `E/R`). The search layer
expands an n-key sequence into up to 2ⁿ letter combinations and queries a
frequency-sorted English-language MIED dictionary (`en_dat.bin` / `en_val.bin`).
Results from all valid prefix combinations are merged and returned in frequency order.

Dictionary tool: `gen_en_dict.py` (planned) — converts a word-frequency list to MIED
format using the same binary layout as `gen_dict.py`; no changes to `TrieSearcher`.

### 5.3 Alphanumeric Mode

Multi-tap cycling with no dictionary lookup:

- First press of a key → primary character (e.g., `Q`).
- Consecutive press of the same key → secondary character (e.g., `W`).
- A different key press confirms the pending character and starts a new one.
- Number row (Row 0): each key produces its two printed digits.

State is tracked in `ImeLogic::MultiTapState {last_row, last_col, tap_count, pending}`.
A hardware timer (Phase 2+) will add auto-confirm on timeout.

---

## 6. Smart Correction

- **Spatial correction:** candidate scoring penalised by physical key distance (key adjacency matrix).
- **Phonetic fuzzy match:** near-homophone Bopomofo syllables mapped to allow lenient search.
- **Dynamic weighting:** candidate rank updated by per-session input history stored in LittleFS.

---

## 7. HAL Interface Contract

All platform implementations must satisfy `mie::IHalPort`:

```cpp
// hal/hal_port.h
namespace mie {
    struct KeyEvent { uint8_t row; uint8_t col; bool pressed; };
    class IHalPort {
    public:
        virtual ~IHalPort() = default;
        virtual bool poll(KeyEvent& out) = 0;  // non-blocking
    };
}
```

- RP2350 implementation (`hal/rp2350/`): reads from the DMA ring buffer populated by the PIO keypad scanner.
- PC implementation (`hal/pc/`): maps PC keyboard input to `KeyEvent` via a static key map table (see §7.1).

### 7.1 PC Virtual Half-Keyboard

The MokyaLora physical keyboard is a **half-keyboard** (ambiguous layout): each key carries
two letters and two Bopomofo symbols. The IME disambiguates based on context. A PC QWERTY
keyboard cannot reproduce this directly, so `hal/pc/` provides a virtual mapping.

**Mapping rule:** use the first letter printed on each physical key as the PC trigger key.

#### PC Key → Matrix Position Map

| PC Key | MokyaLora Key | Row | Col | Bopomofo |
|--------|--------------|-----|-----|----------|
| `1` | 1 2 | 0 | 0 | ㄅ ㄉ |
| `3` | 3 4 | 0 | 1 | ˇ ˋ |
| `5` | 5 6 | 0 | 2 | ㄓ ˊ |
| `7` | 7 8 | 0 | 3 | ˙ ㄚ |
| `9` | 9 0 | 0 | 4 | ㄞ ㄢ ㄦ |
| `q` | Q W | 1 | 0 | ㄆ ㄊ |
| `e` | E R | 1 | 1 | ㄍ ㄐ |
| `t` | T Y | 1 | 2 | ㄔ ㄗ |
| `u` | U I | 1 | 3 | ㄧ ㄛ |
| `o` | O P | 1 | 4 | ㄟ ㄣ |
| `a` | A S | 2 | 0 | ㄇ ㄋ |
| `d` | D F | 2 | 1 | ㄎ ㄑ |
| `g` | G H | 2 | 2 | ㄕ ㄘ |
| `j` | J K | 2 | 3 | ㄨ ㄜ |
| `l` | L   | 2 | 4 | ㄠ ㄤ |
| `z` | Z X | 3 | 0 | ㄈ ㄌ |
| `c` | C V | 3 | 1 | ㄏ ㄒ |
| `b` | B N | 3 | 2 | ㄖ ㄙ |
| `m` | M   | 3 | 3 | ㄩ ㄝ |
| `\` | —   | 3 | 4 | ㄡ ㄥ |
| F1 | FUNC | 0 | 5 | — |
| F2 | SET  | 1 | 5 | — |
| `Backspace` | BACK | 2 | 5 | — |
| `Delete`    | DEL  | 3 | 5 | — |
| `` ` ``     | MODE | 4 | 0 | — |
| `Tab`       | TAB  | 4 | 1 | — |
| `Space`     | SPACE | 4 | 2 | — |
| `,`         | ，SYM | 4 | 3 | — |
| `.`         | 。.？  | 4 | 4 | — |
| `=`         | VOL+  | 4 | 5 | — |
| `↑`         | UP    | 5 | 0 | — |
| `↓`         | DOWN  | 5 | 1 | — |
| `←`         | LEFT  | 5 | 2 | — |
| `→`         | RIGHT | 5 | 3 | — |
| `Enter`     | OK    | 5 | 4 | — |
| `-`         | VOL−  | 5 | 5 | — |

#### Files

| File | Description |
|------|-------------|
| `hal/pc/key_map.h` | Static `pc_key_map[]` table: `char → KeyEvent{row, col}` |
| `hal/pc/hal_pc_stdin.cpp` | `IHalPort` implementation; sets terminal to raw mode, polls stdin |
| `tools/mie_repl.cpp` | Interactive REPL: renders virtual keyboard layout + candidate bar in terminal |

#### REPL Terminal Layout

```
┌─ MokyaLora Virtual Keyboard ──────────────────────────────────┐
│ [1:ㄅㄉ][3:ˇˋ ][5:ㄓˊ][7:˙ㄚ][9:ㄞㄢ][F1:FUNC]             │
│ [q:ㄆㄊ][e:ㄍㄐ][t:ㄔㄗ][u:ㄧㄛ][o:ㄟㄣ][F2:SET ]          │
│ [a:ㄇㄋ][d:ㄎㄑ][g:ㄕㄘ][j:ㄨㄜ][l:ㄠㄤ][BS:BACK]          │
│ [z:ㄈㄌ][c:ㄏㄒ][b:ㄖㄙ][m:ㄩㄝ][\:ㄡㄥ][Del:DEL ]         │
│ [`:MODE][Tab   ][Space      ][,:SYM][.:。？][=:VOL+]          │
│ [↑     ][↓     ][←         ][→    ][↵:OK  ][-:VOL-]          │
├────────────────────────────────────────────────────────────────┤
│ 輸入序列：ㄐ ㄧ ㄣ                                             │
│ 候選字：  ① 今  ② 金  ③ 巾  ④ 近  ⑤ 盡                       │
└────────────────────────────────────────────────────────────────┘
```

### 7.2 PC GUI Test Tool (`mie_gui`)

`tools/mie_gui.cpp` is a host-only graphical application that provides an interactive
virtual keyboard and IME status display. It targets developers who need to test the full
key-input → disambiguation → candidate pipeline without physical MokyaLora hardware.

#### Technology Stack

| Component | Library | Rationale |
|-----------|---------|-----------|
| GUI framework | Dear ImGui (docking branch) | Immediate-mode, zero external styling, single-header, MIT |
| Backend renderer | SDL2 + OpenGL 3 | Cross-platform; `imgui_impl_sdl2` + `imgui_impl_opengl3` bundled with ImGui |
| IME integration | `mie::ImeLogic` (existing) | Pure display consumer — no changes to MIE core library |

The tool is a **pure display consumer**: it calls `ImeLogic::process_key()` and reads
`ImeLogic::input_str()` / `ImeLogic::candidate()`. No MIE source files are modified.

#### UI Layout

```
┌─────────────────────────────────────────────────────────────────────┐
│  MokyaLora IME Test Tool                                            │
├──────────────────────────────┬──────────────────────────────────────┤
│  Virtual Keyboard            │  IME Status                          │
│                              │                                      │
│  [1:ㄅㄉ][3:ˇˋ][5:ㄓˊ]...  │  Mode:  Bopomofo                    │
│  [q:ㄆㄊ][e:ㄍㄐ][t:ㄔㄗ]  │  Input: ㄐ ㄧ ㄣ                   │
│  [a:ㄇㄋ][d:ㄎㄑ][g:ㄕㄘ]  │                                      │
│  [z:ㄈㄌ][c:ㄏㄒ][b:ㄖㄙ]  │  ① 今   ② 金   ③ 巾               │
│  [`][Tab ][Space    ][,][.]  │  ④ 近   ⑤ 盡                       │
│  [↑][↓  ][←        ][→][↵]  │                                      │
│                              │  Committed: 今天                     │
└──────────────────────────────┴──────────────────────────────────────┘
```

- **Left panel — Virtual Keyboard:** 6×6 button grid matching MokyaLora's physical layout.
  Each button shows its PC trigger key and both Bopomofo phonemes (or function label).
  The active key is highlighted on press. Clicking a button fires the same `KeyEvent` as
  the corresponding keyboard shortcut.
- **Right panel — IME Status:** displays current mode, the accumulated input phoneme
  string, up to 10 numbered candidates, and the committed output text.

#### Architecture

```
SDL2 event loop
    │
    ├── keyboard / mouse event → KeyEvent{row, col, pressed}
    │       (same mapping as hal/pc/key_map.h)
    │
    └── ImeLogic::process_key(event)
            │
            ├── ImeLogic::input_str()       → render input phoneme display
            ├── ImeLogic::candidate_count() → render candidate list
            └── ImeLogic::candidate(i)      → render each candidate button
```

`mie_gui` links against the `mie` static library and `HalPcStdin` is **not** used
(events come from ImGui/SDL2 directly). The `pc_key_map[]` table from `hal/pc/key_map.h`
is reused to convert SDL keycodes to `KeyEvent` values.

#### CMake Target

```cmake
# Host-only; guarded by MIE_BUILD_GUI option (OFF by default)
if(MIE_BUILD_GUI)
    find_package(SDL2 REQUIRED)
    add_executable(mie_gui tools/mie_gui.cpp ${IMGUI_SOURCES})
    target_link_libraries(mie_gui PRIVATE mie SDL2::SDL2)
endif()
```

#### Development Milestones

| Milestone | Deliverable | Status |
|-----------|-------------|--------|
| A | CMake integration: `MIE_BUILD_GUI` option; FetchContent for ImGui + SDL2 | Planned |
| B | Window opens; 6×6 keyboard grid renders with correct labels | Planned |
| C | Keyboard input (PC keys + button clicks) fires `ImeLogic::process_key()`; IME status panel updates live | Planned |
| D | Dict file path arguments (`--dat`, `--val`); load real dictionary; full end-to-end candidate display | Planned |

---

## 8. Development Roadmap

### Phase 1 — Environment Setup & PC Validation

- [x] Write `gen_font.py` and verify output against a reference glyph set.
      — Script complete; end-to-end run requires Unifont source + charlist_8104.txt (pending).
- [x] Write `gen_dict.py`, validate DAT search correctness on PC.
      — MIED binary format implemented; end-to-end run requires MoE CSV source (pending).
- [x] Implement `Trie-Searcher` in C++; unit-test with Google Test on PC.
      — `src/trie_searcher.cpp` complete; 13 unit tests passing.  See `docs/design-notes/mie-implementation.md`.
- [x] Implement `IME-Logic` Bopomofo mode de-ambiguation; test with simulated key sequences.
      — Phase 1 skeleton: primary-phoneme mapping, mode FSM, REPL integration.
        Full disambiguation (two-alternative + fuzzy correction) deferred to Phase 3.
- [x] **Phase 1 wrap-up:** Remove `Calculator` mode; MODE key cycles three modes (`% 3`);
      mode-dispatch skeleton (`process_bopomofo` / `process_english` / `process_alpha`);
      `MultiTapState` struct added; all 14 unit tests still passing.
- [ ] **Phase 1 extension — Alphanumeric:** Implement multi-tap cycling in `process_alpha()`;
      `MultiTapState` tracks last key + tap count; different key confirms pending character.
- [ ] **Phase 1 extension — English dictionary:** `gen_en_dict.py` converts word-frequency
      list to MIED format; `ImeLogic` accepts a second `TrieSearcher` for English;
      `process_english()` expands letter pairs and queries prefix search.
- [x] **GUI tool — Milestone A:** CMake `MIE_BUILD_GUI` option; FetchContent for Dear ImGui (v1.91.6) + SDL2 (v2.26.5, static).
- [x] **GUI tool — Milestone B:** Window with 6×6 virtual keyboard grid (correct labels, 150 ms green highlight on press).
- [x] **GUI tool — Milestone C:** Keyboard input (PC keys + button clicks) → `ImeLogic::process_key()`; live IME status panel (mode, input, candidates, committed text).
- [x] **GUI tool — Milestone D:** `--dat`/`--val` CLI arguments; dictionary status indicator; full candidate display with click-to-commit.

### Phase 2 — Hardware Integration (MokyaLora Rev A)

- [ ] Implement `hal/rp2350/` — bridge PIO+DMA key state buffer to `mie::KeyEvent`.
- [ ] Load DAT + values from Flash to PSRAM at boot; validate search latency.
- [ ] Render `font_glyphs.bin` on the NHD 2.4″ display via LVGL custom font driver.
- [ ] Integrate candidate bar UI widget with `Trie-Searcher` output.

### Phase 3 — Optimisation & Extension

- [ ] Spatial and phonetic fuzzy correction.
- [ ] User-defined word list stored in LittleFS, merged into DAT at runtime.
- [ ] Additional language pack slots (Japanese kana, Latin-script prediction).

---

## 9. Future Extraction to Standalone Repository

When MIE is stable enough to stand alone:

1. `git subtree split --prefix=firmware/mie -b mie-standalone`
2. Push `mie-standalone` branch to a new repository.
3. Replace `firmware/mie/` in this repo with `git submodule add <new-repo-url> firmware/mie`.

The HAL interface contract and CMakeLists.txt structure are designed to make this
transition require zero changes to the core library code.
