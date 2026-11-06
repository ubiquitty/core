// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_richtext_corpus_replay.cpp — the RTF corpus, replayed through the fuzz
// harness as a test.
//
// Tests/Corpus/richtext holds bodies the platforms' own writers produce and the
// shapes that stress the reader: deep groups, long parameters, broken
// surrogates, tables at their caps. fuzz_richtext.cpp checks the document
// invariants on every body the reader accepts, so a replay asserts those, and
// the count asserts the loop had a subject.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

// The corpus sits in the source tree, which the build does not copy, so its
// location comes from the build system.
#ifndef UBQ_RICHTEXT_CORPUS_DIR
#error "UBQ_RICHTEXT_CORPUS_DIR must name the corpus; see Tests/CMakeLists.txt"
#endif

TEST(everyCorpusBodyReplaysWithItsInvariantsHeld)
{
    const std::filesystem::path directory(UBQ_RICHTEXT_CORPUS_DIR);
    CHECK_MSG(std::filesystem::is_directory(directory), "the corpus is not where this test looks for it");
    int replayed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file())
            continue;
        std::ifstream file(entry.path(), std::ios::binary);
        CHECK_MSG(file.good(), "a corpus file could not be opened");
        const std::vector<std::uint8_t> bytes { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        (void)LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
        ++replayed;
    }
    CHECK_MSG(replayed >= 8, "the corpus directory held fewer bodies than it ships");
}

UBIQUITTY_TEST_MAIN()
