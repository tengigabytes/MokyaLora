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
│       ├── trie_searcher.h     # (future)
│       └── ime_logic.h         # (future)
├── src/
│   ├── trie_searcher.cpp       # DAT search implementation (future)
│   └── ime_logic.cpp           # De-ambiguation & mode FSM (future)
├── hal/
│   ├── hal_port.h              # Abstract IHalPort interface (KeyEvent)
│   ├── rp2350/                 # RP2350 PIO scan → KeyEvent adapter (future)
│   └── pc/                     # PC stdin/keyboard stub for unit tests (future)
├── tools/
│   ├── gen_font.py             # Unifont → font_glyphs.bin + font_index.bin
│   └── gen_dict.py             # MoE dict → dict_dat.bin + dict_values.bin
├── data/                       # Generated binary assets — NOT committed to git
│   ├── font_glyphs.bin
│   ├── font_index.bin
│   ├── dict_dat.bin
│   ├── dict_values.bin
│   └── dict_meta.json
└── tests/                      # C++ unit tests, host-only build (future)
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
- `datrie` Python package for Double-Array Trie construction

**Output:**
- `dict_dat.bin` — DAT base[] and check[] arrays
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

| Mode                  | Key             | Description                                              |
|-----------------------|-----------------|----------------------------------------------------------|
| Bopomofo Auto         | consonant-first | Initial consonant prediction — type `ㄐ ㄊ` → predict 「今天」 |
| English Auto          | QWERTY          | Word prediction from English vocabulary                  |
| Alphanumeric Manual   | multi-tap       | Traditional T9-style; for passwords and exact input      |
| Calculator            | numeric         | Full calculator UI; `OK` = `=`; supports floats, brackets|

Mode switching: `MODE` key cycles through the four modes; status bar icon updates.

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
- PC stub (`hal/pc/`): translates stdin or a test script into `KeyEvent` for unit tests.

---

## 8. Development Roadmap

### Phase 1 — Environment Setup & PC Validation

- [ ] Write `gen_font.py` and verify output against a reference glyph set.
- [ ] Write `gen_dict.py`, validate DAT search correctness on PC.
- [ ] Implement `Trie-Searcher` in C++; unit-test with Google Test on PC.
- [ ] Implement `IME-Logic` Bopomofo mode de-ambiguation; test with simulated key sequences.

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
