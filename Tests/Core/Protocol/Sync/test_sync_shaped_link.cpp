// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_shaped_link.cpp — the Sync wire over a link with a rate and a
// delay, over the whole corpus.
//
// Every entry of Tests/Corpus/sync is replayed through LoopbackPaneLink with a
// shaped link between the host and client ends: frames leave the host one at a
// time as the link drains and arrive a serialization time and a delay later,
// and acknowledgements come back after the delay. Whatever the link, the
// client must hold the host's screen and history once the link drains.
//
// The same replay is the measurement the host's pacing constants are chosen
// by (Docs/sync.md decision D15). With UBQ_SYNC_CORPUS_REPORT set it prints,
// per link and per variant of a constant, how stale the client's screen ran
// behind the host and what the link carried:
//
//     UBQ_SYNC_CORPUS_REPORT=1 <build>/Tests/test_sync_shaped_link
//
// Docs/sync-measurements.md records the corpus replay method behind that
// output.

#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "SyncCorpus.h"

#include "Core/Protocol/Sync/Connection.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using Ubiquitty::Protocol::Sync::HostPolicy;
using UbiquittyTest::CorpusEntry;
using UbiquittyTest::LinkShape;
using UbiquittyTest::LoopbackOptions;
using UbiquittyTest::LoopbackPaneLink;

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

struct NamedLink {
    const char* name;
    LinkShape shape;
};

// A phone on a slow cellular link, a typical remote shell, and a fast LAN.
constexpr NamedLink links[] = {
    { "1 Mbit/s, 60 ms", { .bytesPerSecond = 125'000, .oneWayDelayMicroseconds = 30'000 } },
    { "10 Mbit/s, 20 ms", { .bytesPerSecond = 1'250'000, .oneWayDelayMicroseconds = 10'000 } },
    { "100 Mbit/s, 1 ms", { .bytesPerSecond = 12'500'000, .oneWayDelayMicroseconds = 500 } },
};

// The refresh interval a 60 Hz client reports.
constexpr std::uint32_t refreshInterval = 16'667;

// The stall bound's floor the spread is read against (Docs/sync.md §6.9).
constexpr std::uint64_t stallFloorMicroseconds = 750'000;

// The acknowledgement delay a client holds one for.
constexpr std::uint64_t ackDelay = 4'000;

struct Variant {
    std::string name;
    HostPolicy policy;
};

std::vector<Variant> variants()
{
    const HostPolicy chosen = Ubiquitty::Protocol::Sync::measuredHostPolicy(0);
    std::vector<Variant> list { { "chosen", chosen } };
    for (const std::uint64_t floor : { std::uint64_t { 4 } << 10, std::uint64_t { 16 } << 10, std::uint64_t { 64 } << 10 }) {
        HostPolicy policy = chosen;
        policy.pacing.windowFloorBytes = floor;
        list.push_back({ "window floor " + std::to_string(floor >> 10) + " KiB", policy });
    }
    for (const std::uint32_t share : { 125u, 250u, 500u }) {
        if (share == chosen.lanes.bulkSharePermille)
            continue;
        HostPolicy policy = chosen;
        policy.lanes.bulkSharePermille = share;
        list.push_back({ "bulk share " + std::to_string(share) + " per mille", policy });
    }
    for (const std::uint64_t probe : { std::uint64_t { 500'000 }, std::uint64_t { 8'000'000 } }) {
        HostPolicy policy = chosen;
        policy.pacing.probeIdleMicroseconds = probe;
        list.push_back({ "probe after " + std::to_string(probe / 1000) + " ms idle", policy });
    }
    for (const std::uint64_t span : { std::uint64_t { 2'500'000 }, std::uint64_t { 40'000'000 } }) {
        HostPolicy policy = chosen;
        policy.pacing.roundTripStaleMicroseconds = span;
        policy.pacing.rateFilterMicroseconds = span;
        list.push_back({ "round trip and rate kept " + std::to_string(span / 1000) + " ms", policy });
    }
    return list;
}

struct Totals {
    std::vector<std::uint64_t> staleness;
    /// Each frame's wait in hundredths of the round trip the host held at
    /// its publish.
    std::vector<std::uint64_t> stallRatios;
    /// The longest wait of a frame published while the round trip was under
    /// the stall floor's quarter, where the floor and not the multiple bounds.
    std::uint64_t longestUnderFloor = 0;
    std::uint64_t bytes = 0;
    std::uint64_t frames = 0;
    std::uint64_t updates = 0;
    std::uint64_t checkpoints = 0;
};

// Whether a checkpoint travels while the program writes: the grid is resized
// half way through each entry.
enum class MidwayCheckpoint : std::uint8_t {
    Absent,
    Present,
};

Totals run(const std::vector<CorpusEntry>& corpus, const LinkShape& shape, const HostPolicy& policy, MidwayCheckpoint midway)
{
    Totals totals;
    for (const CorpusEntry& entry : corpus) {
        const std::vector<UbiquittyTest::PTYRead> reads = entry.ptyReads();
        std::optional<std::size_t> resizeBeforeRead;
        if (midway == MidwayCheckpoint::Present)
            resizeBeforeRead = reads.size() / 2;
        LoopbackPaneLink link(LoopbackOptions { .rows = entry.rows, .columns = entry.columns, .link = shape, .hostPolicy = policy, .ackDelayMicroseconds = ackDelay, .refreshIntervalMicroseconds = refreshInterval, .resizeBeforeRead = resizeBeforeRead });
        link.replay(reads);
        CHECK_MSG(link.failure().empty(), (entry.name + ": " + link.failure()).c_str());
        const std::vector<std::uint64_t> waits = link.stalenessMicroseconds();
        totals.staleness.insert(totals.staleness.end(), waits.begin(), waits.end());
        for (const auto& [wait, roundTrip] : link.stalenessAgainstRoundTrip()) {
            totals.stallRatios.push_back(wait * 100 / roundTrip);
            if (roundTrip * 4 < stallFloorMicroseconds)
                totals.longestUnderFloor = std::max(totals.longestUnderFloor, wait);
        }
        totals.bytes += link.linkBytes();
        totals.frames += link.frames().size();
        totals.updates += link.hostEnd().counters().chainUpdatesSent;
        totals.checkpoints += link.hostEnd().counters().checkpointsSent;
    }
    return totals;
}

} // namespace

TEST(everyCorpusClientConvergesOverEveryShapedLink)
{
    const std::vector<CorpusEntry> corpus = loadOrFail();
    CHECK_MSG(!corpus.empty(), "the corpus is empty");
    const bool isReporting = std::getenv("UBQ_SYNC_CORPUS_REPORT") != nullptr;
    for (const MidwayCheckpoint midway : { MidwayCheckpoint::Absent, MidwayCheckpoint::Present }) {
        if (isReporting) {
            std::printf("\n%s\n\n", midway == MidwayCheckpoint::Present ? "With a resize half way through each entry:" : "As recorded:");
            std::printf("| link | variant | frames | updates sent | checkpoints | KiB carried | staleness p50 ms | p99 ms | max ms |\n");
            std::printf("|---|---|---|---|---|---|---|---|---|\n");
        }
        for (const NamedLink& named : links) {
            for (const Variant& variant : isReporting ? variants() : std::vector<Variant> { variants().front() }) {
                const Totals totals = run(corpus, named.shape, variant.policy, midway);
                CHECK_MSG(!totals.staleness.empty(), "no frame reached the client");
                if (!isReporting)
                    continue;
                std::printf("| %s | %s | %llu | %llu | %llu | %llu | %.1f | %.1f | %.1f |\n", named.name, variant.name.c_str(),
                    static_cast<unsigned long long>(totals.frames), static_cast<unsigned long long>(totals.updates),
                    static_cast<unsigned long long>(totals.checkpoints), static_cast<unsigned long long>(totals.bytes >> 10),
                    static_cast<double>(percentile(totals.staleness, 50)) / 1000.0, static_cast<double>(percentile(totals.staleness, 99)) / 1000.0,
                    static_cast<double>(percentile(totals.staleness, 100)) / 1000.0);
            }
        }
    }
    if (!isReporting)
        return;
    // A receipt or an echo rides the interactive lane with the frames, so a
    // frame's wait against the round trip the host held is the spread a stall
    // bound of max(k × srtt, floor) must clear (Docs/sync.md §6.9).
    std::printf("\nA frame's wait against the smoothed round trip at its publish, the resize run included:\n\n");
    std::printf("| link | frames | wait / srtt p50 | p99 | p99.9 | max | longest wait under the floor ms |\n");
    std::printf("|---|---|---|---|---|---|---|\n");
    for (const NamedLink& named : links) {
        Totals both = run(corpus, named.shape, variants().front().policy, MidwayCheckpoint::Absent);
        const Totals resized = run(corpus, named.shape, variants().front().policy, MidwayCheckpoint::Present);
        both.stallRatios.insert(both.stallRatios.end(), resized.stallRatios.begin(), resized.stallRatios.end());
        both.longestUnderFloor = std::max(both.longestUnderFloor, resized.longestUnderFloor);
        const auto ratio = [&](int percent) { return static_cast<double>(percentile(both.stallRatios, percent)) / 100.0; };
        std::vector<std::uint64_t> sorted = both.stallRatios;
        std::ranges::sort(sorted);
        const double perMille999 = sorted.empty() ? 0.0 : static_cast<double>(sorted[std::min(sorted.size() - 1, sorted.size() * 999 / 1000)]) / 100.0;
        std::printf("| %s | %zu | %.2f | %.2f | %.2f | %.2f | %.1f |\n", named.name, both.stallRatios.size(), ratio(50), ratio(99), perMille999, ratio(100),
            static_cast<double>(both.longestUnderFloor) / 1000.0);
    }
}

UBIQUITTY_TEST_MAIN()
