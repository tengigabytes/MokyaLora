cmake_minimum_required(VERSION 3.20)

include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
# Prevent GoogleTest from overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(mie_tests
    test_trie_stub.cpp
    test_trie_searcher.cpp
)

target_link_libraries(mie_tests
    PRIVATE
        mie
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(mie_tests)
