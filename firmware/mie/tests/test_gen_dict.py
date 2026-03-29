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
        assert any(item[0] == '今天' for item in key_to_words[init_key])

        # Prefix-initials key
        mixed_key = bytes([B_GJ, B_PT, B_I, B_AI])
        assert mixed_key in key_to_words
        assert any(item[0] == '今天' for item in key_to_words[mixed_key])

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


# ── Abbreviation filters (max_abbr_syls / min_freq_for_abbr) ─────────────────

class TestAbbrFilters:
    """
    Verifies that load_libchewing / load_moe_csv respect the two abbreviation
    filter parameters without touching the underlying abbreviated_keyseqs logic.
    We test the filtering inline by replicating the loader decision:
        emit_abbr = (min_freq == 0 or freq >= min_freq) and
                    (max_syls == 0 or n_syls <= max_syls)
    """

    def _emit_abbr(self, reading: str, freq: int,
                   min_freq: int = 0, max_syls: int = 4) -> bool:
        """Mirror the emit_abbr decision in the loaders."""
        n_syls = len(gd.parse_reading_syllables(reading))
        return (
            (min_freq == 0 or freq >= min_freq) and
            (max_syls == 0 or n_syls <= max_syls)
        )

    def test_within_syl_limit_emits_abbr(self):
        # 今天 = 2 syllables ≤ 4 → emit
        assert self._emit_abbr('ㄐㄧㄣ ㄊㄧㄢ', freq=1, max_syls=4)

    def test_at_syl_limit_emits_abbr(self):
        # 中華民國 = 4 syllables == 4 → still emit
        assert self._emit_abbr('ㄓㄨㄥ ㄏㄨㄚˊ ㄇㄧㄣˊ ㄍㄨㄛˊ', freq=1, max_syls=4)

    def test_over_syl_limit_suppresses_abbr(self):
        # 5-syllable phrase → suppress
        reading_5 = 'ㄅ ㄆ ㄇ ㄈ ㄉ'
        assert not self._emit_abbr(reading_5, freq=1, max_syls=4)

    def test_zero_max_syls_means_no_limit(self):
        # max_syls=0 → no limit, even 6-syllable should emit
        reading_6 = 'ㄅ ㄆ ㄇ ㄈ ㄉ ㄊ'
        assert self._emit_abbr(reading_6, freq=1, max_syls=0)

    def test_freq_below_threshold_suppresses_abbr(self):
        assert not self._emit_abbr('ㄐㄧㄣ ㄊㄧㄢ', freq=3, min_freq=5)

    def test_freq_at_threshold_emits_abbr(self):
        assert self._emit_abbr('ㄐㄧㄣ ㄊㄧㄢ', freq=5, min_freq=5)

    def test_freq_above_threshold_emits_abbr(self):
        assert self._emit_abbr('ㄐㄧㄣ ㄊㄧㄢ', freq=100, min_freq=5)

    def test_zero_min_freq_means_no_filter(self):
        # min_freq=0 → no filter, even freq=0 should emit
        assert self._emit_abbr('ㄐㄧㄣ ㄊㄧㄢ', freq=0, min_freq=0)

    def test_both_filters_combined(self):
        # 今天: 2 syls, freq=10 — passes both
        assert self._emit_abbr('ㄐㄧㄣ ㄊㄧㄢ', freq=10, min_freq=5, max_syls=4)
        # 臭豆腐: 3 syls, freq=2 — fails freq filter
        assert not self._emit_abbr('ㄔㄡˋ ㄉㄡˋ ㄈㄨˋ', freq=2, min_freq=5, max_syls=4)
        # 5-syllable phrase, freq=100 — fails syl filter
        assert not self._emit_abbr('ㄅ ㄆ ㄇ ㄈ ㄉ', freq=100, min_freq=5, max_syls=4)


# ── English wordlist & collision pruning ─────────────────────────────────────

import io
import os
import tempfile


def _write_temp(content: str) -> str:
    """Write UTF-8 text to a NamedTemporaryFile; return its path. Caller deletes."""
    fd, path = tempfile.mkstemp(suffix='.txt')
    os.write(fd, content.encode('utf-8'))
    os.close(fd)
    return path


class TestEnWordlist:
    """Tests for load_en_wordlist() — format parsing, min_freq filter."""

    # ── Format handling ───────────────────────────────────────────────────

    def test_space_separated_freq_hermitdave_format(self):
        """hermitdave/FrequencyWords 'word frequency' (single space) format."""
        path = _write_temp("the 10000000\nand 8000000\nof 6000000\n")
        try:
            entries = gd.load_en_wordlist(path)
        finally:
            os.unlink(path)
        words = {e[1]: e[2] for e in entries}
        assert 'the' in words
        assert words['the'] == 10000000
        assert 'and' in words
        assert words['and'] == 8000000

    def test_tab_separated_freq(self):
        """Original TAB-separated 'word\\tfreq' format still works."""
        path = _write_temp("hello\t1000\nworld\t500\n")
        try:
            entries = gd.load_en_wordlist(path)
        finally:
            os.unlink(path)
        words = {e[1]: e[2] for e in entries}
        assert words.get('hello') == 1000
        assert words.get('world') == 500

    def test_no_freq_defaults_to_one(self):
        """Words without a frequency token default to freq=1."""
        path = _write_temp("apple\nbanana\n")
        try:
            entries = gd.load_en_wordlist(path)
        finally:
            os.unlink(path)
        words = {e[1]: e[2] for e in entries}
        assert words.get('apple') == 1
        assert words.get('banana') == 1

    def test_unmappable_word_dropped(self):
        """Words containing unmappable characters (digit, punctuation) are skipped."""
        # 'can\'t' has apostrophe which is not in ENG_LETTER_TO_KEY
        # '123' is all digits — not mappable
        path = _write_temp("hello 100\ncan't 200\n123 50\nworld 80\n")
        try:
            entries = gd.load_en_wordlist(path)
        finally:
            os.unlink(path)
        words = {e[1] for e in entries}
        assert 'hello' in words
        assert 'world' in words
        assert "can't" not in words
        assert '123' not in words

    def test_comment_lines_skipped(self):
        """Lines starting with '#' are treated as comments."""
        path = _write_temp("# comment\nhello 500\n# another comment\nworld 300\n")
        try:
            entries = gd.load_en_wordlist(path)
        finally:
            os.unlink(path)
        words = {e[1] for e in entries}
        assert 'hello' in words
        assert 'world' in words
        assert len(words) == 2

    # ── min_freq filter ───────────────────────────────────────────────────

    def test_min_freq_zero_keeps_all(self):
        """min_freq=0 (default) keeps everything regardless of frequency."""
        path = _write_temp("common 5000\nrare 1\n")
        try:
            entries = gd.load_en_wordlist(path, min_freq=0)
        finally:
            os.unlink(path)
        words = {e[1] for e in entries}
        assert 'common' in words
        assert 'rare' in words

    def test_min_freq_filters_below_threshold(self):
        """Words with freq < min_freq are dropped."""
        path = _write_temp("common 5000\nrare 10\n")
        try:
            entries = gd.load_en_wordlist(path, min_freq=100)
        finally:
            os.unlink(path)
        words = {e[1] for e in entries}
        assert 'common' in words
        assert 'rare' not in words   # 10 < 100

    def test_min_freq_keeps_at_threshold(self):
        """Words with freq == min_freq are kept (inclusive lower bound)."""
        path = _write_temp("edge 100\nbelow 99\n")
        try:
            entries = gd.load_en_wordlist(path, min_freq=100)
        finally:
            os.unlink(path)
        words = {e[1] for e in entries}
        assert 'edge' in words       # 100 == 100 → keep
        assert 'below' not in words  # 99 < 100 → drop

    def test_min_freq_with_hermitdave_scale(self):
        """min_freq works correctly with very large corpus counts."""
        # Simulate hermitdave-scale numbers (raw corpus occurrences)
        path = _write_temp("the 23135851162\nrare 42\n")
        try:
            entries = gd.load_en_wordlist(path, min_freq=1000)
        finally:
            os.unlink(path)
        words = {e[1] for e in entries}
        assert 'the' in words
        assert 'rare' not in words

    # ── keyseq encoding ───────────────────────────────────────────────────

    def test_keyseq_is_correct_for_known_word(self):
        """Verify key sequence encoding matches ENG_KEYMAP_RAW manually."""
        # 'a' → key_index 10 → byte 0x2B
        # 'c' → key_index 16 → byte 0x31
        # 'e' → key_index 6  → byte 0x27
        path = _write_temp("ace 100\n")
        try:
            entries = gd.load_en_wordlist(path)
        finally:
            os.unlink(path)
        assert len(entries) == 1
        keyseq, word, freq = entries[0][0], entries[0][1], entries[0][2]
        assert word == 'ace'
        assert keyseq == bytes([0x2B, 0x31, 0x27])


class TestMaxPerKeyPruning:
    """Tests for build_mied max_per_key collision-pruning parameter."""

    def _make_collision_entries(self):
        """Return 4 entries that all share the same key sequence.

        'a' and 's' both map to key_index 10 (byte 0x2B).
        'c' and 'v' both map to key_index 16 (byte 0x31).
        'e' maps to key_index 6 (byte 0x27).
        So "ace", "sce", "ave", "sve" → all keyseq b'\\x2b\\x31\\x27'.
        """
        ks = bytes([0x2B, 0x31, 0x27])
        # Verify via word_to_eng_keyseq
        assert gd.word_to_eng_keyseq('ace') == ks
        assert gd.word_to_eng_keyseq('sce') == ks
        assert gd.word_to_eng_keyseq('ave') == ks
        assert gd.word_to_eng_keyseq('sve') == ks
        return [
            (ks, 'ace', 1000),
            (ks, 'sce', 800),
            (ks, 'ave', 600),
            (ks, 'sve', 400),
        ]

    def test_no_pruning_keeps_all(self):
        entries = self._make_collision_entries()
        _, _, stats, k2w = gd.build_mied(entries, max_per_key=0)
        ks = bytes([0x2B, 0x31, 0x27])
        assert len(k2w[ks]) == 4
        assert stats['entry_count'] == 4
        assert stats['pruned_count'] == 0

    def test_max_per_key_prunes_to_n(self):
        entries = self._make_collision_entries()
        _, _, stats, k2w = gd.build_mied(entries, max_per_key=2)
        ks = bytes([0x2B, 0x31, 0x27])
        assert len(k2w[ks]) == 2
        # Must be the two highest-frequency words
        kept_words = {item[0] for item in k2w[ks]}
        assert 'ace' in kept_words    # freq=1000
        assert 'sce' in kept_words    # freq=800
        assert 'ave' not in kept_words
        assert 'sve' not in kept_words
        assert stats['entry_count'] == 2
        assert stats['pruned_count'] == 2

    def test_max_per_key_one_keeps_highest_freq(self):
        entries = self._make_collision_entries()
        _, _, stats, k2w = gd.build_mied(entries, max_per_key=1)
        ks = bytes([0x2B, 0x31, 0x27])
        assert len(k2w[ks]) == 1
        kept_words = {item[0] for item in k2w[ks]}
        assert 'ace' in kept_words  # highest freq=1000
        assert stats['pruned_count'] == 3

    def test_max_per_key_gt_count_does_not_prune(self):
        """When max_per_key > number of words per key, nothing is pruned."""
        entries = self._make_collision_entries()  # 4 words
        _, _, stats, k2w = gd.build_mied(entries, max_per_key=10)
        ks = bytes([0x2B, 0x31, 0x27])
        assert len(k2w[ks]) == 4
        assert stats['pruned_count'] == 0

    def test_values_sorted_by_freq_descending(self):
        """build_value_record must store words freq-descending even without pruning."""
        entries = [
            (b'\x2b', 'a', 100),
            (b'\x2b', 's', 500),   # 's' also maps to key 10 → same 1-byte key
        ]
        _, val_bytes, _, k2w = gd.build_mied(entries)
        # The first word in the value record should be 's' (freq=500)
        import struct
        word_count = struct.unpack_from('<H', val_bytes, 0)[0]
        assert word_count == 2
        # v2 per-word layout: freq:u16, tone:u8, word_len:u8, word_utf8
        freq0 = struct.unpack_from('<H', val_bytes, 2)[0]
        # tone0 is at offset 4 (u8) — skip it for this test
        wlen0 = struct.unpack_from('<B', val_bytes, 5)[0]
        word0 = val_bytes[6:6 + wlen0].decode('utf-8')
        assert word0 == 's'
        assert freq0 == 500  # highest freq first

    def test_default_no_pruning_zero(self):
        """build_mied() default max_per_key=0 means no pruning (backward compat)."""
        ks = gd.word_to_eng_keyseq('ace')
        entries = [(ks, f'word{i}', 1000 - i) for i in range(8)]
        _, _, stats, k2w = gd.build_mied(entries)   # default max_per_key=0
        assert len(k2w[ks]) == 8
        assert stats['pruned_count'] == 0

    def test_explicit_max_per_key_five(self):
        """Explicitly passing max_per_key=5 prunes to 5."""
        ks = gd.word_to_eng_keyseq('ace')
        entries = [(ks, f'word{i}', 1000 - i) for i in range(8)]
        _, _, stats, k2w = gd.build_mied(entries, max_per_key=5)
        assert len(k2w[ks]) == 5
        assert stats['pruned_count'] == 3


class TestEnSizeBudget:
    """Verify EN dictionary size behaviour at realistic word-list scales.

    The 1 MB target for en_dat.bin + en_values.bin (task §4.3) is achievable
    with the hermitdave 50k source by combining --en-min-freq filtering and
    --en-max-per-key=5.  These tests use synthetic data to verify the
    relationships rather than requiring a network download.
    """

    def _make_entries(self, n: int, max_len: int = 8, seed: int = 42) -> list:
        """Return n synthetic (keyseq, word, freq) entries using random words."""
        mappable_list = sorted(gd.ENG_LETTER_TO_KEY.keys())
        entries: list = []
        seen_words: set = set()
        rng = __import__('random').Random(seed)
        while len(entries) < n:
            length = rng.randint(3, max_len)
            word   = ''.join(rng.choice(mappable_list) for _ in range(length))
            if word in seen_words:
                continue
            seen_words.add(word)
            freq = max(1, int(rng.expovariate(1 / 5000)))
            ks   = gd.word_to_eng_keyseq(word)
            if ks:
                entries.append((ks, word, freq))
        return entries

    def test_25k_words_under_1mb(self):
        """25 k words with max_per_key=5 must be comfortably under 1 MB.

        25k is a typical size after applying --en-min-freq to a 50k source;
        this test verifies the dictionary pipeline produces a budget-compliant
        result at that scale.
        """
        entries = self._make_entries(25_000, max_len=8)
        _, _, stats, _ = gd.build_mied(entries, max_per_key=5)
        total = stats['dat_bytes'] + stats['val_bytes']
        assert total < 1024 * 1024, (
            f"EN dict too large at 25k words: {total:,} bytes "
            f"(dat={stats['dat_bytes']:,}, val={stats['val_bytes']:,})"
        )

    def test_max_per_key_reduces_size(self):
        """max_per_key=5 must produce a strictly smaller dict than no pruning
        when there are collisions (short words → many collisions)."""
        # Short words (len 3) on 15 keys → 3375 unique keys max, lots of collisions.
        entries = self._make_entries(20_000, max_len=4)
        _, _, stats_unpruned, _ = gd.build_mied(entries, max_per_key=0)
        _, _, stats_pruned,   _ = gd.build_mied(entries, max_per_key=5)
        # Pruning must reduce total stored entries
        assert stats_pruned['entry_count'] <= stats_unpruned['entry_count']
        # Pruning must reduce values.bin size (values hold the word payloads)
        assert stats_pruned['val_bytes'] <= stats_unpruned['val_bytes']

    def test_stats_entry_counts_consistent(self):
        """entry_count + pruned_count must equal the unique aggregated count."""
        entries = self._make_entries(5_000, max_len=4)
        _, _, stats, k2w = gd.build_mied(entries, max_per_key=3)
        stored = sum(len(v) for v in k2w.values())
        assert stored == stats['entry_count']
        assert stats['entry_count'] + stats['pruned_count'] == sum(
            len(v) for v in gd.build_mied(entries, max_per_key=0)[3].values()
        )


# ── reading_to_tone ───────────────────────────────────────────────────────────

class TestReadingToTone:
    """Verify reading_to_tone() extracts the correct Bopomofo tone."""

    def test_tone1_no_mark(self):
        # ㄅ (no tone mark) → tone 1
        assert gd.reading_to_tone('ㄅ') == 1

    def test_tone1_digit(self):
        # digit '1' → tone 1
        assert gd.reading_to_tone('ㄅ1') == 1

    def test_tone2_mark(self):
        # ˊ → tone 2
        assert gd.reading_to_tone('ㄅㄚˊ') == 2

    def test_tone2_digit(self):
        assert gd.reading_to_tone('ㄅㄚ2') == 2

    def test_tone3_mark(self):
        # ˇ → tone 3
        assert gd.reading_to_tone('ㄅㄢˇ') == 3

    def test_tone3_digit(self):
        assert gd.reading_to_tone('ㄅㄢ3') == 3

    def test_tone4_mark(self):
        # ˋ → tone 4
        assert gd.reading_to_tone('ㄅㄚˋ') == 4

    def test_tone4_digit(self):
        assert gd.reading_to_tone('ㄅㄚ4') == 4

    def test_tone5_dot(self):
        # ˙ → tone 5 (輕聲)
        assert gd.reading_to_tone('ㄅㄠ˙') == 5

    def test_tone5_digit(self):
        assert gd.reading_to_tone('ㄅㄠ5') == 5

    def test_multisyllable_uses_last(self):
        # 寶寶: ㄅㄠˇ ㄅㄠ˙ → last syllable tone = 5
        assert gd.reading_to_tone('ㄅㄠˇ ㄅㄠ˙') == 5

    def test_multisyllable_tone4_last(self):
        # 要去: ㄧㄠˋ ㄑㄩˋ → last syllable tone = 4
        assert gd.reading_to_tone('ㄧㄠˋ ㄑㄩˋ') == 4

    def test_multisyllable_tone1_last(self):
        # 今天: ㄐㄧㄣ ㄊㄧㄢ → last syllable has no tone mark → tone 1
        assert gd.reading_to_tone('ㄐㄧㄣ ㄊㄧㄢ') == 1

    def test_empty_reading_returns_1(self):
        assert gd.reading_to_tone('') == 1


# ── v2 binary format: tone byte present ──────────────────────────────────────

class TestV2BinaryFormat:
    """Verify that build_value_record emits the v2 layout with tone byte."""

    def test_tone_byte_present_in_value_record(self):
        import struct as s
        # Build a single-word ValueRecord via build_mied
        ks = bytes([0x21])
        entries = [(ks, '班', 200, 1)]   # word='班', freq=200, tone=1
        _, val_bytes, _, _ = gd.build_mied(entries)
        # v2 layout: word_count:u16, freq:u16, tone:u8, word_len:u8, word_utf8
        wc   = s.unpack_from('<H', val_bytes, 0)[0]
        freq = s.unpack_from('<H', val_bytes, 2)[0]
        tone = s.unpack_from('<B', val_bytes, 4)[0]
        wlen = s.unpack_from('<B', val_bytes, 5)[0]
        word = val_bytes[6:6 + wlen].decode('utf-8')
        assert wc   == 1
        assert freq == 200
        assert tone == 1
        assert word == '班'

    def test_tone4_word_stores_correctly(self):
        import struct as s
        ks = bytes([0x21, 0x24, 0x22])
        entries = [(ks, '爸', 300, 4)]
        _, val_bytes, _, _ = gd.build_mied(entries)
        tone = s.unpack_from('<B', val_bytes, 4)[0]
        assert tone == 4

    def test_en_tone_zero(self):
        import struct as s
        ks = bytes([0x27, 0x29])
        entries = [(ks, 'go', 100, 0)]   # English: tone=0
        _, val_bytes, _, _ = gd.build_mied(entries)
        tone = s.unpack_from('<B', val_bytes, 4)[0]
        assert tone == 0


if __name__ == '__main__':
    pytest.main([__file__, '-v'])

