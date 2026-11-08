// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Builds with Scripts/build-perf-bench.sh bench_sync_history_decode.
// Measures complete decode and cache admission, keeping the wire fingerprint
// and row count observable. Define UBIQUITTY_HISTORY_BENCH_BORROWED for an arm
// whose cache still accepts only borrowed batches.
#include "BenchHarness.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Ubiquitty;
namespace Sync = Protocol::Sync;

int main(int argc, char** argv)
{
    bool shouldEmitJSON = false;
    for (int index = 1; index < argc; ++index)
        shouldEmitJSON = shouldEmitJSON || std::strcmp(argv[index], "--json") == 0;
    std::vector<Bench::Result> results;
    for (int shape = 0; shape < 2; ++shape) {
        auto terminal = std::make_unique<Terminal>();
        resize(*terminal, 24, 160);
        for (int row = 0; row < 256; ++row) {
            const std::string prefix = shape ? "\x1b[38;2;12;34;56m\x1b]8;;https://example.com/\x1b\\" : "";
            const std::string suffix = shape ? "\x1b]8;;\x1b\\\x1b[0m \xe7\x95\x8c e\xcc\x81" : "";
            processBytes(terminal.get(), prefix + std::to_string(row) + " " + std::string(100, 'a') + suffix + "\r\n");
        }
        Sync::HistoryPublisher publisher;
        if (!publisher.capture(*terminal))
            return 2;
        const auto address = Screen::historyAddress(*terminal);
        std::vector<std::uint8_t> body;
        const auto count = publisher.writeBody(*terminal, { address.oldest, address.head }, Sync::HistoryOrder::Ascending, Sync::maxHistoryTransferBytes, body);
        if (!count || body.empty())
            return 2;
        std::uint64_t fingerprint = 1469598103934665603ull;
        for (auto byte : body)
            fingerprint = (fingerprint ^ byte) * 1099511628211ull;
        bool didDecode = true;
        const auto timing = Bench::measure(3, 40, [&] {
            std::vector<Sync::DecodedLine> lines;
            const bool didDecodeBody = Sync::decodeHistoryBody(body, address.oldest, Sync::HistoryOrder::Ascending, Sync::historyBodyLimits, lines);
            Sync::HistoryCache cache({ 1, 2 }, { 3, 4 }, 10000);
            const auto reset = cache.reset({ .newEpoch = 7, .clearedThrough = address.oldest, .historyHead = address.head });
#if defined(UBIQUITTY_HISTORY_BENCH_BORROWED)
            const auto stored = cache.insert(7, lines);
#else
            const auto stored = cache.insertOwned(7, std::move(lines));
#endif
            const auto* last = cache.line(address.oldest + count - 1);
            didDecode = didDecode && didDecodeBody && reset == Sync::HistoryPurgeOutcome::Purged && stored == Sync::CacheInsert::Stored && last;
            Bench::doNotOptimize(last);
            Bench::clobberMemory();
        });
        if (!didDecode)
            return 2;
        const char* name = shape ? "history_decode_cache_rich" : "history_decode_cache_ascii";
        std::printf("%s rows=%zu wire=%zu fingerprint=%016llx median_us=%.2f p95_us=%.2f\n",
            name, count, body.size(), static_cast<unsigned long long>(fingerprint), timing.medianSeconds * 1e6, timing.p95Seconds * 1e6);
        Bench::Result result;
        result.name = name;
        result.unit = "us";
        result.value = timing.medianSeconds * 1e6;
        result.p95 = timing.p95Seconds * 1e6;
        results.push_back(result);
    }
    if (shouldEmitJSON)
        Bench::emitJSON(results, "portable", "");
    return 0;
}
