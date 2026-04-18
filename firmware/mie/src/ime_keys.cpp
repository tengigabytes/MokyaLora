// SPDX-License-Identifier: MIT
// ime_keys.cpp — KeyEntry table for the 20 input keys and keycode lookups.

#include "ime_internal.h"

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

} // namespace mie
