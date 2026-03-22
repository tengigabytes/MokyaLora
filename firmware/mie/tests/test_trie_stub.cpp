// test_trie_stub.cpp — build environment smoke test for MIE host build.
// This stub verifies that the GoogleTest infrastructure and the mie static
// library link correctly before any real source files exist.

#include <gtest/gtest.h>

TEST(MieStub, BuildEnvironmentWorks) {
    // If this test compiles and links, the host build infrastructure is healthy.
    SUCCEED();
}
