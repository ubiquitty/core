// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Tests/Core/Protocol/Sync/SyncFixture.h — writes, reads and replays the
// divergence fixture a diagnostic client keeps (Docs/sync.md §6.2).
//
// A fixture is the checkpoint at the last verified state and every update
// after it through the one whose digest did not match. Replayed onto a fresh
// terminal it reproduces the divergence offline: which update diverged, and
// the row hashes the client built from it, which the host's journal line or
// its own row hashes then name the first wrong row of.
//
//     Fixture := magic:[4] "UBQ1" flags:u8 {Diverged, Incomplete}
//                checkpoint:bytes updates:[bytes]
//

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace UbiquittyTest {

/// The bytes a fixture opens with.
inline constexpr std::array<std::uint8_t, 4> syncFixtureMagic { 'U', 'B', 'Q', '1' };

/// Writes a fixture as bytes a file can hold.
///
/// - Parameter fixture: The fixture.
/// - Returns: The bytes.
inline std::vector<std::uint8_t> writeSyncFixture(const Ubiquitty::Protocol::Sync::DivergenceFixture& fixture)
{
    std::vector<std::uint8_t> bytes;
    Ubiquitty::Protocol::Sync::WireWriter writer(bytes);
    writer.raw(syncFixtureMagic);
    writer.u8(static_cast<std::uint8_t>((fixture.hasDiverged ? 1 : 0) | (fixture.isIncomplete ? 2 : 0)));
    writer.bytes(fixture.checkpoint);
    writer.varint(fixture.updates.size());
    for (const std::vector<std::uint8_t>& update : fixture.updates)
        writer.bytes(update);
    return bytes;
}

/// Reads a fixture `writeSyncFixture` wrote.
///
/// - Parameter bytes: The bytes.
/// - Returns: The fixture, or `nullopt` for bytes that are not one.
inline std::optional<Ubiquitty::Protocol::Sync::DivergenceFixture> readSyncFixture(std::span<const std::uint8_t> bytes)
{
    Ubiquitty::Protocol::Sync::WireReader reader(bytes);
    const std::span<const std::uint8_t> magic = reader.take(syncFixtureMagic.size());
    if (!reader.ok() || !std::ranges::equal(magic, syncFixtureMagic))
        return std::nullopt;
    Ubiquitty::Protocol::Sync::DivergenceFixture fixture;
    const std::uint8_t flags = reader.u8();
    fixture.hasDiverged = flags & 1;
    fixture.isIncomplete = flags & 2;
    const std::span<const std::uint8_t> checkpoint = reader.bytes(bytes.size());
    fixture.checkpoint.assign(checkpoint.begin(), checkpoint.end());
    const std::size_t count = reader.count(bytes.size(), 1);
    for (std::size_t index = 0; index < count && reader.ok(); ++index) {
        const std::span<const std::uint8_t> update = reader.bytes(bytes.size());
        fixture.updates.emplace_back(update.begin(), update.end());
        fixture.updateBytes += update.size();
    }
    if (!reader.ok() || !reader.isAtEnd())
        return std::nullopt;
    return fixture;
}

/// What replaying a fixture found.
struct SyncReplay {
    /// Whether the checkpoint applied.
    bool isCheckpointApplied = false;
    /// The index of the update that did not apply, or `nullopt` when all did.
    std::optional<std::size_t> divergedUpdate;
    /// The active screen's row hashes the client built: from the update that
    /// diverged, or after the last one.
    std::vector<std::uint64_t> builtRowHashes;
};

/// Replays a fixture onto a fresh terminal.
///
/// - Parameter fixture: The fixture.
/// - Returns: What it found.
inline SyncReplay replaySyncFixture(const Ubiquitty::Protocol::Sync::DivergenceFixture& fixture)
{
    SyncReplay result;
    Ubiquitty::Terminal terminal;
    Ubiquitty::Protocol::Sync::ScreenReplica replica;
    result.isCheckpointApplied = Ubiquitty::Protocol::Sync::applyCheckpoint(terminal, fixture.checkpoint, replica) == Ubiquitty::Protocol::Sync::ApplyOutcome::Applied;
    if (!result.isCheckpointApplied)
        return result;
    for (std::size_t index = 0; index < fixture.updates.size(); ++index) {
        std::vector<std::uint64_t> refused;
        if (Ubiquitty::Protocol::Sync::applyRowUpdate(terminal, fixture.updates[index], replica, &refused) != Ubiquitty::Protocol::Sync::ApplyOutcome::Applied) {
            result.divergedUpdate = index;
            result.builtRowHashes = std::move(refused);
            return result;
        }
    }
    const auto hashes = replica.rowHashes()[terminal.screens.isUsingAlternateScreen ? 1 : 0];
    result.builtRowHashes.assign(hashes.begin(), hashes.end());
    return result;
}

/// The first row two screens' row hashes disagree on.
///
/// - Parameters:
///   - built: The hashes a replay built.
///   - expected: The hashes the host had.
/// - Returns: The row, or -1 when they agree.
inline int firstMismatchRow(std::span<const std::uint64_t> built, std::span<const std::uint64_t> expected)
{
    for (std::size_t row = 0; row < std::min(built.size(), expected.size()); ++row) {
        if (built[row] != expected[row])
            return static_cast<int>(row);
    }
    return built.size() == expected.size() ? -1 : static_cast<int>(std::min(built.size(), expected.size()));
}

} // namespace UbiquittyTest
