// SPDX-License-Identifier: MIT
// ime_keys.cpp — KeyEntry table for the 20 input keys and keycode lookups,
//                 plus position-counter classifier for the v4 dispatch path.

#include "ime_internal.h"
#include <mie/ime_logic.h>

namespace mie {

const KeyEntry kKeyTable[20] = {
    // Row 0 (digit primary / Bopomofo secondary / calculator tertiary)
    { MOKYA_KEY_1, {"ㄅ","ㄉ",nullptr}, {"1","2"}, {nullptr,nullptr,nullptr,nullptr} },
    { MOKYA_KEY_3, {"ˇ", "ˋ", nullptr}, {"3","4"}, {nullptr,nullptr,nullptr,nullptr} },
    { MOKYA_KEY_5, {"ㄓ","ˊ",nullptr}, {"5","6"}, {nullptr,nullptr,nullptr,nullptr} },
    { MOKYA_KEY_7, {"˙", "ㄚ",nullptr}, {"7","8"}, {nullptr,nullptr,nullptr,nullptr} },
    { MOKYA_KEY_9, {"ㄞ","ㄢ","ㄦ"},    {"9","0"}, {nullptr,nullptr,nullptr,nullptr} },

    // Row 1 (letters primary / Bopomofo secondary / calculator tertiary)
    { MOKYA_KEY_Q, {"ㄆ","ㄊ",nullptr}, {nullptr,nullptr}, {"q","w","Q","W"} },
    { MOKYA_KEY_E, {"ㄍ","ㄐ",nullptr}, {nullptr,nullptr}, {"e","r","E","R"} },
    { MOKYA_KEY_T, {"ㄔ","ㄗ",nullptr}, {nullptr,nullptr}, {"t","y","T","Y"} },
    { MOKYA_KEY_U, {"ㄧ","ㄛ",nullptr}, {nullptr,nullptr}, {"u","i","U","I"} },
    { MOKYA_KEY_O, {"ㄟ","ㄣ",nullptr}, {nullptr,nullptr}, {"o","p","O","P"} },

    // Row 2
    { MOKYA_KEY_A, {"ㄇ","ㄋ",nullptr}, {nullptr,nullptr}, {"a","s","A","S"} },
    { MOKYA_KEY_D, {"ㄎ","ㄑ",nullptr}, {nullptr,nullptr}, {"d","f","D","F"} },
    { MOKYA_KEY_G, {"ㄕ","ㄘ",nullptr}, {nullptr,nullptr}, {"g","h","G","H"} },
    { MOKYA_KEY_J, {"ㄨ","ㄜ",nullptr}, {nullptr,nullptr}, {"j","k","J","K"} },
    { MOKYA_KEY_L, {"ㄠ","ㄤ",nullptr}, {nullptr,nullptr}, {"l","L",nullptr,nullptr} },

    // Row 3
    { MOKYA_KEY_Z,         {"ㄈ","ㄌ",nullptr}, {nullptr,nullptr}, {"z","x","Z","X"} },
    { MOKYA_KEY_C,         {"ㄏ","ㄒ",nullptr}, {nullptr,nullptr}, {"c","v","C","V"} },
    { MOKYA_KEY_B,         {"ㄖ","ㄙ",nullptr}, {nullptr,nullptr}, {"b","n","B","N"} },
    { MOKYA_KEY_M,         {"ㄩ","ㄝ",nullptr}, {nullptr,nullptr}, {"m","M",nullptr,nullptr} },
    { MOKYA_KEY_BACKSLASH, {"ㄡ","ㄥ",nullptr}, {nullptr,nullptr}, {nullptr,nullptr,nullptr,nullptr} },
};

int keycode_to_input_slot(mokya_keycode_t kc) {
    for (int i = 0; i < 20; ++i)
        if (kKeyTable[i].keycode == kc) return i;
    return -1;
}

const KeyEntry* find_key_entry(mokya_keycode_t kc) {
    int slot = keycode_to_input_slot(kc);
    return (slot >= 0) ? &kKeyTable[slot] : nullptr;
}

// ── Position counter (H2 heuristic) ─────────────────────────────────────
// Classifies each phoneme key byte by its DOMINANT phonological role and
// counts syllable positions. The v4 dispatch maps position count to
// CompositionSearcher::search target_char_count.
//
// Key role per slot (by dominant phoneme; slots with mixed roles fall to
// their dominant interpretation):
//
//   Role: Initial   → starts a new syllable (or ends a prev bare-initial)
//         Medial    → glide vowel; merges into current syllable
//         Final     → vowel/coda; merges into current syllable
//         Tone      → ends current syllable
//         None      → unmappable byte
//
// SPACE (0x20) is the explicit tone-1 marker and is treated as Tone.
namespace {

enum class KeyRole : uint8_t { None = 0, Initial, Medial, Final, Tone };

// Indexed by slot 0..19. Mirrors the phoneme assignments in ime_keys.cpp's
// kKeyTable above and gen_dict.py's _BPMF_KEYMAP_RAW.
static const KeyRole kSlotRoles[20] = {
    KeyRole::Initial, // 0  ㄅㄉ
    KeyRole::Tone,    // 1  ˇ ˋ                (tone 3 / 4)
    KeyRole::Initial, // 2  ㄓ ˊ               (treat as initial; ˊ rare)
    KeyRole::Final,   // 3  ˙ ㄚ                (treat as final; ˙ rare)
    KeyRole::Final,   // 4  ㄞㄢㄦ
    KeyRole::Initial, // 5  ㄆㄊ
    KeyRole::Initial, // 6  ㄍㄐ
    KeyRole::Initial, // 7  ㄔㄗ
    KeyRole::Medial,  // 8  ㄧㄛ                 (ㄧ medial dominates)
    KeyRole::Final,   // 9  ㄟㄣ
    KeyRole::Initial, // 10 ㄇㄋ
    KeyRole::Initial, // 11 ㄎㄑ
    KeyRole::Initial, // 12 ㄕㄘ
    KeyRole::Medial,  // 13 ㄨㄜ                 (ㄨ medial dominates)
    KeyRole::Final,   // 14 ㄠㄤ
    KeyRole::Initial, // 15 ㄈㄌ
    KeyRole::Initial, // 16 ㄏㄒ
    KeyRole::Initial, // 17 ㄖㄙ
    KeyRole::Medial,  // 18 ㄩㄝ                 (ㄩ medial dominates)
    KeyRole::Final,   // 19 ㄡㄥ
};

static inline KeyRole classify_byte(uint8_t b) {
    if (b == 0x20) return KeyRole::Tone;  // SPACE = explicit tone-1 marker
    int slot = (int)b - 0x21;
    if (slot < 0 || slot >= 20) return KeyRole::None;
    return kSlotRoles[slot];
}

} // namespace

// Syllable state machine (H3). Tracks position WITHIN the current syllable
// so the counter can recognise natural boundaries even when the user omits
// the tone marker.
//
//   kNone   = outside any syllable (start, or after a Tone marker)
//   kAfterI = just consumed an Initial; expecting optional Medial/Final
//   kAfterM = just consumed a Medial; expecting optional Final
//   kAfterF = just consumed a Final (coda); syllable is complete, so any
//             next byte of any role starts a new syllable
//
// Transitions per role:
//   Initial:  always closes any open syllable and starts a new one.
//   Medial:   combines with a preceding Initial (state kAfterI); otherwise
//             (state kNone / kAfterM / kAfterF) starts a new syllable.
//   Final:    combines with kAfterI or kAfterM; otherwise starts new.
//   Tone:     terminates current syllable (state → kNone).
//
// This fixes e.g. ㄕㄨㄚ+ㄧㄚ (刷牙): the trailing ㄚ drives state to
// kAfterF, and the following ㄧ then correctly starts a new syllable.
// Also disambiguates ㄓ+ㄩ (retroflex + ü, phonologically invalid together
// so ㄩ must be a new syllable) after kAfterI — well, ㄩ after kAfterI of
// ㄓ is actually still merged here; the user then relies on the adjacent-
// bucket merge in run_search_v4 to catch the miscount.
enum class SyllState : uint8_t { kNone, kAfterI, kAfterM, kAfterF };

// Context-aware byte→role override. Some keypad slots share an Initial
// primary phoneme with a Tone secondary phoneme (slot 2 = ㄓ/ˊ, slot 3 =
// ˙/ㄚ). Pure byte-level classify_byte() defaults to Initial for slot 2,
// which causes `ㄆㄧㄥˊ` to be miscounted as 2 syllables (the trailing ˊ
// is read as ㄓ). Resolve by treating 0x23 as Tone when the current
// syllable is already partially built (kAfterI/M/F).
static inline KeyRole resolve_role(uint8_t b, SyllState st) {
    // Tone 2 (ˊ) can only apply to a syllable that already has a vowel
    // (kAfterM or kAfterF). After a bare Initial (kAfterI) the byte 0x23
    // must be ㄓ — otherwise 4-initial abbreviations like ㄆㄈㄔㄓ would
    // mis-count. After kNone / kAfterF boundaries it's also ㄓ starting
    // a new syllable, except when it directly closes a vowel-bearing one.
    if (b == 0x23 && (st == SyllState::kAfterM ||
                      st == SyllState::kAfterF)) {
        return KeyRole::Tone;
    }
    return classify_byte(b);
}

static inline bool advance_state(KeyRole role, SyllState& st, bool& starts_new) {
    starts_new = false;
    switch (role) {
    case KeyRole::Initial:
        starts_new = true;
        st = SyllState::kAfterI;
        return true;
    case KeyRole::Medial:
        if (st == SyllState::kAfterI) {
            st = SyllState::kAfterM;
        } else if (st == SyllState::kAfterM) {
            // Medial-slot byte (0x33) serves dual role: ㄩ (glide) primary
            // and ㄝ (nucleus) secondary — similarly slots 8 (ㄧ/ㄛ) and
            // 13 (ㄨ/ㄜ). A second Medial byte after a first is almost
            // always the nucleus of the same syllable (ㄩㄝ, ㄨㄛ, ㄧㄛ, ...),
            // not the start of a new one; treat as a Final role transition.
            st = SyllState::kAfterF;
        } else {
            // kNone or kAfterF: new vowel-initial syllable.
            starts_new = true;
            st = SyllState::kAfterM;
        }
        return true;
    case KeyRole::Final:
        if (st == SyllState::kAfterI || st == SyllState::kAfterM) {
            st = SyllState::kAfterF;
        } else {
            starts_new = true;
            st = SyllState::kAfterF;
        }
        return true;
    case KeyRole::Tone:
        st = SyllState::kNone;
        return true;
    case KeyRole::None:
    default:
        return false;
    }
}

int ImeLogic::count_positions(const char* seq, int len) {
    if (!seq || len <= 0) return 0;
    int count = 0;
    SyllState st = SyllState::kNone;
    for (int i = 0; i < len; ++i) {
        KeyRole role = resolve_role((uint8_t)seq[i], st);
        bool starts_new = false;
        advance_state(role, st, starts_new);
        if (starts_new) ++count;
    }
    return count;
}

int ImeLogic::first_n_positions_bytes(const char* seq, int len,
                                       int n_positions) {
    if (!seq || len <= 0 || n_positions <= 0) return 0;
    int count = 0;
    SyllState st = SyllState::kNone;
    for (int i = 0; i < len; ++i) {
        KeyRole role = resolve_role((uint8_t)seq[i], st);
        bool starts_new = false;
        advance_state(role, st, starts_new);

        if (starts_new && count == n_positions) {
            return i;
        }
        if (starts_new) ++count;

        if (role == KeyRole::Tone && count >= n_positions) {
            return i + 1;
        }
    }
    return len;
}

} // namespace mie
