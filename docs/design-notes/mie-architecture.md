# MokyaInput Engine (MIE) — Architecture Design Note

**Sub-project:** MokyaInput Engine (MIE)
**Location:** `firmware/mie/`
**Language:** C++11 (core), Python 3 (data pipeline tools)
**Status:** Phase 1.5 complete — C API shipped, standalone repo extraction deferred

---

## 1. Positioning & Goals

MIE is the input method engine for MokyaLora, developed as a **self-contained library**
within the main firmware tree. Its design allows it to be extracted into an independent
open-source repository in the future with minimal refactoring.

| Goal                     | Approach                                                               |
|--------------------------|------------------------------------------------------------------------|
| Cross-platform core      | Pure C++11, zero hardware-register dependencies                        |
| Taiwan localisation      | MoE standard character list + Academia Sinica corpus                   |
| PSRAM-optimised search   | Double-Array Trie (DAT) — 4 MB budget, sub-second lookup               |
| Standalone extraction    | `libmie-standalone` branch created; push to `tengigabytes/libmie` deferred |
| Android / Windows targets| C API `include/mie/mie.h` + `src/mie_c_api.cpp` **done**; JNI/TSF wrappers planned |

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
│  (PIO scan +             │  (stdin → keycode →       │
│   keymap_matrix →        │   KeyEvent)               │
│   keycode → KeyEvent)    │                           │
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
│   ├── ime_internal.h          # Internal inline helpers (NOT public)
│   ├── ime_logic.cpp           # Constructor + process_key dispatcher (~120 lines)
│   ├── ime_keys.cpp            # Static phoneme/symbol tables + key_to_* lookups
│   ├── ime_search.cpp          # run_search, build_merged, rebuild_input_buf
│   ├── ime_display.cpp         # compound_input_str, matched_prefix_*, display helpers
│   ├── ime_commit.cpp          # do_commit, do_commit_partial, did_commit
│   ├── ime_smart.cpp           # process_smart (SmartZh + SmartEn modes)
│   └── ime_direct.cpp          # process_direct, process_sym_key, commit_sym_pending
├── hal/
│   ├── hal_port.h              # Shim → redirects to include/mie/hal_port.h
│   ├── rp2350/                 # RP2350 PIO scan + keymap_matrix → keycode → KeyEvent (Phase 2)
│   └── pc/                     # PC keyboard adapters (host build only)
│       ├── key_map.h           #   Static PC key → mokya_keycode_t table
│       ├── hal_pc_stdin.h      #   HalPcStdin class declaration
│       └── hal_pc_stdin.cpp    #   IHalPort impl: raw terminal + non-blocking stdin
├── tools/
│   ├── gen_font.py             # Unifont → font_glyphs.bin + font_index.bin
│   ├── gen_dict.py             # MoE CSV → dict_dat.bin + dict_values.bin (MIED format)
│   ├── mie_repl.cpp            # Terminal REPL: IME-connected, keyboard + candidate bar
│   └── mie_gui.cpp             # GUI test tool: graphical keyboard + IME display
├── data/                       # Generated binary assets — NOT committed to git
│   ├── font_glyphs.bin
│   ├── font_index.bin
│   ├── dict_dat.bin
│   ├── dict_values.bin
│   └── dict_meta.json
└── tests/                      # GoogleTest unit tests (host-only build)
    ├── CMakeLists.txt
    ├── test_helpers.h           # Shared fixture: TEntry, kev helpers, build_single/multi
    ├── test_trie_stub.cpp       # Build-environment smoke test
    ├── test_trie_searcher.cpp   # TrieSearcher unit tests (14 cases)
    ├── test_ime_basic.cpp       # ImeLogic: SmartMode, DirectMode, SymbolKeys, CandidateNav
    ├── test_ime_modes.cpp       # ImeLogic: ModeSwitch, ModeSeparation
    └── test_ime_advanced.cpp    # ImeLogic: GreedyPrefix, ToneSort, SmartEn, DirectBopomofo…
```

---

## 4. Data Pipeline

### 4.1 Font Compiler — `tools/gen_font.py`

#### Inputs
| Source | Notes |
|--------|-------|
| GNU Unifont latest `.ttf` | https://unifoundry.com/unifont/ — OFL 1.1 + GPL v2+ with font exception |

#### Processing
1. **Subset** the TTF via `fonttools` to only the specified Unicode ranges (reduces memory
   for the render step):

   | Range | Block |
   |-------|-------|
   | U+0020–U+007F | ASCII |
   | U+00A0–U+00FF | Latin-1 Supplement (×÷ etc.) |
   | U+2000–U+206F | General Punctuation (— …) |
   | U+3000–U+303F | CJK Symbols & Punctuation |
   | U+3100–U+312F | Bopomofo |
   | U+4E00–U+9FFF | CJK Unified Ideographs (繁體) |

2. **Render** each codepoint at **16 px** with `Pillow` (`ImageFont.truetype`).
3. **Threshold** to **1 bpp** (MSB-first, each row padded to full byte).
4. **Optional RLE** compression pass (flag `--rle`).

#### Output format — MIEF v1 (`mie_unifont_16.bin`)

```
Header (12 bytes, little-endian):
  magic[4]     = b'MIEF'
  version      = uint8  (= 1)
  px_height    = uint8  (= 16)
  bpp          = uint8  (= 1)
  flags        = uint8  (bit 0 = RLE enabled)
  num_glyphs   = uint32

Index (num_glyphs × 8 bytes, sorted by codepoint):
  codepoint    = uint32
  data_offset  = uint32   (byte offset into glyph-data section)

Glyph data (variable-length, pointed to by index):
  adv_w  = uint8   (advance width in pixels)
  box_w  = uint8   (bitmap width;  0 = empty/whitespace glyph)
  box_h  = uint8   (bitmap height; 0 = empty/whitespace glyph)
  ofs_x  = int8    (left bearing from origin)
  ofs_y  = int8    (bottom bearing from baseline, positive = up)
  bitmap = bytes   (1 bpp, MSB first; present only when box_w > 0)
```

#### LVGL integration (Phase 2)
A custom `lv_font_t` driver (`firmware/core1/src/mie_font_driver.c`) will implement
`get_glyph_dsc` / `get_glyph_bitmap` callbacks that binary-search the MIEF index and
read bitmap data directly from Flash (no PSRAM copy needed).

#### Dependency & build
```
pip install fonttools Pillow
python firmware/mie/tools/gen_font.py \
    --unifont unifont.ttf \
    --out firmware/mie/data/mie_unifont_16.bin
```

**Flash budget:** ~28,000 codepoints × ~35 bytes avg ≈ ~1 MB

---

### 4.2 Dictionary Compiler — `tools/gen_dict.py`

#### Design goals (Nokia-style prefix prediction)
- Press any key → candidates narrow in real time as keys are pressed.
- No phoneme disambiguation in the user's head: the engine resolves ambiguity.
- Same TrieSearcher binary-search engine for Chinese **and** English.
- O(log N) prefix lookup at runtime; O(1) hash fast-path for short sequences (Phase 3).

#### Half-keyboard KEYMAP (`firmware/mie/tools/gen_dict.py`)

Each physical key (row 0–3, col 0–4) carries 2–3 Bopomofo phonemes.
The compiler pre-computes: **phoneme → key index** (0–19, one per physical key).

```
Key index = row × 5 + col

  (0,0) idx=0  ㄅ ㄉ      (0,1) idx=1  ˇ ˋ      (0,2) idx=2  ㄓ ˊ
  (0,3) idx=3  ˙ ㄚ       (0,4) idx=4  ㄞ ㄢ ㄦ

  (1,0) idx=5  ㄆ ㄊ      (1,1) idx=6  ㄍ ㄐ     (1,2) idx=7  ㄔ ㄗ
  (1,3) idx=8  ㄧ ㄛ      (1,4) idx=9  ㄟ ㄣ

  (2,0) idx=10 ㄇ ㄋ      (2,1) idx=11 ㄎ ㄑ     (2,2) idx=12 ㄕ ㄘ
  (2,3) idx=13 ㄨ ㄜ      (2,4) idx=14 ㄠ ㄤ

  (3,0) idx=15 ㄈ ㄌ      (3,1) idx=16 ㄏ ㄒ     (3,2) idx=17 ㄖ ㄙ
  (3,3) idx=18 ㄩ ㄝ      (3,4) idx=19 ㄡ ㄥ
```

Key sequence encoding: `byte = key_index + 0x21` (maps 0–19 → ASCII `!`–`4`,
avoids null bytes, directly usable as a `std::string` key in TrieSearcher).

#### Data sources

| Input | Format | Content |
|-------|--------|---------|
| `--libchewing tsi.src` | libchewing-data tab-separated | Traditional Chinese word list with Bopomofo + frequency |
| `--moe-csv moe.csv` | MoE CSV (BOM UTF-8) | 注音 / 詞語 / 頻率 columns |
| `--en-wordlist wordlist.txt` | one word per line (optional freq) | English words ≥ 50,000 entries |

#### Processing — Chinese

1. Parse each entry → `(word, phoneme_list, freq)`.
2. For each phoneme, look up `PHONEME_TO_KEY[ph]` → `key_index`.
   Tone 1 (no mark) is skipped (it has no physical key).
3. Encode key sequence: `bytes(k + 0x21 for k in key_indices)`.
4. Multiple words can share a key sequence (ambiguity) — they are merged
   into the same value record, sorted by frequency descending.
5. Output sorted by key sequence → MIED format.

Example — "百年" (ㄅㄞˇ ㄋㄧㄢˊ):
```
ㄅ(0) ㄞ(4) ˇ(1)  ㄋ(10) ㄧ(8) ㄢ(4) ˊ(2)
→ key seq bytes: [33,37,34, 43,41,37,35]
→ key seq str:   "!%\"  +)%#"
```
All words sharing these physical key presses appear together in the candidate list.

#### Processing — English

English KEYMAP: letter → key index (rows 1–3 only; row 0 = numeric layer).

```
Q/W → 5   E/R → 6   T/Y → 7   U/I → 8   O/P → 9
A/S → 10  D/F → 11  G/H → 12  J/K → 13  L → 14
Z/X → 15  C/V → 16  B/N → 17  M → 18
```

Same key-sequence encoding (+0x21). Each word's key sequence is built letter by letter.
Stored separately in `en_dat.bin` / `en_values.bin` (same MIED format, second
TrieSearcher instance in ImeLogic).

#### Outputs

| File | Contents |
|------|----------|
| `dict_dat.bin` | Chinese key-sequence MIED index |
| `dict_values.bin` | Chinese word pool |
| `en_dat.bin` | English key-sequence MIED index |
| `en_values.bin` | English word pool |
| `dict_meta.json` | Build metadata + license notices |

**PSRAM budget:** Chinese dict ≤ 3 MB, English dict ≤ 0.5 MB, total ≤ 4 MB.

#### MIED v2 ValueRecord format

The v2 value layout (used by `gen_dict.py` since Phase 1 completion) extends v1 with a
tone byte per word entry:

```
ValueRecord (per word, inside a values-file entry):
  word_count : uint16                    (number of candidate words at this key)
  per-word records (repeated word_count times):
    freq     : uint16                    (corpus frequency)
    tone     : uint8                     (Bopomofo tone: 1=陰平 2=陽平ˊ 3=上聲ˇ 4=去聲ˋ 5=輕聲˙  0=unknown/English)
    word_len : uint8                     (UTF-8 byte length of the word)
    word_utf8: bytes[word_len]           (UTF-8 encoded candidate word)
```

`TrieSearcher::dict_version()` returns 1 or 2.  v1 dicts (tone byte absent) are still
accepted — ImeLogic treats all tone values as 0 (unknown) and falls back to full
frequency-sorted candidate ordering.

#### Runtime query (ImeLogic → TrieSearcher)
```cpp
// Bopomofo mode: after pressing key indices [0, 4, 1]
std::string ks;
for (uint8_t k : pressed_keys) ks += char(k + 0x21);
auto results = zh_searcher_.search(ks, /*max=*/10);  // prefix match
```

#### Dependency & build
```
pip install (none — stdlib only)
python firmware/mie/tools/gen_dict.py \
    --libchewing tsi.src \
    --moe-csv    moe_dict.csv \
    --en-wordlist en_wordlist.txt
```

---

### 4.3 Asset Loading at Runtime

At boot, Core 1 loads `dict_dat.bin` and `dict_values.bin` from Flash into PSRAM:

```
Flash [0x0080_0000 + offset]  →  memcpy  →  PSRAM [base address]
```

Font glyphs remain in Flash and are read on demand (cache-friendly sequential access during rendering).

---

## 5. Input Modes

The MODE key cycles through five modes in order.  Each press of MODE advances by one;
wraps back to `SmartZh` after `DirectBopomofo`.

| # | `InputMode` enum | Trigger | Description |
|---|-----------------|---------|-------------|
| 0 | `SmartZh` | default / MODE×0 | Bopomofo prefix prediction; SPACE appends first-tone marker `ˉ` |
| 1 | `SmartEn` | MODE×1 | Half-keyboard letter-pair English prediction (MIED `en_dat.bin`) |
| 2 | `DirectUpper` | MODE×2 | Multi-tap uppercase letters / digits |
| 3 | `DirectLower` | MODE×3 | Multi-tap lowercase letters |
| 4 | `DirectBopomofo` | MODE×4 | Single Bopomofo phoneme cycling; produces single-char candidates |

### 5.1 Tone-Aware Candidate Ranking (SmartZh)

After a key sequence has a matched prefix in the trie, ImeLogic extracts a **tone intent**
from the trailing key bytes and applies a 4-tier sort before presenting candidates:

**Tone intent extraction:**
- Trailing byte `0x22` (key index 1, carries ˇ/ˋ) → intent 34 (tone 3 or tone 4).
- SPACE (`0x20`) appended after a matched prefix → intent 1 (first tone / 陰平).
- No trailing tone key → intent 0 (no filter applied).

**4-tier sort (within each tier: frequency descending):**

| Tier | Condition |
|------|-----------|
| 0 | single-char word **and** tone matches intent |
| 1 | multi-char word **and** tone matches intent |
| 2 | single-char word, tone does not match |
| 3 | multi-char word, tone does not match |

**Strict filter:** when intent ≠ 0, only tier-0 and tier-1 candidates are shown.
If no tier-0/1 candidates exist (e.g., v1 dict without tone data), the filter is
automatically disabled and the full frequency-sorted list is presented (backwards compat).

**First-tone SPACE marker:** `0x20` is appended to `key_seq_buf_`; rendered as `ˉ` in
the compound display; consumed by `do_commit_partial` immediately after commit so it
does not bleed into the next word.

### 5.2 SmartZh Mode (Bopomofo prediction)

Bopomofo syllable structure constrains which symbol can appear at each position:

```
[ 聲母 (initial) ] → [ 介音 (medial) ] → [ 韻母 (final) ] → [ 聲調 (tone) ]
```

Each physical key carries two phonemes.  Primary-phoneme mapping is used; full
phoneme-position disambiguation (Phase 3) will use the syllable state machine to
resolve ambiguity automatically.

### 5.3 SmartEn Mode (English prediction)

Each half-keyboard key carries two letters (e.g., `Q/W`, `E/R`). The search layer
expands an n-key sequence into up to 2ⁿ letter combinations and queries a
frequency-sorted English MIED dictionary (`en_dat.bin` / `en_values.bin`).
Results from all valid prefix combinations are merged and returned in frequency order.

### 5.4 DirectUpper / DirectLower Modes (multi-tap)

Multi-tap cycling with no dictionary lookup.  `DirectUpper` produces uppercase letters
and digits; `DirectLower` produces lowercase letters.

- First press of a key → primary character.
- Consecutive press of the same key → next character in the slot's cycle list.
- A different key press (or BACK) confirms the pending character and starts a new one.

### 5.5 DirectBopomofo Mode

Single Bopomofo phoneme per key, cycling through the two phonemes printed on each key.
Produces single-character candidates that match the selected phoneme exactly.
No multi-character word prediction.

---

## 6. Smart Correction

- **Spatial correction:** candidate scoring penalised by physical key distance (key adjacency matrix).
- **Phonetic fuzzy match:** near-homophone Bopomofo syllables mapped to allow lenient search.
- **Dynamic weighting:** candidate rank updated by per-session input history stored in LittleFS.

---

## 7. HAL Interface Contract

MIE is a **service**: it accepts a stream of keycode events and produces
input/candidate state. It has no knowledge of matrix geometry, scan hardware,
or physical key layout — all of that is confined to the adapters below the
`IHalPort` interface and, on the embedded target, to
`firmware/core1/src/keymap_matrix.h`.

All platform implementations must satisfy `mie::IHalPort`:

```cpp
// include/mie/hal_port.h
#include <mie/keycode.h>   // MOKYA_KEY_* constants, 0x01..0x3F

namespace mie {
    struct KeyEvent { uint8_t keycode; bool pressed; };
    class IHalPort {
    public:
        virtual ~IHalPort() = default;
        virtual bool poll(KeyEvent& out) = 0;   // non-blocking
    };
}
```

`keycode` is a value from `firmware/mie/include/mie/keycode.h` — a compact
semantic enumeration where the power button, matrix keys, and future side
buttons all share the same namespace (`0x01..0x3F`). `0x00` is reserved
(`MOKYA_KEY_NONE`).

Two header rule:

| Header                                        | Owner       | Purpose                                                    |
|-----------------------------------------------|-------------|------------------------------------------------------------|
| `firmware/mie/include/mie/keycode.h`          | MIE (MIT)   | Canonical keycode constants. Shared by MIE, Core 1 UI, USB Control Protocol, and host tooling (Python binding generates `Key` enum from this file). |
| `firmware/core1/src/keymap_matrix.h`          | Core 1      | 6×6 matrix `(row, col) → keycode` lookup. Used **once** inside `KeypadScan` before events enter the KeyEvent queue. Nothing above `KeypadScan` sees matrix coordinates. |

- RP2350 implementation (`hal/rp2350/`): reads from the DMA ring buffer populated by the PIO keypad scanner, applies `keymap_matrix.h`, and emits `KeyEvent { keycode, pressed }`.
- PC implementation (`hal/pc/`): maps PC keyboard input directly to `keycode` — no matrix translation, since the PC has no matrix (see §7.1).

### 7.1 PC Virtual Half-Keyboard

The MokyaLora physical keyboard is a **half-keyboard** (ambiguous layout): each key carries
two letters and two Bopomofo symbols. The IME disambiguates based on context. A PC QWERTY
keyboard cannot reproduce this directly, so `hal/pc/` provides a virtual mapping.

**Mapping rule:** use the first letter printed on each physical key as the PC trigger key.

#### PC Key → MokyaLora Keycode Map

The `Row` and `Col` columns below show the **hardware matrix location** for
reference only — they are not used by the PC HAL, which maps directly to
`keycode`. The canonical `MOKYA_KEY_*` constants and their numeric values
live in `firmware/mie/include/mie/keycode.h`.

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
| `hal/pc/key_map.h` | Static `pc_key_map[]` table: `char → keycode_t` (one lookup, no matrix translation) |
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

The keyboard panel mirrors the physical PCB layout from the assembly drawing:

```
┌────────────────────────────────────────────────┬──────────────────────────┐
│  Virtual Keyboard                              │  IME Status              │
│                                                │                          │
│  Navigation & Control                          │  Mode:  注音             │
│  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐       │  Input: ㄐ ㄧ ㄣ        │
│  │ FUNC │  │  ▲   │  │ SET  │  │ VOL+ │       │                          │
│  └──────┘  ├──────┤  └──────┘  └──────┘       │  ① 今  ② 金  ③ 巾      │
│            │◄ OK ►│                            │  ④ 近  ⑤ 盡             │
│  ┌──────┐  ├──────┤  ┌──────┐  ┌──────┐       │                          │
│  │ BACK │  │  ▼   │  │ DEL  │  │ VOL- │       │  Committed: 今天         │
│  └──────┘  └──────┘  └──────┘  └──────┘       │                          │
│  Core Input (5×4)                              │                          │
│  ┌──────┬──────┬──────┬──────┬──────┐          │                          │
│  │1/2   │3/4   │5/6   │7/8   │9/0   │          │                          │
│  │ㄅ ㄉ │ˇ ˋ  │ㄓ ˊ │˙ ㄚ │ㄞㄢㄦ│          │                          │
│  │[ANS] │[7]   │[8]   │[9]   │[÷]   │          │                          │
│  ├──────┼──────┼──────┼──────┼──────┤          │                          │
│  │ Q/W  │ E/R  │ T/Y  │ U/I  │ O/P  │          │                          │
│  │ㄆ ㄊ │ㄍ ㄐ │ㄔ ㄗ │ㄧ ㄛ │ㄟ ㄣ │          │                          │
│  │[(]   │[4]   │[5]   │[6]   │[×]   │          │                          │
│  ├──────┼──────┼──────┼──────┼──────┤          │                          │
│  │ A/S  │ D/F  │ G/H  │ J/K  │  L   │          │                          │
│  │ㄇ ㄋ │ㄎ ㄑ │ㄕ ㄘ │ㄨ ㄜ │ㄠ ㄤ │          │                          │
│  │[)]   │[1]   │[2]   │[3]   │[-]   │          │                          │
│  ├──────┼──────┼──────┼──────┼──────┤          │                          │
│  │ Z/X  │ C/V  │ B/N  │  M   │  —   │          │                          │
│  │ㄈ ㄌ │ㄏ ㄒ │ㄖ ㄙ │ㄩ ㄝ │ㄡ ㄥ │          │                          │
│  │[AC]  │[0]   │[.]   │[x10ˣ]│[+]   │          │                          │
│  ├──────┼──────┼──────┼──────┼──────┤          │                          │
│  │ MODE │ TAB  │SPACE │，SYM │。.？ │          │                          │
│  └──────┴──────┴──────┴──────┴──────┘          │                          │
└────────────────────────────────────────────────┴──────────────────────────┘
```

- **Left panel — Virtual Keyboard:** matches the physical PCB assembly layout.
  - *Navigation & Control area (top):* FUNC and BACK on the left; D-pad cluster
    (▲ / ◄ OK ► / ▼) in the centre; SET and DEL on the right; VOL+ and VOL- far right.
  - *Core Input area (below):* 5×4 grid where each key shows three label lines
    (English letters / Bopomofo phonemes / Calculator layer), plus a 5-key function bar
    (MODE TAB SPACE ，SYM 。.？). Keys flash green for 150 ms on press.
    Hovering shows the PC keyboard shortcut in a tooltip.
- **Right panel — IME Status:** displays current mode, the accumulated input phoneme
  string, up to 10 numbered candidates (click to commit), and the committed output text.

#### Architecture

```
SDL2 event loop
    │
    ├── keyboard / mouse event → KeyEvent{keycode, pressed}
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
is reused to convert SDL keycodes to MokyaLora keycodes.

#### CMake Target

```cmake
# Host-only; guarded by MIE_BUILD_GUI option (OFF by default)
# FetchContent pulls Dear ImGui v1.91.6 and SDL2 v2.26.5 (static).
option(MIE_BUILD_GUI "Build mie_gui graphical test tool" OFF)
if(MIE_BUILD_GUI AND NOT CMAKE_CROSSCOMPILING)
    # imgui_lib static target built manually (ImGui has no CMakeLists.txt)
    add_executable(mie_gui tools/mie_gui.cpp)
    target_compile_definitions(mie_gui PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(mie_gui PRIVATE mie imgui_lib)
endif()
```

Build helper: `build_mie_gui.bat` (Windows) — configures with `-DMIE_BUILD_GUI=ON` and
builds the `mie_gui` target using the MSVC/Ninja toolchain.

#### Development Milestones

| Milestone | Deliverable | Status |
|-----------|-------------|--------|
| A | CMake `MIE_BUILD_GUI` option; FetchContent for Dear ImGui v1.91.6 + SDL2 v2.26.5 (static) | Done |
| B | Hardware-accurate virtual keyboard: nav cluster at top (FUNC/BACK/D-pad/SET/DEL/VOL), 5×4 input grid + function bar below; 3-layer key labels (English / Bopomofo / Calc) | Done |
| C | Keyboard input (PC keys + button clicks) → `ImeLogic::process_key()`; live IME status panel (mode, input, candidates, committed text) | Done |
| D | `--dat`/`--val` CLI arguments; dictionary status indicator; full candidate display with click-to-commit | Done |

---

## 8. Development Roadmap

### Phase 1 — PC Validation ✓ complete

- [x] `gen_font.py`: extract 8,104 glyphs from GNU Unifont; output MIEF v1 binary.
- [x] `gen_dict.py`: compile MoE word list + English word list to MIED v2 (with tone byte); validate on PC.
- [x] `Trie-Searcher`: binary search on sorted key index; `dict_version()` accessor; v1/v2 compat.
- [x] `IME-Logic`: 5 input modes (SmartZh, SmartEn, DirectUpper, DirectLower, DirectBopomofo).
- [x] Tone-aware candidate ranking (4-tier sort, strict filter, first-tone SPACE marker).
- [x] English MIED dictionary; `ImeLogic` accepts second `TrieSearcher`; `SmartEn` mode live.
- [x] GUI tool (mie_gui): Dear ImGui + SDL2; virtual keyboard; live candidate display; click-to-commit.
- [x] **83 GoogleTest cases passing** (69 ImeLogic + 14 TrieSearcher).

### Phase 1.5 — Standalone Repo & C API ✓ complete

- [x] Split `src/ime_logic.cpp` (991 lines) → 7 focused files + `ime_internal.h`.
- [x] Split `tests/test_ime_logic.cpp` (1,554 lines) → 3 test files + `test_helpers.h`.
- [x] Add `include/mie/mie.h` C API (opaque handles, C-linkage) — see §10.
- [x] Add `src/mie_c_api.cpp` implementing the C API — **37 new tests; total 120/120 passing**.
- [x] Add `firmware/mie/README.md` for standalone project landing page.
- [x] `git subtree split --prefix=firmware/mie -b libmie-standalone` — branch created locally.
- [ ] Push `libmie-standalone` to `tengigabytes/libmie` *(deferred — awaiting repo creation)*.
- [ ] Replace `firmware/mie/` with submodule pointing to `tengigabytes/libmie`.

### Phase 2 — Hardware Integration (MokyaLora Rev A)

- [ ] `hal/rp2350/`: bridge PIO+DMA key state buffer through `keymap_matrix.h` → `keycode_t` → `mie::KeyEvent`.
- [ ] Boot loader: copy DAT + values from Flash to PSRAM; measure search latency.
- [ ] Display: render `font_glyphs.bin` via LVGL custom font driver on NHD 2.4″.
- [ ] UI: integrate candidate bar widget with Trie-Searcher output.

### Phase 3 — Optimisation & Extension

- [ ] Spatial + phonetic fuzzy correction.
- [ ] User-defined word list in LittleFS, merged into DAT at runtime.
- [ ] Additional language pack slots (Japanese kana, Latin-script prediction).

---

## 9. Standalone Repository Extraction

MIE is designed from the start as an extractable library.

**Extraction status:**

| Step | Status |
|------|--------|
| `include/mie/mie.h` C API + `src/mie_c_api.cpp` | ✅ done |
| `firmware/mie/README.md` | ✅ done |
| `git subtree split --prefix=firmware/mie -b libmie-standalone` | ✅ done (branch exists locally) |
| Create `tengigabytes/libmie` on GitHub | ⏳ deferred |
| Push `libmie-standalone:main` to `tengigabytes/libmie` | ⏳ deferred |
| `git submodule add` to replace `firmware/mie/` | ⏳ deferred (after repo push) |

**Extraction commands (when ready):**
```sh
git remote add libmie https://github.com/tengigabytes/libmie.git
git push libmie libmie-standalone:main
# then in MokyaLora repo:
git rm -r firmware/mie
git submodule add https://github.com/tengigabytes/libmie.git firmware/mie
git submodule update --init
```

The HAL interface contract and CMakeLists.txt structure ensure zero changes to core
library code during extraction.

---

## 10. Cross-Platform Targets & C API

MIE exposes a stable C API via `include/mie/mie.h` (opaque handles, C-linkage,
no C++ exceptions crossing the boundary). Implemented in `src/mie_c_api.cpp`; covered
by 37 dedicated tests in `tests/test_mie_c_api.cpp`.

### C API surface (`include/mie/mie.h`)

```c
/* ── Dictionary ──────────────────────────────────────────── */
mie_dict_t* mie_dict_open(const char* dat_path, const char* val_path);
mie_dict_t* mie_dict_open_memory(const uint8_t* dat_buf, size_t dat_size,
                                  const uint8_t* val_buf, size_t val_size);
void        mie_dict_close(mie_dict_t*);

/* ── Context ─────────────────────────────────────────────── */
mie_ctx_t*  mie_ctx_create(mie_dict_t* zh, mie_dict_t* en);  /* en may be NULL */
void        mie_ctx_destroy(mie_ctx_t*);
void        mie_set_commit_cb(mie_ctx_t*, void(*cb)(const char*, void*), void*);
int         mie_process_key(mie_ctx_t*, uint8_t keycode, int pressed);
void        mie_clear_input(mie_ctx_t*);

/* ── Display ─────────────────────────────────────────────── */
const char* mie_input_str(mie_ctx_t*);       /* raw phoneme/letter display */
const char* mie_compound_str(mie_ctx_t*);    /* [ph0ph1]ˉ compound display */
const char* mie_mode_indicator(mie_ctx_t*);  /* "中" / "EN" / "ABC" / "abc" / "ㄅ" */

/* ── Candidates (merged ZH+EN list) ─────────────────────── */
int         mie_candidate_count(mie_ctx_t*);
const char* mie_candidate_word(mie_ctx_t*, int idx);
int         mie_candidate_lang(mie_ctx_t*, int idx);  /* 0=ZH 1=EN -1=OOB */

/* ── Pagination (page size = 5) ──────────────────────────── */
int         mie_page_size(void);
int         mie_cand_page(mie_ctx_t*);
int         mie_cand_page_count(mie_ctx_t*);
int         mie_page_cand_count(mie_ctx_t*);
const char* mie_page_cand_word(mie_ctx_t*, int idx);
int         mie_page_cand_lang(mie_ctx_t*, int idx);
int         mie_page_sel(mie_ctx_t*);
```

### Platform wrappers

| Target | Wrapper | Dict location | Status |
|--------|---------|---------------|--------|
| MokyaLora (RP2350) | Direct C++ call to `ImeLogic` | Flash → PSRAM at boot (`mie_dict_open_memory`) | Phase 2 |
| Android IME service | JNI wrapper calling C API | APK assets → internal storage | Planned |
| Windows TSF text service | COM `ITextInputProcessor` → C API | `%APPDATA%\libmie\` | Planned |
