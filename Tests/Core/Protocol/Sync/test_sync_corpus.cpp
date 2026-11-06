// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_corpus.cpp — the Sync screen plane over the whole corpus.
//
// Every entry of Tests/Corpus/sync is replayed through LoopbackPaneLink, the
// daemon's publish path in memory, whose client is fed the connection's
// updates and must pass the whole oracle after every frame. At every frame the
// host is also checkpointed by a publisher of its own and the checkpoint
// applied to a client of its own, which must pass the oracle too.
//
// The same replay is the measurement. With UBQ_SYNC_CORPUS_REPORT set it
// prints, per entry, the wire bytes of the link's frames beside a checkpoint's,
// and how many of the link's frames had to be checkpoints:
//
//     UBQ_SYNC_CORPUS_REPORT=1 <build>/Tests/test_sync_corpus
//
// Keep captured reports in the external performance archive.

#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "SyncCorpus.h"

#include "Core/Protocol/Sync/ScreenPlane.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using Ubiquitty::Protocol::Sync::ApplyOutcome;
using Ubiquitty::Protocol::Sync::AttachmentBaseline;
using Ubiquitty::Protocol::Sync::ScreenPublisher;
using Ubiquitty::Protocol::Sync::ScreenReplica;
using UbiquittyTest::CorpusEntry;
using UbiquittyTest::LoopbackOptions;
using UbiquittyTest::LoopbackPaneLink;
using UbiquittyTest::PublishedFrame;

namespace {

std::vector<CorpusEntry> loadOrFail()
{
    std::string error;
    auto corpus = UbiquittyTest::loadSyncCorpus(UbiquittyTest::syncCorpusDirectory(), error);
    CHECK_MSG(corpus.has_value(), error.c_str());
    return corpus ? std::move(*corpus) : std::vector<CorpusEntry> {};
}

std::uint64_t percentile(std::vector<std::uint64_t> values, int percent)
{
    if (values.empty())
        return 0;
    std::ranges::sort(values);
    const std::size_t index = (values.size() * static_cast<std::size_t>(percent) + 99) / 100;
    return values[std::min(values.size() - 1, index == 0 ? 0 : index - 1)];
}

std::uint64_t mean(const std::vector<std::uint64_t>& values)
{
    std::uint64_t total = 0;
    for (const std::uint64_t value : values)
        total += value;
    return values.empty() ? 0 : total / values.size();
}

// What one entry cost.
struct EntryRun {
    std::string failure;
    std::vector<std::uint64_t> checkpointBytes;
    std::vector<std::uint64_t> updateBytes;
    std::size_t checkpointFrames = 0;
    std::size_t frames = 0;
};

EntryRun run(const CorpusEntry& entry)
{
    EntryRun result;
    auto checkpointClient = std::make_unique<Ubiquitty::Terminal>();
    ScreenPublisher checkpointPublisher;
    std::uint64_t generation = 0;

    LoopbackPaneLink link(LoopbackOptions { .rows = entry.rows, .columns = entry.columns });
    link.onFrame = [&](const PublishedFrame& frame, const LoopbackPaneLink& published) {
        if (!result.failure.empty())
            return;
        const Ubiquitty::Terminal& host = published.host();
        ++generation;
        ++result.frames;
        if (frame.isCheckpoint)
            ++result.checkpointFrames;
        else
            result.updateBytes.push_back(frame.wireBytes);
        const std::string where = entry.name + " frame " + std::to_string(result.frames) + ": ";

        AttachmentBaseline baseline;
        std::vector<std::uint8_t> body;
        if (!checkpointPublisher.capture(host) || !checkpointPublisher.checkpoint(host, generation, body, baseline)) {
            result.failure = where + "the host could not be checkpointed";
            return;
        }
        result.checkpointBytes.push_back(body.size());
        ScreenReplica replica;
        if (const ApplyOutcome outcome = applyCheckpoint(*checkpointClient, body, replica); outcome != ApplyOutcome::Applied) {
            result.failure = where + "the checkpoint was refused (" + std::to_string(static_cast<int>(outcome)) + ")";
            return;
        }
        if (std::string difference = UbiquittyTest::firstDisagreement(host, *checkpointClient); !difference.empty())
            result.failure = where + "after the checkpoint, " + difference;
    };
    link.replay(entry.ptyReads());
    if (result.failure.empty() && !link.failure().empty())
        result.failure = entry.name + ": the connection's client disagreed: " + link.failure();
    return result;
}

} // namespace

TEST(everyCorpusFrameRoundTripsThroughACheckpointAndTheConnection)
{
    const std::vector<CorpusEntry> corpus = loadOrFail();
    CHECK_MSG(!corpus.empty(), "the corpus is empty");
    const bool isReporting = std::getenv("UBQ_SYNC_CORPUS_REPORT") != nullptr;
    if (isReporting) {
        std::printf("| entry | frames | checkpoint B mean (p50, max) | update wire B/frame mean (p50, p99) | frames sent as checkpoints |\n");
        std::printf("|---|---|---|---|---|\n");
    }
    for (const CorpusEntry& entry : corpus) {
        const EntryRun result = run(entry);
        CHECK_MSG(result.failure.empty(), result.failure.c_str());
        CHECK_MSG(result.frames > 0, (entry.name + " published nothing").c_str());
        if (!isReporting)
            continue;
        std::printf("| %s | %zu | %llu (%llu, %llu) | %llu (%llu, %llu) | %zu |\n", entry.name.c_str(), result.frames,
            static_cast<unsigned long long>(mean(result.checkpointBytes)), static_cast<unsigned long long>(percentile(result.checkpointBytes, 50)),
            static_cast<unsigned long long>(percentile(result.checkpointBytes, 100)),
            static_cast<unsigned long long>(mean(result.updateBytes)), static_cast<unsigned long long>(percentile(result.updateBytes, 50)),
            static_cast<unsigned long long>(percentile(result.updateBytes, 99)), result.checkpointFrames);
    }
}

UBIQUITTY_TEST_MAIN()
