// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// RunTextStore — the text of every OSC 66 proportional run, one entry a run.
//
// A run's text rides its anchor (Docs/proportional-text.md): the anchor cell
// carries the entry's id in the field an ordinary cell spends on a cluster-pool
// id, and its continuation cells carry only the claim. A run is bounded by the
// OSC payload rather than by the cells it claims, so its text does not fit the
// cluster pool's sixteen-code-point entries, and the pool emoji clusters share
// is not where a four-kilobyte run belongs.
//
// Ids are 1-based and fit the cell's cluster field. An id is handed out again
// only after ``retainOnly`` freed it, and its caller builds the reference set
// by walking every place a cell can still carry an id: both screens and every
// scrollback line. So an id never resolves to another run's text for a cell
// that still shows the first. Nothing is deduplicated: two runs with the same
// text are two entries, and freeing one cannot take the other's text with it.

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "Base/FallibleVector.h"
#include "Core/Cell.h"
#include "Core/CellContent.h"

namespace Ubiquitty {

/// The text of every OSC 66 proportional run, one entry a run, named by the
/// id its anchor cell carries.
///
/// A run is bounded by the OSC payload rather than by the cells it claims, so
/// its text does not fit the cluster pool. Ids are 1-based, fit the cell's
/// cluster field, and are handed out again only after ``retainOnly`` freed
/// them, so an id never resolves to another run's text for a cell that still
/// shows the first.
class RunTextStore {
public:
    /// Why a decoded text owner cannot be admitted.
    enum class Error : std::uint8_t {
        OverCapacity = 0,
        AllocationRefused = 1,
        Malformed = 2
    };

    /// The largest id a cell's cluster field carries.
    static constexpr int maxEntries = (1 << (16 - cellCombiningLengthBits)) - 1;

    /// The words a reference set for ``retainOnly`` spans: one bit an id,
    /// id 0 included.
    static constexpr std::size_t referenceWords = (static_cast<std::size_t>(maxEntries) + 64) / 64;

    /// The total code points retained across all runs.
    static constexpr std::size_t maxLiveCodePoints = std::size_t { 1 } << 18;
    /// The live code points below which no sweep is due.
    static constexpr std::size_t sweepFloorCodePoints = maxLiveCodePoints / 4;
    static_assert(maxLiveCodePoints >= runTextMaximumCodePoints);
    static_assert(sweepFloorCodePoints >= runTextMaximumCodePoints);

    /// Stores a run's text under a new id.
    ///
    /// - Parameter codePoints: The run's text.
    /// - Returns: The entry's 1-based id; 0 for empty text, or when every id
    ///   is live or the code-point budget is exhausted.
    [[nodiscard]] std::uint16_t intern(std::span<const std::uint32_t> codePoints)
    {
        if (codePoints.empty() || codePoints.size() > maxLiveCodePoints - m_liveCodePoints
            || (m_freeIDs.empty() && m_entries.size() >= static_cast<std::size_t>(maxEntries)))
            return 0;
        FallibleVector<std::uint32_t> text;
        if (!text.tryAssign(codePoints.begin(), codePoints.end()))
            return 0;
        std::uint16_t id = 0;
        if (!m_freeIDs.empty()) {
            id = m_freeIDs.back();
            m_freeIDs.pop_back();
        } else if (static_cast<int>(m_entries.size()) < maxEntries) {
            if (!m_entries.tryEmplaceBack())
                return 0;
            id = static_cast<std::uint16_t>(m_entries.size());
        } else {
            return 0;
        }
        auto& entry = m_entries[static_cast<std::size_t>(id - 1)];
        entry = std::move(text);
        m_liveCodePoints += entry.size();
        return id;
    }

    /// The text behind an id.
    ///
    /// - Parameter id: A 1-based id.
    /// - Returns: The run's code points; empty for 0, an id never issued, or
    ///   one a sweep freed.
    [[nodiscard]] std::span<const std::uint32_t> text(std::uint16_t id) const noexcept
    {
        if (id == 0 || id > m_entries.size())
            return {};
        return m_entries[static_cast<std::size_t>(id - 1)];
    }

    /// Whether the next placement should sweep before it interns.
    ///
    /// True once the live text has doubled since the last sweep, past a floor,
    /// or once every id is live.
    [[nodiscard]] bool isSweepDue() const noexcept
    {
        const bool isSpent = m_freeIDs.empty() && static_cast<int>(m_entries.size()) >= maxEntries;
        return isSpent || m_liveCodePoints >= m_sweepThreshold;
    }

    /// Frees every entry the reference set does not name.
    ///
    /// - Parameter referenced: One bit an id, set for every id a cell on
    ///   either screen or a scrollback line still carries. A word past the
    ///   span's end reads as unreferenced.
    void retainOnly(std::span<const std::uint64_t> referenced)
    {
        // An entry is only released when its id can remain reachable for reuse.
        if (!m_freeIDs.tryReserve(m_entries.size()))
            return;
        for (std::size_t slot = 0; slot < m_entries.size(); ++slot) {
            auto& entry = m_entries[slot];
            // A live entry is never empty, since intern refuses empty text; an
            // empty one is already on the free list.
            if (entry.empty())
                continue;
            const std::size_t id = slot + 1;
            const std::size_t word = id >> 6;
            if (word < referenced.size() && (referenced[word] & (std::uint64_t { 1 } << (id & 63))) != 0)
                continue;
            m_liveCodePoints -= entry.size();
            FallibleVector<std::uint32_t>().swap(entry);
            m_freeIDs.push_back(static_cast<std::uint16_t>(id));
        }
        m_sweepThreshold = std::max(sweepFloorCodePoints, 2 * m_liveCodePoints);
    }

    /// Adds an entry under the id it was sent with, for a snapshot decode.
    ///
    /// Ids arrive in ascending order; an id skipped over joins the free list,
    /// and an id that is already live keeps the text it has.
    ///
    /// - Parameters:
    ///   - id: The entry's 1-based id.
    ///   - codePoints: The run's text; empty text is ignored.
    void appendDecoded(std::uint16_t id, std::span<const std::uint32_t> codePoints)
    {
        (void)tryAppendDecoded(id, codePoints);
    }

    /// Reserves exact slot and reusable-id capacities before decoded entries arrive.
    /// - Parameter slotCount: The highest id the decoded store will contain.
    /// - Returns: Refusal without changing any entry or id allocation order.
    [[nodiscard]] std::expected<void, Error> reserveDecoded(std::size_t slotCount)
    {
        if (slotCount > static_cast<std::size_t>(maxEntries))
            return std::unexpected(Error::OverCapacity);
        if (!m_entries.tryReserve(slotCount) || !m_freeIDs.tryReserve(slotCount))
            return std::unexpected(Error::AllocationRefused);
        return {};
    }

    /// Copies one decoded entry into fallible, exact-capacity text storage.
    ///
    /// A refused allocation preserves live entries, free ids, and counters.
    /// Pre-reserving all slots keeps their arrays fixed during materialization.
    /// - Parameters:
    ///   - id: The entry's nonzero canonical id.
    ///   - codePoints: The entry's nonempty text.
    /// - Returns: Admission or a typed validation/allocation refusal.
    [[nodiscard]] std::expected<void, Error> tryAppendDecoded(std::uint16_t id, std::span<const std::uint32_t> codePoints)
    {
        if (!id || id > maxEntries || codePoints.empty() || !text(id).empty())
            return std::unexpected(Error::Malformed);
        if (codePoints.size() > maxLiveCodePoints - m_liveCodePoints)
            return std::unexpected(Error::OverCapacity);
        FallibleVector<std::uint32_t> text;
        if (!text.tryAssign(codePoints.begin(), codePoints.end()))
            return std::unexpected(Error::AllocationRefused);
        if (auto reserved = reserveDecoded(id); !reserved)
            return reserved;
        const auto previousSlots = m_entries.size();
        if (!m_entries.tryResize(std::max(previousSlots, static_cast<std::size_t>(id))))
            return std::unexpected(Error::AllocationRefused);
        for (std::size_t slot = previousSlots + 1; slot < id; ++slot) {
            const auto appended = m_freeIDs.tryPushBack(static_cast<std::uint16_t>(slot));
            assert(appended.has_value());
            (void)appended;
        }
        m_freeIDs.erase(std::remove(m_freeIDs.begin(), m_freeIDs.end(), id), m_freeIDs.end());
        m_entries[static_cast<std::size_t>(id - 1)] = std::move(text);
        m_liveCodePoints += codePoints.size();
        return {};
    }

    /// Drops every entry and frees every id.
    void clear() noexcept
    {
        m_entries.clear();
        m_freeIDs.clear();
        m_liveCodePoints = 0;
        m_sweepThreshold = sweepFloorCodePoints;
    }

    /// Every slot in id order, index `i` holding id `i + 1`; a freed slot is
    /// empty. For the snapshot encode.
    [[nodiscard]] std::span<const FallibleVector<std::uint32_t>> slots() const noexcept { return m_entries; }

    /// Reports every currently owned slot, free-id, and text array capacity.
    [[nodiscard]] std::size_t storageBytes() const noexcept
    {
        std::size_t bytes = m_entries.capacity() * sizeof(FallibleVector<std::uint32_t>) + m_freeIDs.capacity() * sizeof(std::uint16_t);
        for (const auto& text : m_entries)
            bytes += text.capacity() * sizeof(std::uint32_t);
        return bytes;
    }

    /// The code points all live entries hold.
    [[nodiscard]] std::size_t liveCodePoints() const noexcept { return m_liveCodePoints; }

    /// Borrows allocator order and the sweep threshold alongside the live slots.
    struct Continuation {
        std::uint16_t slotCount = 0;
        std::span<const std::uint16_t> freeIDs;
        std::size_t sweepThreshold = sweepFloorCodePoints;
    };

    /// Captures allocation order without copying retained text or free IDs.
    /// - Returns: A view valid until the next store mutation.
    [[nodiscard]] Continuation continuation() const noexcept { return { static_cast<std::uint16_t>(m_entries.size()), m_freeIDs, m_sweepThreshold }; }

    /// Restores empty trailing slots and the exact order of reclaimed IDs.
    ///
    /// Live entries must have been decoded first. Input is validated before
    /// mutation; reconstruction retains the store's bounded STL allocations.
    /// - Parameter continuation: Borrowed source counters and every free ID.
    /// - Returns: False for inconsistent counts, references or duplicate IDs.
    [[nodiscard]] bool restoreContinuation(const Continuation& continuation)
    {
        if (continuation.slotCount > maxEntries || continuation.slotCount < m_entries.size()
            || continuation.freeIDs.size() > continuation.slotCount || continuation.sweepThreshold < sweepFloorCodePoints
            || continuation.sweepThreshold > 2 * maxLiveCodePoints)
            return false;
        std::array<std::uint64_t, referenceWords> seen {};
        for (const auto id : continuation.freeIDs) {
            if (!id || id > continuation.slotCount || !text(id).empty())
                return false;
            auto& word = seen[id >> 6];
            const auto bit = std::uint64_t { 1 } << (id & 63);
            if (word & bit)
                return false;
            word |= bit;
        }
        for (std::uint32_t id = 1; id <= continuation.slotCount; ++id) {
            if (text(static_cast<std::uint16_t>(id)).empty() && !(seen[id >> 6] & (std::uint64_t { 1 } << (id & 63))))
                return false;
        }
        FallibleVector<std::uint16_t> freeIDs;
        if (!freeIDs.tryAssign(continuation.freeIDs.begin(), continuation.freeIDs.end()) || !m_entries.tryResize(continuation.slotCount))
            return false;
        m_freeIDs = std::move(freeIDs);
        m_sweepThreshold = continuation.sweepThreshold;
        return true;
    }

private:
    FallibleVector<FallibleVector<std::uint32_t>> m_entries;
    FallibleVector<std::uint16_t> m_freeIDs;
    std::size_t m_liveCodePoints = 0;
    std::size_t m_sweepThreshold = sweepFloorCodePoints;
};

} // namespace Ubiquitty
