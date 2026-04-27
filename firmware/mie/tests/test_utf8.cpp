/* test_utf8.cpp — coverage for mie_utf8_truncate. */

#include <gtest/gtest.h>
#include <mie/utf8.h>

#include <cstring>

static size_t T(const char *s, size_t max) {
    return mie_utf8_truncate(s, std::strlen(s), max);
}

TEST(MieUtf8, NullSafe) {
    EXPECT_EQ(0u, mie_utf8_truncate(nullptr, 5, 10));
    EXPECT_EQ(0u, mie_utf8_truncate("abc", 3, 0));
}

TEST(MieUtf8, AsciiTrivial) {
    EXPECT_EQ(0u, T("ABCD", 0));
    EXPECT_EQ(2u, T("ABCD", 2));
    EXPECT_EQ(4u, T("ABCD", 4));
    EXPECT_EQ(4u, T("ABCD", 99));   /* len < max → return len */
}

TEST(MieUtf8, EmptyString) {
    EXPECT_EQ(0u, T("", 5));
    EXPECT_EQ(0u, T("", 0));
}

TEST(MieUtf8, ChineseExactBoundary) {
    /* 我=E68891 (3 B), 的=E79A84 (3 B). Total 6 B. */
    const char *s = "我的";
    ASSERT_EQ(6u, std::strlen(s));
    EXPECT_EQ(6u, T(s, 6));    /* exact fit */
    EXPECT_EQ(6u, T(s, 99));   /* len < max */
    EXPECT_EQ(3u, T(s, 3));    /* exactly first codepoint */
    EXPECT_EQ(0u, T(s, 0));
}

TEST(MieUtf8, ChineseMidCodepointBacksOff) {
    /* "我的鋰電" — four 3-byte codepoints. Total 12 B. */
    const char *s = "我的鋰電";
    ASSERT_EQ(12u, std::strlen(s));
    /* Asking for 5 → must back off to 3 (after first codepoint).  */
    EXPECT_EQ(3u, T(s, 5));
    /* 4 → back off to 3. */
    EXPECT_EQ(3u, T(s, 4));
    /* 7 → back off to 6 (two codepoints). */
    EXPECT_EQ(6u, T(s, 7));
    /* 8 → back off to 6. */
    EXPECT_EQ(6u, T(s, 8));
    /* 11 → back off to 9 (three codepoints). */
    EXPECT_EQ(9u, T(s, 11));
    /* 12 → exact fit. */
    EXPECT_EQ(12u, T(s, 12));
}

TEST(MieUtf8, FourByteSequence) {
    /* U+1F600 GRINNING FACE encodes as F0 9F 98 80 (4 bytes). */
    const char *s = "\xF0\x9F\x98\x80""abc";
    ASSERT_EQ(7u, std::strlen(s));
    /* Asking for 1, 2, 3 → all back off to 0 (incomplete codepoint). */
    EXPECT_EQ(0u, T(s, 1));
    EXPECT_EQ(0u, T(s, 2));
    EXPECT_EQ(0u, T(s, 3));
    /* 4 → exact codepoint boundary. */
    EXPECT_EQ(4u, T(s, 4));
    /* 5,6,7 → fits ASCII tail. */
    EXPECT_EQ(5u, T(s, 5));
    EXPECT_EQ(6u, T(s, 6));
    EXPECT_EQ(7u, T(s, 7));
}

TEST(MieUtf8, LatinTwoByte) {
    /* Latin-1 supplement: ñ = C3 B1 (2 B). */
    const char *s = "caña";  /* c, a, ñ, a → 5 bytes */
    ASSERT_EQ(5u, std::strlen(s));
    EXPECT_EQ(2u, T(s, 2));    /* "ca" */
    EXPECT_EQ(2u, T(s, 3));    /* mid-ñ → back to 2 */
    EXPECT_EQ(4u, T(s, 4));    /* "cañ" complete */
    EXPECT_EQ(5u, T(s, 5));    /* full */
}

TEST(MieUtf8, LenLessThanMaxIsIdentity) {
    /* If the source is shorter than max, return len even if max is huge.
     * (No memory access past `len` is performed when this branch fires.) */
    EXPECT_EQ(3u, mie_utf8_truncate("我", 3, 10));
    EXPECT_EQ(3u, mie_utf8_truncate("我", 3, 1000));
}

TEST(MieUtf8, MalformedLeadingContinuations) {
    /* All-continuation prefix — defensive: walks back to 0. */
    const char s[] = "\x80\x80\x80\x80";
    EXPECT_EQ(0u, mie_utf8_truncate(s, 4, 3));
}
