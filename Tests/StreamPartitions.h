// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace UbiquittyTest {

/// Selects exhaustive compact-stream cuts or allocation-boundary cuts.
enum class PartitionCoverage {
    Exhaustive,
    Boundaries
};

/// Replays one stream whole, at two-part cuts, and in reproducible multi-part
/// deliveries. The callback returns false at the first differing partition.
/// Large streams use cuts around allocation quanta instead of quadratic replay.
///
/// - Parameters:
///   - length: The complete stream's byte count.
///   - coverage: The two-part cut policy; exhaustive also visits bytewise input.
///   - check: Replays the exclusive end offsets and reports equality; the label
///     identifies the cut or seed in a failure.
/// - Returns: Whether every visited partition passed.
template<typename Check>
bool sweepStreamPartitions(std::size_t length, PartitionCoverage coverage, Check&& check)
{
    std::vector<std::size_t> ends { length };
    if (!check(ends, "whole"))
        return false;
    std::vector<std::size_t> cuts { 0, length };
    if (coverage == PartitionCoverage::Exhaustive) {
        for (std::size_t cut = 1; cut < length; ++cut)
            cuts.push_back(cut);
    } else {
        for (std::size_t quantum = 1; quantum <= length; quantum *= 2) {
            for (const auto cut : { quantum - 1, quantum, quantum + 1 }) {
                if (cut <= length) {
                    cuts.push_back(cut);
                    cuts.push_back(length - cut);
                }
            }
            if (quantum > length / 2)
                break;
        }
    }
    std::ranges::sort(cuts);
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    for (const auto cut : cuts) {
        ends = { cut, length };
        if (!check(ends, "split=" + std::to_string(cut)))
            return false;
    }
    if (coverage == PartitionCoverage::Exhaustive) {
        ends.clear();
        for (std::size_t end = 1; end <= length; ++end)
            ends.push_back(end);
        if (!check(ends, "byte-at-a-time"))
            return false;
    }
    for (std::uint32_t seed = 1; seed <= 32; ++seed) {
        std::mt19937 random(seed);
        const std::size_t maximum = length > 1024 * 1024 ? 65536 : seed % 2 ? 17
                                                                            : 4096;
        ends.clear();
        for (std::size_t end = 0; end < length;) {
            end += std::min(length - end, 1 + static_cast<std::size_t>(random()) % maximum);
            ends.push_back(end);
        }
        if (!check(ends, "seed=" + std::to_string(seed)))
            return false;
    }
    return true;
}

/// Delivers each consecutive piece of one partition to its reader.
///
/// - Parameters:
///   - bytes: The whole immutable stream.
///   - ends: Ordered exclusive end offsets, ending at the stream's size.
///   - feed: Consumes one borrowed piece before the next is delivered.
template<typename Element, typename Feed>
void feedStreamPartition(std::span<const Element> bytes, std::span<const std::size_t> ends, Feed&& feed)
{
    std::size_t offset = 0;
    for (const auto end : ends) {
        feed(bytes.subspan(offset, end - offset));
        offset = end;
    }
}

} // namespace UbiquittyTest
