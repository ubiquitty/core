// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The terminal limits a local share is captured and admitted under. Header-only
// and free of the share's transport, so a decoder harness reads a share body
// under the daemon's own policy without linking sockets or processes.

#include "Core/Cell.h"
#include "Core/ClusterPool.h"
#include "Core/Image/TerminalImage.h"
#include "Core/LineStore.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/RunTextStore.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Ubiquitty::Session {

/// Chooses finite local terminal limits independently at each handoff endpoint.
///
/// Encoded sections share the smaller of `maxBodyBytes` and the local adopt
/// maximum; grid, history descriptor/cell counts, image policy, text identities,
/// Tek lists, and PCM have separate finite bounds.
/// These component limits are not an aggregate preparation-memory allowance or
/// a guarantee that every bounded container allocation can recover from failure.
/// - Parameter maxBodyBytes: This endpoint's complete encoded-record ceiling.
/// - Returns: The local admission policy, never inferred from carried state.
[[nodiscard]] inline Protocol::Sync::HandoverTerminalLimits localShareTerminalLimits(std::size_t maxBodyBytes)
{
    const auto maxEncodedBytes = std::min(maxBodyBytes, Protocol::Local::maxAdoptBodyBytes);
    // Geometry and PCM products are evaluated before narrowing to size_t, whose
    // width also covers wasm32. Each is a component ceiling, not a peak budget.
    constexpr auto maxComponentBytes = std::numeric_limits<std::uint64_t>::max();
    static_assert(std::uint64_t { maxRows } <= maxComponentBytes / 2 / maxColumns / sizeof(Cell));
    static_assert(std::uint64_t { VT::sampleSlotCount } <= (maxComponentBytes - VT::PendingSound::maxPCMBytes) / VT::sampleMaxFrames / sizeof(std::int16_t));
    static_assert(std::uint64_t { ClusterPool::maxEntries } <= (maxComponentBytes - RunTextStore::maxLiveCodePoints) / ClusterPool::maxCodePoints);
    constexpr std::uint64_t screenCellBytes = std::uint64_t { 2 } * maxRows * maxColumns * sizeof(Cell);
    constexpr std::uint64_t pcmBytes = std::uint64_t { VT::sampleSlotCount } * VT::sampleMaxFrames * sizeof(std::int16_t) + VT::PendingSound::maxPCMBytes;
    constexpr std::uint64_t textCodePoints = std::uint64_t { ClusterPool::maxEntries } * ClusterPool::maxCodePoints + RunTextStore::maxLiveCodePoints;
    constexpr std::uint64_t historyCapacity = Protocol::Local::maxAdoptBodyBytes / sizeof(StoredLine);
    constexpr auto maxSize = std::numeric_limits<std::size_t>::max();
    static_assert(ClusterPool::maxEntries <= std::numeric_limits<std::uint16_t>::max());
    static_assert(RunTextStore::maxEntries <= std::numeric_limits<std::uint16_t>::max());
    static_assert(Tek::tekMaxPrimitives <= std::numeric_limits<std::uint16_t>::max());
    // A tiny carried section must not announce an unbounded empty descriptor
    // ring. Its count policy is fixed by the local maximum, not its wire size.
    const auto maxHistoryCapacity = static_cast<std::uint32_t>(std::min({ historyCapacity,
        static_cast<std::uint64_t>(std::numeric_limits<int>::max()), static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) }));
    return {
        .maxRecordBytes = maxEncodedBytes,
        .maxCheckpointBytes = maxEncodedBytes,
        .maxScreenCellBytes = static_cast<std::size_t>(std::min<std::uint64_t>(screenCellBytes, maxSize)),
        .maxHostStateBytes = maxEncodedBytes,
        .maxLinkBytes = maxEncodedBytes,
        .maxParserBytes = maxEncodedBytes,
        // The carried image budget is policy as well as retained pixels. A
        // default empty terminal carries this budget even with no pixel bytes.
        .images = { maxEncodedBytes, Image::terminalImageBytesBudgetDefault },
        .history = { maxEncodedBytes, maxHistoryCapacity, maxHistoryCapacity, Protocol::Local::maxAdoptBodyBytes / sizeof(Cell) },
        .text = { maxEncodedBytes, static_cast<std::uint16_t>(ClusterPool::maxEntries), static_cast<std::uint16_t>(RunTextStore::maxEntries),
            static_cast<std::uint32_t>(std::min<std::uint64_t>(textCodePoints, std::numeric_limits<std::uint32_t>::max())) },
        .visual = { maxEncodedBytes, static_cast<std::uint16_t>(Tek::tekMaxPrimitives) },
        .sounds = { maxEncodedBytes, static_cast<std::size_t>(std::min<std::uint64_t>(pcmBytes, maxSize)) },
    };
}

} // namespace Ubiquitty::Session
