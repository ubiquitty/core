// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/ClusterPool.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Protocol/Sync/HandoverChannel.h"
#include "Core/RunTextStore.h"
#include "Core/Tek/Tek4014.h"
#include "PAL/OutputQueue.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

using namespace Ubiquitty::Protocol::Sync;

namespace {
void checkHeader(std::span<const std::uint8_t> input)
{
    constexpr HandoverHeaderLimits limits { 64, 1 << 20 };
    const auto header = decodeHandoverHeader(input, limits);
    if (!header)
        return;
    std::vector<std::uint8_t> encoded;
    encodeHandoverHeader(encoded, *header);
    assert(std::ranges::equal(input, encoded));
    assert(encoded.size() <= 153);
    assert(header->inherited.size() <= maxInheritedInstances);
    assert(header->paneCount <= limits.maxPanes && header->tableBytes <= limits.maxTableBytes);
    const auto repeated = decodeHandoverHeader(encoded, limits);
    assert(repeated && *repeated == *header);
    for (std::size_t length = 0; length < encoded.size(); ++length)
        assert(!decodeHandoverHeader(std::span(encoded).first(length), limits));
    encoded.push_back(0);
    assert(!decodeHandoverHeader(encoded, limits));
}
}

/// A pane record the receiver accepts is canonical: the restored pane encodes
/// to the same bytes. Seeded with the frozen record of the current version.
void checkPane(std::span<const std::uint8_t> input)
{
    constexpr HandoverPaneLimits limits {
        { 1 << 20, 1 << 18, 1 << 18, 1 << 18, 1 << 16, 4096, { 8192, 1 << 20 }, { 1 << 18, 2000, 1024, 100000 },
            { 1 << 18, Ubiquitty::ClusterPool::maxEntries, Ubiquitty::RunTextStore::maxEntries, 1 << 16 },
            { 1 << 16, Ubiquitty::Tek::tekMaxPrimitives }, { 1 << 18, 1 << 18 }, 4 << 20 },
        { 1 << 18, 4096, 1024, 1024, 4096 }, 2 << 20, 1 << 20, 1 << 18, 1 << 18, 1 << 18, 8
    };
    auto restored = decodePaneHandover(input, limits);
    if (!restored)
        return;
    const auto repeated = encodePaneHandover(*restored->source, restored->session, restored->unwrittenPTY, limits);
    assert(repeated && std::ranges::equal(repeated->view(), input));
}

/// Exercises the control decoder, not the separately authenticated transfer.
/// Accepted records must be canonical, bounded and reject every truncation.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span<const std::uint8_t> input(data, size);
    checkHeader(input);
    checkPane(input);
    // A complete bounded header makes every offset reachable even from an empty corpus.
    std::vector<std::uint8_t> headerBytes;
    encodeHandoverHeader(headerBytes, { { 1, 2 }, { { 3, 4 }, { 5, 6 } }, 2, 512 });
    for (std::size_t i = 1; i < input.size(); i += 2)
        headerBytes[input[i - 1] % headerBytes.size()] = input[i];
    checkHeader(headerBytes);
    assert(isHandoverPreamble(input) == std::ranges::equal(input, handoverPreamble));
    constexpr HandoverChannelLimits limits { 1, 64, 1 << 20, 1 << 20 };
    const auto decoded = decodeHandoverMessage(input, limits);
    if (!decoded)
        return 0;
    const auto encoded = encodeHandoverMessage(*decoded);
    assert(std::ranges::equal(input, encoded));
    assert(encoded.size() <= 25);
    const auto repeated = decodeHandoverMessage(encoded, limits);
    assert(repeated && *repeated == *decoded);
    for (std::size_t length = 0; length < encoded.size(); ++length)
        assert(!decodeHandoverMessage(std::span(encoded).first(length), limits));
    auto extended = encoded;
    extended.push_back(0);
    assert(!decodeHandoverMessage(extended, limits));
    std::visit([&](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, HandoverOffer>) {
            assert(value.bodyVersion && value.bodyVersion <= limits.maxBodyVersion);
            assert(value.paneCount <= limits.maxPanes && value.tableBytes <= limits.maxTableBytes);
            assert(value.listeners && value.listeners <= maxHandoverListeners && value.datagramSockets <= 1);
        } else if constexpr (std::is_same_v<Value, HandoverPaneFollows>)
            assert(value.index < limits.maxPanes && value.recordBytes && value.recordBytes <= limits.maxPaneBytes && value.sessionLeader > 1 && value.leaderParent);
    },
        *decoded);
    return 0;
}
