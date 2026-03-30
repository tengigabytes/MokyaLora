# libmie — MokyaInput Engine

Portable, embeddable input method engine for Traditional Chinese (Bopomofo)
and English T9 prediction.  Written in C++11 with a stable C API for
cross-platform integration.

Originally developed for the **MokyaLora** open-hardware Meshtastic feature
phone (RP2350B), but deliberately kept self-contained so it can be embedded
in any project — Android IME services, Windows TSF text services, or any
bare-metal firmware.

---

## Features

- **5 input modes** — SmartZh (Bopomofo prediction), SmartEn (English T9),
  DirectUpper / DirectLower (multi-tap letters), DirectBopomofo (single
  Bopomofo phoneme cycling)
- **Tone-aware candidate ranking** — 4-tier sort (single/multi × tone-match);
  SPACE as first-tone marker; strict filtering discards off-tone candidates
- **Abbreviated input** — initial-consonant-only sequences expand to full words
- **Greedy prefix matching** — longest-match commit preserves trailing keystrokes
- **v2 MIED binary dictionary** — compact sorted-key format with per-word tone
  byte; v1 dicts accepted as a fallback
- **C API** (`include/mie/mie.h`) — opaque handles, C linkage, zero C++
  exposure; suitable for JNI and COM wrappers
- **HAL abstraction** — `IHalPort` interface; PC (termios) and RP2350 (PIO)
  adapters provided; add your own platform with ~30 lines
- **No OS / SDK dependency** — builds as a static library with plain CMake;
  optional Pico SDK integration for RP2350 targets

---

## Repository layout

```
libmie/
├── include/mie/
│   ├── mie.h            — stable C API (opaque handles)
│   ├── ime_logic.h      — C++ API (direct class access)
│   ├── trie_searcher.h  — dictionary search
│   └── hal_port.h       — HAL interface
├── src/
│   ├── mie_c_api.cpp    — C API implementation
│   ├── ime_logic.cpp    — process_key dispatcher
│   ├── ime_keys.cpp     — phoneme/symbol tables
│   ├── ime_search.cpp   — greedy prefix search + tone ranking
│   ├── ime_display.cpp  — compound display string
│   ├── ime_commit.cpp   — commit / partial commit
│   ├── ime_smart.cpp    — SmartZh / SmartEn handler
│   ├── ime_direct.cpp   — Direct mode + symbol handler
│   └── trie_searcher.cpp
├── hal/
│   ├── pc/              — termios stdin adapter (host/PC)
│   └── rp2350/          — RP2350 PIO adapter (planned)
├── tools/
│   ├── gen_dict.py      — compile tsi.csv + word list → MIED v2 binary
│   ├── gen_font.py      — compile GNU Unifont → 16 × 16 bitmap tiles
│   ├── fetch_data.py    — download data sources (tsi.csv, word list)
│   └── mie_repl.cpp     — interactive REPL for testing on PC
├── tests/               — 120 GoogleTest cases
├── data/                — generated .bin assets (gitignored; run gen_dict.py)
└── CMakeLists.txt
```

---

## Build

### Requirements

- CMake ≥ 3.20
- C++11 compiler (GCC, Clang, MSVC 2019+)
- Python 3.9+ (data pipeline only — not needed to build the library)

### Linux / macOS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### Windows (VS Build Tools 2019)

```sh
# From a VS Developer Command Prompt; project must be on a local drive
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release --parallel
```

### Run tests

```sh
ctest --test-dir build --output-on-failure
# Expected: 120/120 tests passed
```

### Interactive REPL (PC)

```sh
./build/mie_repl          # Linux / macOS
build\Debug\mie_repl.exe  # Windows
```

---

## Dictionary generation

```sh
# Download data sources (requires internet, run once)
cmake --build build --target mie_fetch_data

# Build dictionary + font assets
cmake -S . -B build \
  -DMIE_DATA_SOURCES_DIR=/path/to/data_sources \
  -DMIE_UNIFONT_OTF=/path/to/unifont-17.0.04.otf
cmake --build build --target mie_data_sm   # small — for RP2350 Flash
cmake --build build --target mie_data_lg   # large — for PC testing
```

Outputs written to `data/`:

| File | Contents |
|------|----------|
| `dict_dat.bin` | Chinese trie index (MIED v2) |
| `dict_values.bin` | Chinese candidate words + tone bytes |
| `en_dat.bin` | English trie index |
| `en_values.bin` | English candidates |
| `mie_unifont_sm_16.bin` | 16 × 16 font tiles (charlist-subset) |

---

## C API usage

```c
#include <mie/mie.h>

// 1 — open dictionaries
mie_dict_t* zh = mie_dict_open("data/dict_dat.bin", "data/dict_values.bin");
mie_dict_t* en = mie_dict_open("data/en_dat.bin",   "data/en_values.bin");

// 2 — create context
mie_ctx_t* ctx = mie_ctx_create(zh, en);

// 3 — register commit callback
mie_set_commit_cb(ctx, [](const char* text, void*) {
    printf("commit: %s\n", text);
}, NULL);

// 4 — feed key events (row 0-5, col 0-5, pressed 1=down 0=up)
mie_process_key(ctx, 0, 0, 1);   // key press
// ... redraw UI using mie_input_str / mie_compound_str / mie_candidate_word

// 5 — tear down
mie_ctx_destroy(ctx);
mie_dict_close(en);
mie_dict_close(zh);
```

For embedded targets with dictionary data in PSRAM / Flash:

```c
mie_dict_t* zh = mie_dict_open_memory(dat_ptr, dat_size, val_ptr, val_size);
```

---

## Platform integration

| Target | Approach |
|--------|----------|
| **MokyaLora RP2350B** | Link `libmie` into Core 1 firmware; use RP2350 HAL adapter; dict files in PSRAM |
| **Android IME service** | JNI wrapper calling C API; dict files deployed as app assets |
| **Windows TSF** | COM `ITextInputProcessor` thin wrapper over C API; dict files in `%APPDATA%\libmie\` |

---

## Key layout (6 × 6 matrix)

```
Row 0:  ㄅ/ㄉ   ㄆ/ㄊ   ㄇ/ㄋ   ㄈ/ㄌ   ㄞ/ㄢ   ← (row 0, col 0–4; col 5 = UP)
Row 1:  ㄍ/ㄐ   ㄎ/ㄑ   ㄏ/ㄒ   ㄧ/ㄛ   ㄟ/ㄣ   → (RIGHT)
Row 2:  ㄓ/ㄗ   ㄔ/ㄘ   ㄕ/ㄙ   ㄨ/ㄜ   ㄩ/ㄝ   BACK
Row 3:  ㄡ      ㄤ      ㄥ      ㄦ      ↑       ↓
Row 4:  MODE    TAB     SPACE   ，SYM  。.？   (empty)
Row 5:  (empty) (empty) (empty) (empty) OK      (empty)
```

---

## License

MIT — see [LICENSE](LICENSE).

MokyaInput Engine has no dependency on Meshtastic or any GPL-licensed code.
It is safe to embed in proprietary firmware or commercial applications.
