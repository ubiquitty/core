// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Shared lookups over the sorted UCD snapshot tables (*.tables.inc): rows sorted
// ascending, probed by binary search; hot per-codePoint properties flattened
// into a content-deduplicated stage1/stage2 byte trie.

#include <array>
#include <cstdint>
#include <cstring>

namespace Ubiquitty::Unicode {

// True when codePoint falls inside one of the sorted, disjoint
// [first, last] rows.
template<typename Range> inline bool rangesContain(const Range* ranges, int count, std::uint32_t codePoint)
{
    int low = 0;
    int high = count - 1;
    while (low <= high) {
        int middle = low + (high - low) / 2;
        if (codePoint < ranges[middle].first)
            high = middle - 1;
        else if (codePoint > ranges[middle].last)
            low = middle + 1;
        else
            return true;
    }
    return false;
}

// The row whose [first, last] contains codePoint, or nullptr — for
// tables that carry a payload alongside the range.
template<typename Range> inline const Range* findRange(const Range* ranges, int count, std::uint32_t codePoint)
{
    int low = 0;
    int high = count - 1;
    while (low <= high) {
        int middle = low + (high - low) / 2;
        if (codePoint < ranges[middle].first)
            high = middle - 1;
        else if (codePoint > ranges[middle].last)
            low = middle + 1;
        else
            return &ranges[middle];
    }
    return nullptr;
}

// The row whose key member equals key, or nullptr — for tables keyed by
// a single sorted value (presentation forms, packed DBCS pairs).
template<typename Row, typename Key> inline const Row* findByKey(const Row* rows, int count, Key Row::* keyMember, Key key)
{
    int low = 0;
    int high = count - 1;
    while (low <= high) {
        int middle = low + (high - low) / 2;
        if (key < rows[middle].*keyMember)
            high = middle - 1;
        else if (key > rows[middle].*keyMember)
            low = middle + 1;
        else
            return &rows[middle];
    }
    return nullptr;
}

// Build a content-deduplicated two-stage byte trie over the full
// Unicode range: stage1[codePoint >> 8] selects a shared 256-byte
// stage2 block. Block 0 is the all-defaultByte block; fillBlock(block,
// base) overlays the 256 codePoints starting at base and returns true
// when it wrote anything (an untouched block maps straight to block 0).
// Returns false when maxBlocks would be exceeded — the caller abandons
// the trie and keeps its binary-search fallback.
template<int maxBlocks, typename FillBlock> inline bool buildByteTrie(std::uint16_t (&stage1)[0x1100], std::uint8_t (&stage2)[maxBlocks][256], int& blocksUsed, std::uint8_t defaultByte, FillBlock&& fillBlock)
{
    for (int k = 0; k < 256; ++k)
        stage2[0][k] = defaultByte;
    blocksUsed = 1;
    std::array<std::uint8_t, 256> block;
    for (std::uint32_t blockIndex = 0; blockIndex < 0x1100; ++blockIndex) {
        const std::uint32_t base = blockIndex << 8;
        block.fill(defaultByte);
        if (!fillBlock(block.data(), base)) {
            stage1[blockIndex] = 0;
            continue;
        }
        int found = -1;
        for (int k = 0; k < blocksUsed; ++k) {
            if (std::memcmp(stage2[k], block.data(), block.size()) == 0) {
                found = k;
                break;
            }
        }
        if (found >= 0) {
            stage1[blockIndex] = static_cast<std::uint16_t>(found);
        } else if (blocksUsed < maxBlocks) {
            std::memcpy(stage2[blocksUsed], block.data(), block.size());
            stage1[blockIndex] = static_cast<std::uint16_t>(blocksUsed++);
        } else {
            return false;
        }
    }
    return true;
}

} // namespace Ubiquitty::Unicode
