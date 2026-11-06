// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_sync_corpus_replay.cpp — the fuzz corpus, replayed as a test.
//
// Tests/Core/Protocol/Sync/fuzz-corpus holds frames a fuzzer found interesting
// enough to keep, including crash reproducers for the decoders.
// fuzz_sync.cpp is a libFuzzer harness, so it needs -fsanitize=fuzzer to link
// and is not a ctest entry: without this replay no corpus input runs in the
// suite, and the coverage index has no entry for fuzz_sync at all.
//
// Replaying it needs no fuzzer. LLVMFuzzerTestOneInput is an ordinary function,
// and the corpus is ordinary files: feeding one to the other is deterministic,
// takes about a second, and drives the decoders down refusal paths that no
// hand-written frame reaches — the ones a mutator found by not knowing the
// format.
//
// What this asserts is what a replay can assert: every input is consumed without
// crashing, and the harness is reached for each. It is not an oracle for what a
// decoder returned — the harness discards that, which is why the decoders needed
// their own tests. The value here is the paths, not the verdicts.

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

// The corpus sits beside this file in the source tree, which the build does not
// copy, so the location comes from the build system — which is the only thing
// that still knows the absolute source root.
//
// __FILE__ cannot answer it. The compiler cache rewrites absolute source paths
// so that checkouts at different paths share one cache, and __FILE__ follows
// the command line: it goes relative with everything else, then resolves
// against a working directory this binary does not run in, and the failure
// names a path nobody wrote.
#ifndef UBQ_SYNC_FUZZ_CORPUS_DIR
#error "UBQ_SYNC_FUZZ_CORPUS_DIR must name the corpus; see Tests/CMakeLists.txt"
#endif

static std::filesystem::path corpusDirectory()
{
    return std::filesystem::path(UBQ_SYNC_FUZZ_CORPUS_DIR);
}

TEST(everyCorpusFrameReplaysWithoutCrashing)
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

    // A replay that found no files would pass every assertion above by never
    // running one, which is the shape this suite keeps hitting. The count is the
    // assertion that the loop had a subject.
    CHECK_MSG(replayed > 0, "the corpus directory held no files to replay");
    CHECK_MSG(largest > 0, "every corpus file was empty, so nothing was decoded");
}

// The corpus speaks the current wire. A stream seed's framed Greet or Welcome
// is at this build's version, or 0 for the malformed-handshake path; one at
// another version is refused at its first frame and spends the fuzzer's time
// on that refusal. And the corpus does reach both handshakes whole, so the
// decoders behind them have inputs to mutate from.
TEST(theCorpusHandshakesAtTheCurrentWire)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    int greets = 0;
    int welcomes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(corpusDirectory())) {
        if (!entry.is_regular_file())
            continue;
        std::ifstream file(entry.path(), std::ios::binary);
        const std::vector<std::uint8_t> bytes { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        if (bytes.empty())
            continue;
        const int selector = bytes[0] % 10;
        if (selector != 0 && selector != 3 && selector != 5)
            continue;
        Sync::FrameDecoder decoder;
        decoder.append(selector == 0 ? std::span(bytes) : std::span(bytes).subspan(1));
        while (const std::optional<Sync::Frame> frame = decoder.next()) {
            if (frame->kind != Sync::FrameKind::Greet && frame->kind != Sync::FrameKind::Welcome)
                continue;
            if (frame->body.size() < 2)
                continue;
            const auto version = static_cast<std::uint16_t>(frame->body[0] | frame->body[1] << 8);
            CHECK_MSG(version == 0 || version == Sync::wireVersion,
                (entry.path().filename().string() + " frames a handshake at another wire version").c_str());
            if (frame->kind == Sync::FrameKind::Greet && Sync::decodeGreet(frame->body))
                ++greets;
            if (frame->kind == Sync::FrameKind::Welcome && Sync::decodeWelcome(frame->body))
                ++welcomes;
        }
    }
    CHECK_MSG(greets > 0, "no corpus stream carries a Greet this build admits");
    CHECK_MSG(welcomes > 0, "no corpus stream carries a Welcome this build admits");
}

UBIQUITTY_TEST_MAIN()
