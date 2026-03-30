// test_mie_c_api.cpp — Unit tests for the MIE C API (mie.h / mie_c_api.cpp)
// SPDX-License-Identifier: MIT
//
// Uses the same synthetic in-memory dict helpers as the C++ tests.
// Key layout reference (row, col):
//   (0,0) ㄅ/ㄉ   seq_byte=0x21
//   (4,0) MODE    (5,4) OK    (2,5) BACK

#include <gtest/gtest.h>
#include <mie/mie.h>
#include "test_helpers.h"

#include <cstring>
#include <string>
#include <vector>

// ── Fixture helper ────────────────────────────────────────────────────────────

// Builds a synthetic in-memory dict and opens it via the C API.
struct CDictFixture {
    std::vector<uint8_t> dat, val;
    mie_dict_t* dict = nullptr;

    explicit CDictFixture(const std::vector<TEntry>& entries) {
        build_single(entries, dat, val);
        dict = mie_dict_open_memory(dat.data(), dat.size(),
                                    val.data(), val.size());
    }
    ~CDictFixture() { mie_dict_close(dict); }
};

// One-entry dict: key=0x21 → 巴 (tone1, freq=500)
static std::vector<TEntry> one_word() {
    // 巴 = UTF-8 E5 B7 B4
    return { { "\x21", 1, "\xe5\xb7\xb4", 500, 1 } };
}

// ── NULL-safety ───────────────────────────────────────────────────────────────
// All API functions must handle NULL gracefully (no crash, sane return value).

TEST(CApiNull, DictClose)       { mie_dict_close(nullptr); }
TEST(CApiNull, CtxDestroy)      { mie_ctx_destroy(nullptr); }
TEST(CApiNull, CtxCreateNull)   { EXPECT_EQ(nullptr, mie_ctx_create(nullptr, nullptr)); }
TEST(CApiNull, ProcessKey)      { EXPECT_EQ(0, mie_process_key(nullptr, 0, 0, 1)); }
TEST(CApiNull, ClearInput)      { mie_clear_input(nullptr); }
TEST(CApiNull, SetCommitCb)     { mie_set_commit_cb(nullptr, nullptr, nullptr); }
TEST(CApiNull, InputStr)        { EXPECT_STREQ("", mie_input_str(nullptr)); }
TEST(CApiNull, CompoundStr)     { EXPECT_STREQ("", mie_compound_str(nullptr)); }
TEST(CApiNull, ModeIndicator)   { EXPECT_STREQ("", mie_mode_indicator(nullptr)); }
TEST(CApiNull, CandCount)       { EXPECT_EQ(0,       mie_candidate_count(nullptr)); }
TEST(CApiNull, CandWord)        { EXPECT_EQ(nullptr, mie_candidate_word(nullptr, 0)); }
TEST(CApiNull, CandLang)        { EXPECT_EQ(-1,      mie_candidate_lang(nullptr, 0)); }
TEST(CApiNull, PageCount)       { EXPECT_EQ(0,       mie_cand_page_count(nullptr)); }
TEST(CApiNull, PageCandCount)   { EXPECT_EQ(0,       mie_page_cand_count(nullptr)); }
TEST(CApiNull, PageCandWord)    { EXPECT_EQ(nullptr, mie_page_cand_word(nullptr, 0)); }
TEST(CApiNull, PageCandLang)    { EXPECT_EQ(-1,      mie_page_cand_lang(nullptr, 0)); }
TEST(CApiNull, PageSel)         { EXPECT_EQ(0,       mie_page_sel(nullptr)); }
TEST(CApiNull, CandPage)        { EXPECT_EQ(0,       mie_cand_page(nullptr)); }

// ── Dictionary open / close ───────────────────────────────────────────────────

TEST(CApiDict, OpenMemorySuccess) {
    CDictFixture f(one_word());
    ASSERT_NE(nullptr, f.dict);
}

TEST(CApiDict, OpenMemoryBadMagic) {
    uint8_t junk[] = { 0x00, 0x01, 0x02, 0x03 };
    EXPECT_EQ(nullptr, mie_dict_open_memory(junk, sizeof(junk),
                                             junk, sizeof(junk)));
}

// ── Context create / destroy ──────────────────────────────────────────────────

TEST(CApiCtx, CreateAndDestroy) {
    CDictFixture f(one_word());
    ASSERT_NE(nullptr, f.dict);
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    mie_ctx_destroy(ctx);
}

TEST(CApiCtx, CreateWithEnDictNull) {
    CDictFixture f(one_word());
    ASSERT_NE(nullptr, f.dict);
    // en = nullptr is valid — just disables English candidates
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    EXPECT_NE(nullptr, ctx);
    mie_ctx_destroy(ctx);
}

// ── Mode indicator ────────────────────────────────────────────────────────────

TEST(CApiDisplay, InitialModeIsZh) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    // SmartZh indicator = "中" (UTF-8: E4 B8 AD)
    EXPECT_STREQ("\xe4\xb8\xad", mie_mode_indicator(ctx));
    mie_ctx_destroy(ctx);
}

TEST(CApiDisplay, ModeAdvancesOnModeKey) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    // MODE key = (4, 0)
    mie_process_key(ctx, 4, 0, 1);
    EXPECT_STREQ("EN", mie_mode_indicator(ctx));
    mie_ctx_destroy(ctx);
}

// ── Input string ─────────────────────────────────────────────────────────────

TEST(CApiDisplay, InitialInputStrEmpty) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    EXPECT_STREQ("", mie_input_str(ctx));
    EXPECT_STREQ("", mie_compound_str(ctx));
    mie_ctx_destroy(ctx);
}

TEST(CApiDisplay, ProcessKeyUpdatesInputStr) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    int refresh = mie_process_key(ctx, 0, 0, 1);  // (0,0) = ㄅ/ㄉ
    EXPECT_NE(0, refresh);
    EXPECT_NE(0, (int)strlen(mie_input_str(ctx)));
    mie_ctx_destroy(ctx);
}

TEST(CApiDisplay, CompoundStrNonEmptyAfterKey) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    mie_process_key(ctx, 0, 0, 1);
    EXPECT_NE(0, (int)strlen(mie_compound_str(ctx)));
    mie_ctx_destroy(ctx);
}

// ── Candidates ────────────────────────────────────────────────────────────────

TEST(CApiCand, NoCandidatesInitially) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    EXPECT_EQ(0, mie_candidate_count(ctx));
    mie_ctx_destroy(ctx);
}

TEST(CApiCand, CandidatesAfterKeyPress) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    mie_process_key(ctx, 0, 0, 1);  // (0,0) → key 0x21 in dict
    EXPECT_GT(mie_candidate_count(ctx), 0);
    mie_ctx_destroy(ctx);
}

TEST(CApiCand, CandidateWordAndLang) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    mie_process_key(ctx, 0, 0, 1);
    ASSERT_GT(mie_candidate_count(ctx), 0);
    const char* word = mie_candidate_word(ctx, 0);
    ASSERT_NE(nullptr, word);
    EXPECT_GT((int)strlen(word), 0);
    EXPECT_EQ(0, mie_candidate_lang(ctx, 0));  // ZH
    mie_ctx_destroy(ctx);
}

TEST(CApiCand, OutOfRangeReturnsNull) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    mie_process_key(ctx, 0, 0, 1);
    int n = mie_candidate_count(ctx);
    EXPECT_EQ(nullptr, mie_candidate_word(ctx, n));
    EXPECT_EQ(-1,       mie_candidate_lang(ctx, n));
    mie_ctx_destroy(ctx);
}

// ── Commit callback ───────────────────────────────────────────────────────────

TEST(CApiCommit, CallbackFiresOnOk) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);

    std::string committed;
    mie_set_commit_cb(ctx,
        [](const char* utf8, void* ud) {
            *reinterpret_cast<std::string*>(ud) += utf8;
        },
        &committed);

    mie_process_key(ctx, 0, 0, 1);   // press (0,0) → candidates
    ASSERT_GT(mie_candidate_count(ctx), 0);
    mie_process_key(ctx, 5, 4, 1);   // OK → commit first candidate
    EXPECT_FALSE(committed.empty());
    mie_ctx_destroy(ctx);
}

TEST(CApiCommit, ClearCallbackWithNull) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);

    int fired = 0;
    mie_set_commit_cb(ctx,
        [](const char*, void* ud) { ++*reinterpret_cast<int*>(ud); },
        &fired);

    // Replace with nullptr — subsequent commits must not crash
    mie_set_commit_cb(ctx, nullptr, nullptr);
    mie_process_key(ctx, 0, 0, 1);
    mie_process_key(ctx, 5, 4, 1);
    EXPECT_EQ(0, fired);
    mie_ctx_destroy(ctx);
}

// ── Clear input ───────────────────────────────────────────────────────────────

TEST(CApiCtx, ClearInputResetsState) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    mie_process_key(ctx, 0, 0, 1);
    EXPECT_GT(mie_candidate_count(ctx), 0);
    mie_clear_input(ctx);
    EXPECT_STREQ("", mie_input_str(ctx));
    EXPECT_EQ(0, mie_candidate_count(ctx));
    mie_ctx_destroy(ctx);
}

// ── Pagination ────────────────────────────────────────────────────────────────

TEST(CApiPage, PageSizeIsFixed) {
    EXPECT_EQ(5, mie_page_size());
}

TEST(CApiPage, InitialPageState) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    mie_process_key(ctx, 0, 0, 1);
    EXPECT_EQ(0, mie_cand_page(ctx));
    EXPECT_GE(mie_cand_page_count(ctx), 1);
    EXPECT_GT(mie_page_cand_count(ctx), 0);
    EXPECT_NE(nullptr, mie_page_cand_word(ctx, 0));
    EXPECT_EQ(0, mie_page_cand_lang(ctx, 0));  // ZH
    EXPECT_EQ(0, mie_page_sel(ctx));
    mie_ctx_destroy(ctx);
}

TEST(CApiPage, PageCandWordOobReturnsNull) {
    CDictFixture f(one_word());
    mie_ctx_t* ctx = mie_ctx_create(f.dict, nullptr);
    ASSERT_NE(nullptr, ctx);
    mie_process_key(ctx, 0, 0, 1);
    int pcnt = mie_page_cand_count(ctx);
    EXPECT_EQ(nullptr, mie_page_cand_word(ctx, pcnt));
    EXPECT_EQ(-1,       mie_page_cand_lang(ctx, pcnt));
    mie_ctx_destroy(ctx);
}
