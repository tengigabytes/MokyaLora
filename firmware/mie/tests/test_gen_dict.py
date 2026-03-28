# test_gen_dict.py — Unit tests for gen_dict.py helper functions
# SPDX-License-Identifier: MIT
#
# Run with:  python -m pytest firmware/mie/tests/test_gen_dict.py  (from repo root)
# or:        python firmware/mie/tests/test_gen_dict.py

import sys
import os
import pytest

# Make gen_dict importable regardless of working directory.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
import gen_dict as gd

# ── Key reference ─────────────────────────────────────────────────────────────
# Physical key positions and their primary/secondary Bopomofo symbols:
#   (0,0) ㄅ/ㄉ  key_index=0  byte=0x21
#   (0,1) ˇ/ˋ   key_index=1  byte=0x22
#   (0,2) ㄓ/ˊ  key_index=2  byte=0x23
#   (0,3) ˙/ㄚ  key_index=3  byte=0x24
#   (0,4) ㄞ/ㄢ key_index=4  byte=0x25
#   (1,0) ㄆ/ㄊ  key_index=5  byte=0x26
#   (1,1) ㄍ/ㄐ  key_index=6  byte=0x27
#   (1,2) ㄔ/ㄗ  key_index=7  byte=0x28
#   (1,3) ㄧ/ㄛ  key_index=8  byte=0x29
#   (1,4) ㄟ/ㄣ  key_index=9  byte=0x2A
#   (2,0) ㄇ/ㄋ  key_index=10 byte=0x2B
#   (2,1) ㄎ/ㄑ  key_index=11 byte=0x2C
#   (2,2) ㄕ/ㄘ  key_index=12 byte=0x2D
#   (2,3) ㄨ/ㄜ  key_index=13 byte=0x2E
#   (2,4) ㄠ/ㄤ  key_index=14 byte=0x2F
#   (3,0) ㄈ/ㄌ  key_index=15 byte=0x30
#   (3,1) ㄏ/ㄒ  key_index=16 byte=0x31
#   (3,2) ㄖ/ㄙ  key_index=17 byte=0x32
#   (3,3) ㄩ/ㄝ  key_index=18 byte=0x33
#   (3,4) ㄡ/ㄥ  key_index=19 byte=0x34

B_BA   = 0x21  # ㄅ/ㄉ
B_TONE3= 0x22  # ˇ/ˋ
B_TONE2= 0x23  # ㄓ/ˊ (ˊ is here)
B_DOT  = 0x24  # ˙/ㄚ
B_AI   = 0x25  # ㄞ/ㄢ
B_PT   = 0x26  # ㄆ/ㄊ
B_GJ   = 0x27  # ㄍ/ㄐ
B_CZ   = 0x28  # ㄔ/ㄗ
B_I    = 0x29  # ㄧ/ㄛ
B_EIN  = 0x2A  # ㄟ/ㄣ
B_MN   = 0x2B  # ㄇ/ㄋ
B_KQ   = 0x2C  # ㄎ/ㄑ
B_SHC  = 0x2D  # ㄕ/ㄘ
B_OU   = 0x2E  # ㄨ/ㄜ
B_AO   = 0x2F  # ㄠ/ㄤ
B_FL   = 0x30  # ㄈ/ㄌ
B_HX   = 0x31  # ㄏ/ㄒ
B_RS   = 0x32  # ㄖ/ㄙ
B_YY   = 0x33  # ㄩ/ㄝ
B_OU2  = 0x34  # ㄡ/ㄥ


# ── parse_reading_syllables ───────────────────────────────────────────────────

class TestParseReadingSyllables:
    def test_single_syllable(self):
        # ㄅ alone → one syllable, one phoneme
        result = gd.parse_reading_syllables('ㄅ')
        assert result == [['ㄅ']]

    def test_two_syllables_space_separated(self):
        # 今天: ㄐㄧㄣ ㄊㄧㄢ
        result = gd.parse_reading_syllables('ㄐㄧㄣ ㄊㄧㄢ')
        assert result == [['ㄐ', 'ㄧ', 'ㄣ'], ['ㄊ', 'ㄧ', 'ㄢ']]

    def test_three_syllables(self):
        # 臭豆腐: ㄔㄡˋ ㄉㄡˋ ㄈㄨˋ
        result = gd.parse_reading_syllables('ㄔㄡˋ ㄉㄡˋ ㄈㄨˋ')
        assert result == [
            ['ㄔ', 'ㄡ', 'ˋ'],
            ['ㄉ', 'ㄡ', 'ˋ'],
            ['ㄈ', 'ㄨ', 'ˋ'],
        ]

    def test_tone1_dropped(self):
        # Tone-1 syllable ㄅ (no tone marker) → ㄅ kept; tone digit '1' → skipped
        result = gd.parse_reading_syllables('ㄅ1')
        # '1' is tone-1 (silence) → parse_syllable drops it
        assert result == [['ㄅ']]

    def test_empty_reading(self):
        result = gd.parse_reading_syllables('')
        assert result == []

    def test_digit_tones_parsed(self):
        # Digit-tone form: ㄐㄧㄣ3 ㄊㄧㄢ1
        result = gd.parse_reading_syllables('ㄐㄧㄣ3 ㄊㄧㄢ1')
        assert result[0] == ['ㄐ', 'ㄧ', 'ㄣ', 'ˇ']
        assert result[1] == ['ㄊ', 'ㄧ', 'ㄢ']   # tone-1 dropped


# ── abbreviated_keyseqs ───────────────────────────────────────────────────────

class TestAbbreviatedKeyseqs:

    # Helper
    def full_key(self, reading):
        return gd.phonemes_to_keyseq(gd.parse_reading(reading))

    def test_single_syllable_ba(self):
        # 巴 = ㄅㄚ, full=[0x21,0x24], initial=[0x21]
        full = self.full_key('ㄅㄚ')
        abbrs = gd.abbreviated_keyseqs('ㄅㄚ', full)
        assert bytes([B_BA]) in abbrs
        assert full not in abbrs  # full must not be re-emitted

    def test_single_syllable_initial_equals_full(self):
        # A 1-phoneme syllable: full == initial, so no abbreviated variant
        full = self.full_key('ㄅ')
        abbrs = gd.abbreviated_keyseqs('ㄅ', full)
        assert abbrs == []

    def test_two_syllable_jintian(self):
        # 今天 = ㄐㄧㄣ ㄊㄧㄢ
        # full  = [ㄐ,ㄧ,ㄣ,ㄊ,ㄧ,ㄢ] = [0x27,0x29,0x2A,0x26,0x29,0x25]
        # init  = [ㄐ,ㄊ]              = [0x27,0x26]
        # mixed = [ㄐ,ㄊ,ㄧ,ㄢ]        = [0x27,0x26,0x29,0x25]
        full = self.full_key('ㄐㄧㄣ ㄊㄧㄢ')
        abbrs = gd.abbreviated_keyseqs('ㄐㄧㄣ ㄊㄧㄢ', full)
        expected_init  = bytes([B_GJ, B_PT])
        expected_mixed = bytes([B_GJ, B_PT, B_I, B_AI])
        assert expected_init  in abbrs
        assert expected_mixed in abbrs
        assert full not in abbrs
        assert len(abbrs) == 8  # cartesian product: 3 choices × 3 choices − 1 (full) = 8

    def test_two_syllable_yaoqu(self):
        # 要去 = ㄧㄠˋ ㄑㄩˋ
        # full  = [ㄧ,ㄠ,ˋ,ㄑ,ㄩ,ˋ] = [0x29,0x2F,0x22,0x2C,0x33,0x22]
        # init  = [ㄧ,ㄑ]              = [0x29,0x2C]
        # mixed = [ㄧ,ㄑ,ㄩ,ˋ]         = [0x29,0x2C,0x33,0x22]
        full = self.full_key('ㄧㄠˋ ㄑㄩˋ')
        abbrs = gd.abbreviated_keyseqs('ㄧㄠˋ ㄑㄩˋ', full)
        expected_init  = bytes([B_I, B_KQ])
        expected_mixed = bytes([B_I, B_KQ, B_YY, B_TONE3])
        assert expected_init  in abbrs
        assert expected_mixed in abbrs

    def test_three_syllable_choudoufu(self):
        # 臭豆腐 = ㄔㄡˋ ㄉㄡˋ ㄈㄨˋ
        # full  = [ㄔ,ㄡ,ˋ,ㄉ,ㄡ,ˋ,ㄈ,ㄨ,ˋ] = [0x28,0x34,0x22,0x21,0x34,0x22,0x30,0x2E,0x22]
        # init  = [ㄔ,ㄉ,ㄈ]                  = [0x28,0x21,0x30]
        # mixed = [ㄔ,ㄉ,ㄈ,ㄨ,ˋ]             = [0x28,0x21,0x30,0x2E,0x22]
        full = self.full_key('ㄔㄡˋ ㄉㄡˋ ㄈㄨˋ')
        abbrs = gd.abbreviated_keyseqs('ㄔㄡˋ ㄉㄡˋ ㄈㄨˋ', full)
        expected_init  = bytes([B_CZ, B_BA, B_FL])
        expected_mixed = bytes([B_CZ, B_BA, B_FL, B_OU, B_TONE3])
        assert expected_init  in abbrs
        assert expected_mixed in abbrs

    def test_no_duplicate_variants(self):
        # If init == mixed (cannot happen for n>=2 normally, but guard test)
        # Build a contrived case where prefix-initials == all-initials by
        # making the last syllable have only 1 phoneme (no medial/final/tone).
        # E.g. "ㄐ ㄊ" — both syllables are 1-phoneme initials
        full = self.full_key('ㄐ ㄊ')
        abbrs = gd.abbreviated_keyseqs('ㄐ ㄊ', full)
        # all-initials = [0x27, 0x26], mixed = [0x27, 0x26] (same as full) → 0 variants
        # (both collapsed into the full key which is already excluded)
        assert len(abbrs) == 0  # all variants equal full, nothing to emit
        assert len(abbrs) == len(set(abbrs))  # uniqueness

    def test_full_key_never_in_abbrs(self):
        # full_keyseq must never appear in the abbreviated list
        for reading in ['ㄅ', 'ㄅㄚ', 'ㄐㄧㄣ ㄊㄧㄢ', 'ㄔㄡˋ ㄉㄡˋ ㄈㄨˋ']:
            full = gd.phonemes_to_keyseq(gd.parse_reading(reading))
            abbrs = gd.abbreviated_keyseqs(reading, full)
            assert full not in abbrs, f"full key leaked into abbrs for '{reading}'"


# ── Smoke-test: round-trip through build_mied ─────────────────────────────────

class TestAbbreviatedEntriesInMied:
    """
    Verifies that abbreviated entries survive the build_mied pipeline and
    can be found by the TrieSearcher.
    """

    def _load_trie(self, entries):
        """Build MIED bytes and load into a TrieSearcher (C extension via ctypes
        is not available in the Python test; instead check key_to_words directly)."""
        dat, val, stats, key_to_words = gd.build_mied(entries)
        return key_to_words, stats

    def test_jintian_all_initials_in_key_to_words(self):
        # 今天: full key, all-initials key, and prefix-initials key should all
        # have 今天 in their value lists after build_mied aggregation.
        reading = 'ㄐㄧㄣ ㄊㄧㄢ'
        full_key = gd.phonemes_to_keyseq(gd.parse_reading(reading))
        entries = [(full_key, '今天', 1)]
        for abbr in gd.abbreviated_keyseqs(reading, full_key):
            entries.append((abbr, '今天', 1))

        key_to_words, stats = self._load_trie(entries)

        # All-initials key
        init_key = bytes([B_GJ, B_PT])
        assert init_key in key_to_words
        assert any(w == '今天' for w, _ in key_to_words[init_key])

        # Prefix-initials key
        mixed_key = bytes([B_GJ, B_PT, B_I, B_AI])
        assert mixed_key in key_to_words
        assert any(w == '今天' for w, _ in key_to_words[mixed_key])

        # Full key also present
        assert full_key in key_to_words

    def test_entry_count_increases_for_multisyllable(self):
        # A 2-syllable word produces 3 keys (full + 2 abbreviated).
        reading = 'ㄐㄧㄣ ㄊㄧㄢ'
        full_key = gd.phonemes_to_keyseq(gd.parse_reading(reading))
        abbrs = gd.abbreviated_keyseqs(reading, full_key)
        assert len(abbrs) == 8  # cartesian product: 3×3 − 1 (full excluded) = 8

    def test_entry_count_single_syllable_with_medial(self):
        # 巴(ㄅㄚ): full=[0x21,0x24], initial=[0x21] → 1 abbreviated variant
        reading = 'ㄅㄚ'
        full_key = gd.phonemes_to_keyseq(gd.parse_reading(reading))
        abbrs = gd.abbreviated_keyseqs(reading, full_key)
        assert len(abbrs) == 1

    def test_single_phoneme_syllable_no_variant(self):
        # 不(ㄅ): full=[0x21], initial=[0x21] → identical, no abbreviated variant
        reading = 'ㄅ'
        full_key = gd.phonemes_to_keyseq(gd.parse_reading(reading))
        abbrs = gd.abbreviated_keyseqs(reading, full_key)
        assert len(abbrs) == 0


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
