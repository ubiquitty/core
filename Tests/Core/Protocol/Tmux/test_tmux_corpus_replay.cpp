// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_tmux_corpus_replay.cpp — the tmux Control Mode fuzz corpus, replayed as a
// test.
//
// Tests/Core/Protocol/Tmux/fuzz-corpus holds inputs a fuzzer kept, including the
// ones that drive the `tmux -CC` parser down a refusal path. fuzz_tmux.cpp is a
// libFuzzer harness, so it needs -fsanitize=fuzzer to link and is no ctest
// entry: without this replay no corpus input runs in the suite, as with the
// sync corpus and test_sync_corpus_replay.
//
// Replaying it needs no fuzzer. LLVMFuzzerTestOneInput is an ordinary function
// and the corpus is ordinary files: feeding one to the other is deterministic
// and drives the recursive layout decoder, the octal-escape `%output` path and
// the integer parsers down inputs no hand-written case reaches.
//
// What a replay asserts is what a replay can: every input is consumed without
// crashing, and the harness is reached for each. It is not an oracle for what the
// parser returned — the harness discards that, which is why the tmux decoders
// keep their own tests. The value here is the paths, not the verdicts.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

// The corpus sits beside this file, which the build does not copy, so its path
// comes from the build system — the same reason as the sync corpus: the compiler
// cache rewrites __FILE__, so anything derived from it resolves against a working
// directory this binary does not run in.
#ifndef UBQ_TMUX_FUZZ_CORPUS_DIR
#error "UBQ_TMUX_FUZZ_CORPUS_DIR must name the corpus; see Tests/CMakeLists.txt"
#endif

static std::filesystem::path corpusDirectory()
{
    return std::filesystem::path(UBQ_TMUX_FUZZ_CORPUS_DIR);
}

TEST(everyCorpusInputReplaysWithoutCrashing)
{
    const std::filesystem::path directory = corpusDirectory();
    CHECK_MSG(std::filesystem::is_directory(directory),
        "the fuzz corpus is not where this test looks for it");

    int replayed = 0;
    std::size_t largest = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file())
            continue;
        std::ifstream file(entry.path(), std::ios::binary);
        CHECK_MSG(file.good(), "a corpus file could not be opened");
        const std::vector<std::uint8_t> bytes {
            std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()
        };
        (void)LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
        ++replayed;
        if (bytes.size() > largest)
            largest = bytes.size();
    }

    // A replay that found no files passes every assertion above by never running
    // one, which is the shape this suite keeps hitting. The count is the
    // assertion that the loop had a subject.
    CHECK_MSG(replayed > 0, "the corpus directory held no files to replay");
    CHECK_MSG(largest > 0, "every corpus file was empty, so nothing was decoded");
}

UBIQUITTY_TEST_MAIN()
