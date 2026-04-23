// SPDX-License-Identifier: MIT
// MokyaInput Engine — LruCache implementation (Phase 1.6).

#include <mie/lru_cache.h>

#include <string.h>

namespace mie {

namespace {

constexpr uint8_t kHintAny = 0xFF;
constexpr uint8_t kPackedAny = 0x3;   // 2-bit sentinel for ANY

uint8_t hint_to_bits(uint8_t hint) {
    if (hint == kHintAny) return kPackedAny;
    return (uint8_t)(hint & 0x3);
}

uint8_t bits_to_hint(uint8_t bits) {
    if (bits == kPackedAny) return kHintAny;
    return bits;
}

bool hint_matches(uint8_t stored, uint8_t user) {
    if (user == kHintAny) return true;
    if (stored == kHintAny) return true;
    return stored == user;
}

const uint8_t kMagic[4] = { 'L', 'R', 'U', '1' };

} // namespace

uint8_t lru_pack_phoneme_hints(const uint8_t* hints, int n) {
    uint8_t packed = 0;
    for (int k = 0; k < kLruPackedPositions; ++k) {
        uint8_t h = (hints && k < n) ? hints[k] : kHintAny;
        packed |= (uint8_t)(hint_to_bits(h) << (k * 2));
    }
    return packed;
}

uint8_t lru_unpack_phoneme_hint(uint8_t pos_packed, int k) {
    if (k < 0 || k >= kLruPackedPositions) return kHintAny;
    uint8_t bits = (uint8_t)((pos_packed >> (k * 2)) & 0x3);
    return bits_to_hint(bits);
}

void LruCache::reset() {
    memset(entries_, 0, sizeof(entries_));
    count_ = 0;
}

int LruCache::find_exact(const uint8_t* kbytes, int klen,
                         uint8_t pos_packed, const char* utf8) const {
    int utf8_len = (int)strlen(utf8);
    for (int i = 0; i < count_; ++i) {
        const LruEntry& e = entries_[i];
        if (e.klen != klen) continue;
        if (e.pos_packed != pos_packed) continue;
        if (e.utf8_len != utf8_len) continue;
        if (memcmp(e.kbytes, kbytes, klen) != 0) continue;
        if (memcmp(e.utf8, utf8, utf8_len) != 0) continue;
        return i;
    }
    return -1;
}

int LruCache::find_evict_victim() const {
    // Minimum last_used_ms; ties broken by minimum use_count.
    int victim = 0;
    uint32_t v_ms = entries_[0].last_used_ms;
    uint16_t v_uc = entries_[0].use_count;
    for (int i = 1; i < count_; ++i) {
        const LruEntry& e = entries_[i];
        if (e.last_used_ms < v_ms ||
            (e.last_used_ms == v_ms && e.use_count < v_uc)) {
            victim = i;
            v_ms = e.last_used_ms;
            v_uc = e.use_count;
        }
    }
    return victim;
}

void LruCache::upsert(const uint8_t* kbytes, int klen, uint8_t pos_packed,
                      uint8_t tone, const char* utf8, uint32_t now_ms) {
    if (klen <= 0 || klen > 8 || !kbytes || !utf8) return;
    int utf8_len = (int)strlen(utf8);
    if (utf8_len <= 0 || utf8_len > (int)sizeof(((LruEntry*)0)->utf8)) return;

    int idx = find_exact(kbytes, klen, pos_packed, utf8);
    if (idx >= 0) {
        LruEntry& e = entries_[idx];
        e.last_used_ms = now_ms;
        if (e.use_count < UINT16_MAX) ++e.use_count;
        e.tone = tone;
        return;
    }

    int slot;
    if (count_ < kCap) {
        slot = count_++;
    } else {
        slot = find_evict_victim();
    }
    LruEntry& e = entries_[slot];
    memset(&e, 0, sizeof(e));
    memcpy(e.kbytes, kbytes, klen);
    e.klen = (uint8_t)klen;
    e.pos_packed = pos_packed;
    e.tone = tone;
    e.utf8_len = (uint8_t)utf8_len;
    memcpy(e.utf8, utf8, utf8_len);
    e.last_used_ms = now_ms;
    e.use_count = 1;
}

int LruCache::lookup(const uint8_t* user_keys, int user_n,
                     const uint8_t* user_phoneme_hints,
                     Candidate* out, int max_results) const {
    if (!user_keys || user_n <= 0 || !out || max_results <= 0) return 0;

    // Collect indices of matching entries. Capacity kCap is small so we
    // perform a selection sort over indices to avoid extra storage.
    int matches[kCap];
    int n_match = 0;

    for (int i = 0; i < count_; ++i) {
        const LruEntry& e = entries_[i];
        if (e.klen == 0 || e.klen > user_n) continue;
        if (e.klen != user_n && e.klen == 1) continue;  // no prefix on len-1
        if (memcmp(e.kbytes, user_keys, e.klen) != 0) continue;

        // Phoneme-hint filter.
        bool ok = true;
        for (int k = 0; k < e.klen; ++k) {
            uint8_t user_h = user_phoneme_hints ? user_phoneme_hints[k] : kHintAny;
            uint8_t stored_h = lru_unpack_phoneme_hint(e.pos_packed, k);
            if (!hint_matches(stored_h, user_h)) { ok = false; break; }
        }
        if (!ok) continue;

        matches[n_match++] = i;
    }

    // Sort matches by last_used_ms desc, then use_count desc.
    for (int i = 1; i < n_match; ++i) {
        int cur = matches[i];
        const LruEntry& ec = entries_[cur];
        int j = i - 1;
        while (j >= 0) {
            const LruEntry& ep = entries_[matches[j]];
            bool worse = ep.last_used_ms < ec.last_used_ms ||
                         (ep.last_used_ms == ec.last_used_ms &&
                          ep.use_count < ec.use_count);
            if (!worse) break;
            matches[j + 1] = matches[j];
            --j;
        }
        matches[j + 1] = cur;
    }

    // Emit into out[], deduping by utf8 string.
    int n_out = 0;
    for (int m = 0; m < n_match && n_out < max_results; ++m) {
        const LruEntry& e = entries_[matches[m]];
        bool dup = false;
        for (int j = 0; j < n_out; ++j) {
            if (strncmp(out[j].word, e.utf8, e.utf8_len) == 0 &&
                out[j].word[e.utf8_len] == '\0') {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        Candidate& c = out[n_out++];
        memset(&c, 0, sizeof(c));
        int copy = e.utf8_len;
        if (copy >= kCandidateMaxBytes) copy = kCandidateMaxBytes - 1;
        memcpy(c.word, e.utf8, copy);
        c.word[copy] = '\0';
        c.freq = UINT16_MAX;   // promote above dict candidates
        c.tone = e.tone;
    }
    return n_out;
}

int LruCache::serialize(uint8_t* buf, int cap) const {
    int need = serialized_size();
    if (!buf || cap < need) return -1;
    memcpy(buf, kMagic, 4);
    uint16_t ver = kSerialVersion;
    memcpy(buf + 4, &ver, 2);
    uint16_t cnt = (uint16_t)count_;
    memcpy(buf + 6, &cnt, 2);
    memcpy(buf + kHeaderSize, entries_, (size_t)count_ * sizeof(LruEntry));
    return need;
}

bool LruCache::deserialize(const uint8_t* buf, int len) {
    if (!buf || len < kHeaderSize) return false;
    if (memcmp(buf, kMagic, 4) != 0) return false;
    uint16_t ver = 0;
    memcpy(&ver, buf + 4, 2);
    if (ver != kSerialVersion) return false;
    uint16_t cnt = 0;
    memcpy(&cnt, buf + 6, 2);
    if (cnt > kCap) return false;
    int need = kHeaderSize + (int)cnt * (int)sizeof(LruEntry);
    if (len < need) return false;

    reset();
    memcpy(entries_, buf + kHeaderSize, (size_t)cnt * sizeof(LruEntry));
    count_ = cnt;

    // Clamp any out-of-range klen coming from a corrupted file.
    for (int i = 0; i < count_; ++i) {
        if (entries_[i].klen == 0 || entries_[i].klen > 8 ||
            entries_[i].utf8_len == 0 ||
            entries_[i].utf8_len > (uint8_t)sizeof(entries_[i].utf8)) {
            reset();
            return false;
        }
    }
    return true;
}

} // namespace mie
