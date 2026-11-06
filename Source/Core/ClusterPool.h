// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// ClusterPool — store of grapheme-cluster codepoint sequences too long for a Cell's inline combining
// buffer (long ZWJ emoji families). An overflowing cell interns its sequence here and carries the 1-based
// id in its clusterID; the frontend resolves it at paint.
//
// The pool GROWS to hold every distinct cluster once, and an id names one sequence for the pool's whole
// life: ids are never reused. Cells everywhere hold ids (the screens, raw scrollback, folded scrollback,
// the Sync mirror), so a reused id would make an old cell read a different cluster, which shows and copies
// text the cell never held. At the 15-bit ceiling the pool therefore refuses a NEW sequence: intern returns
// 0 and the writer keeps the cell's inline base and mark, so only the new cell loses its cluster's tail.
// Interning DEDUPS by content, so a sequence the pool already holds keeps its id after the ceiling.
//
// Entries are contiguous for snapshot copies. Bucket chains use indices rather than pointers, so
// storage growth cannot invalidate a link. Interning is O(1) on average; dense IDs resolve in O(1).

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include "Core/VT/Constants.h"

namespace Ubiquitty {

class ClusterPool {
public:
    static constexpr int maxCodePoints = clusterPoolMaximumCodePoints;
    static constexpr int maxEntries = clusterPoolMaxEntries;

    // One interned sequence: 1-based id + length-bounded codepoints. Trivially copyable for the snapshot.
    struct Entry {
        std::uint16_t id;
        std::uint8_t length;
        std::array<std::uint32_t, maxCodePoints> codePoints;
    };

    enum class Error : std::uint8_t {
        OverCapacity = 0,
        AllocationRefused = 1,
    };

    ClusterPool() = default;
    ClusterPool(const ClusterPool&) = delete;
    ClusterPool& operator=(const ClusterPool&) = delete;
    ClusterPool(ClusterPool&& other) noexcept { *this = std::move(other); }
    ClusterPool& operator=(ClusterPool&& other) noexcept
    {
        if (this == &other)
            return *this;
        m_entries = std::move(other.m_entries);
        m_buckets = std::move(other.m_buckets);
        m_index = std::move(other.m_index);
        m_size = std::exchange(other.m_size, 0);
        m_capacity = std::exchange(other.m_capacity, 0);
        m_bucketCount = std::exchange(other.m_bucketCount, 0);
        m_mask = std::exchange(other.m_mask, 0);
        m_nextID = std::exchange(other.m_nextID, 1);
        m_head = std::exchange(other.m_head, 0);
        return *this;
    }

    /// Admits all entry and lookup storage before changing any live owner.
    /// A refused grow preserves entries, identifiers, buckets and replacement order.
    [[nodiscard]] std::expected<void, Error> reserve(std::size_t entryCapacity)
    {
        if (entryCapacity > static_cast<std::size_t>(maxEntries))
            return std::unexpected(Error::OverCapacity);
        if (entryCapacity <= m_capacity)
            return {};
        ClusterPool staged;
        staged.m_entries.reset(new (std::nothrow) Entry[entryCapacity]);
        if (!staged.m_entries)
            return std::unexpected(Error::AllocationRefused);
        staged.m_index.reset(new (std::nothrow) IndexLink[entryCapacity]);
        if (!staged.m_index)
            return std::unexpected(Error::AllocationRefused);
        const auto bucketCount = bucketsForCapacity(entryCapacity);
        if (bucketCount > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(std::uint32_t))
            return std::unexpected(Error::OverCapacity);
        staged.m_buckets.reset(new (std::nothrow) std::uint32_t[bucketCount]);
        if (!staged.m_buckets)
            return std::unexpected(Error::AllocationRefused);
        staged.m_capacity = entryCapacity;
        staged.m_bucketCount = bucketCount;
        staged.m_mask = bucketCount - 1;
        staged.m_nextID = m_nextID;
        staged.m_head = m_head;
        for (std::size_t index = 0; index < bucketCount; ++index)
            staged.m_buckets[index] = emptyBucket;
        for (std::size_t index = 0; index < m_size; ++index) {
            staged.m_entries[index] = m_entries[index];
            staged.indexInsert(m_index[index].hash, static_cast<std::uint32_t>(index));
        }
        staged.m_size = m_size;
        *this = std::move(staged);
        return {};
    }

    /// Measures the actual three arrays, excluding the inline pool object.
    [[nodiscard]] static std::expected<std::size_t, Error> storageBytesForCapacity(std::size_t entryCapacity)
    {
        if (entryCapacity > static_cast<std::size_t>(maxEntries))
            return std::unexpected(Error::OverCapacity);
        return entryCapacity * (sizeof(Entry) + sizeof(IndexLink)) + bucketsForCapacity(entryCapacity) * sizeof(std::uint32_t);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] std::size_t allocationBytes() const noexcept
    {
        return m_capacity * (sizeof(Entry) + sizeof(IndexLink)) + m_bucketCount * sizeof(std::uint32_t);
    }

    // Intern `codePoints` (deduped by content), returning its 1-based id, or 0
    // for an empty sequence, refused storage or a full pool. Refusal never
    // consumes an id, and no id is ever reused. O(1) amortized.
    std::uint16_t intern(std::span<const std::uint32_t> codePoints)
    {
        if (codePoints.empty())
            return 0;
        if (codePoints.size() > static_cast<std::size_t>(maxCodePoints))
            codePoints = codePoints.first(maxCodePoints);

        const std::uint64_t hash = hashOf(codePoints);
        if (const int existing = findEntry(hash, codePoints); existing >= 0)
            return m_entries[existing].id;
        if (isFull())
            return 0;

        // The borrowed sequence may belong to this pool's current backing.
        Entry entry = makeEntry(m_nextID, codePoints);
        if (!reserveAppend())
            return 0;
        const std::uint16_t id = mintID();
        pushEntry(entry, hash);
        return id;
    }

    /// Whether every id has been given out, so a sequence the pool doesn't
    /// already hold is refused.
    [[nodiscard]] bool isFull() const noexcept { return m_size >= static_cast<std::size_t>(maxEntries); }

    // The full sequence behind `id`, or an empty span for id 0 / an unknown id.
    // O(1): ids are dense (slot == id - 1).
    [[nodiscard]] std::span<const std::uint32_t> sequence(std::uint16_t id) const noexcept
    {
        const int slot = slotForID(id);
        if (slot < 0)
            return {};
        const Entry& entry = m_entries[static_cast<std::size_t>(slot)];
        return { entry.codePoints.data(), entry.length };
    }

    void clear() noexcept
    {
        m_entries.reset();
        m_buckets.reset();
        m_index.reset();
        m_size = 0;
        m_capacity = 0;
        m_bucketCount = 0;
        m_mask = 0;
        m_nextID = 1;
        m_head = 0;
    }

    [[nodiscard]] bool empty() const noexcept { return !m_size; }
    [[nodiscard]] int size() const noexcept { return static_cast<int>(m_size); }
    [[nodiscard]] std::span<const Entry> entries() const noexcept { return { m_entries.get(), m_size }; }

    /// The next identity and replacement slot, independent of the lookup cache.
    struct Continuation {
        std::uint16_t nextID = 1;
        std::uint16_t oldestSlot = 0;

        /// Checks counter consistency before allocating decoded entries.
        /// - Parameter entryCount: The complete retained pool size.
        /// - Returns: Whether the counters describe that pool.
        [[nodiscard]] bool fits(std::size_t entryCount) const noexcept
        {
            if (entryCount > static_cast<std::size_t>(maxEntries) || !nextID || nextID > maxEntries || oldestSlot >= maxEntries)
                return false;
            if (entryCount < static_cast<std::size_t>(maxEntries))
                return nextID == entryCount + 1 && !oldestSlot;
            return nextID == oldestSlot + 1;
        }
    };

    /// Captures the allocator after every entry has been captured in slot order.
    /// - Returns: The identity the next distinct cluster will receive.
    [[nodiscard]] Continuation continuation() const noexcept { return { m_nextID, static_cast<std::uint16_t>(m_head) }; }

    /// Restores allocation order after all decoded entries have been installed.
    /// - Parameter continuation: The source pool's captured counters.
    /// - Returns: False without mutation for counters inconsistent with the entries.
    [[nodiscard]] bool restoreContinuation(Continuation continuation) noexcept
    {
        if (!continuation.fits(m_size))
            return false;
        m_nextID = continuation.nextID;
        m_head = continuation.oldestSlot;
        return true;
    }

    /// Appends a decoded wire entry under the id it carries.
    ///
    /// The id has to be the next one: entries are stored in slot order and found
    /// at `id - 1`, so ids that skip or repeat turn every lookup that misses
    /// that guess into a walk of the whole pool. A peer is free to write any
    /// u16 there, and a pool of thousands of entries all bearing one id costs a
    /// scan of all of them per cell per frame — which is why a wire id that is
    /// not the next one is refused rather than stored.
    ///
    /// - Parameters:
    ///   - id: The id the wire gave the entry, which must be size() + 1.
    ///   - codePoints: The sequence, truncated to the per-entry ceiling.
    /// - Returns: False when storage refuses, the id is not next, or the pool is full,
    ///   which is a body the caller cannot honour rather than one entry lost.
    [[nodiscard]] bool appendDecoded(std::uint16_t id, std::span<const std::uint32_t> codePoints)
    {
        if (id == 0 || m_size >= static_cast<std::size_t>(maxEntries))
            return false;
        if (id != static_cast<std::uint16_t>(m_size + 1))
            return false;
        if (codePoints.size() > static_cast<std::size_t>(maxCodePoints))
            codePoints = codePoints.first(maxCodePoints);
        Entry entry = makeEntry(id, codePoints);
        const auto hash = hashOf(codePoints);
        if (!reserveAppend())
            return false;
        if (id >= m_nextID)
            m_nextID = static_cast<std::uint16_t>((id >= maxEntries) ? 1 : (id + 1));
        pushEntry(entry, hash);
        return true;
    }

private:
    /// Links an interned entry to its hash bucket without storing relocatable pointers.
    struct IndexLink {
        std::uint64_t hash;
        std::uint32_t next;
    };
    static constexpr std::uint32_t emptyBucket = 0xFFFFFFFFu;

    static Entry makeEntry(std::uint16_t id, std::span<const std::uint32_t> codePoints) noexcept
    {
        Entry entry {};
        entry.id = id;
        entry.length = static_cast<std::uint8_t>(codePoints.size());
        for (std::size_t i = 0; i < codePoints.size(); ++i)
            entry.codePoints[i] = codePoints[i];
        return entry;
    }

    std::uint16_t mintID() noexcept
    {
        const std::uint16_t id = m_nextID;
        std::uint16_t next = static_cast<std::uint16_t>(m_nextID + 1);
        if (next == 0 || next > maxEntries)
            next = 1;
        m_nextID = next;
        return id;
    }

    /// Reserves geometric live growth before any entry or counter changes.
    [[nodiscard]] std::expected<void, Error> reserveAppend()
    {
        if (m_size < m_capacity)
            return {};
        const auto nextCapacity = !m_capacity                       ? std::size_t { 16 }
            : m_capacity > static_cast<std::size_t>(maxEntries) / 2 ? static_cast<std::size_t>(maxEntries)
                                                                    : m_capacity * 2;
        return reserve(nextCapacity);
    }

    void pushEntry(const Entry& entry, std::uint64_t hash) noexcept
    {
        const auto entryIndex = static_cast<std::uint32_t>(m_size);
        m_entries[m_size++] = entry;
        indexInsert(hash, entryIndex);
    }

    static std::size_t bucketsForCapacity(std::size_t entryCapacity) noexcept
    {
        static_assert(static_cast<std::size_t>(maxEntries) <= std::numeric_limits<std::size_t>::max() / (sizeof(Entry) + sizeof(IndexLink) + 16 * sizeof(std::uint32_t)));
        static_assert(static_cast<std::size_t>(maxEntries) <= std::numeric_limits<std::size_t>::max() / 32);
        if (!entryCapacity)
            return 0;
        std::size_t count = 16;
        while (entryCapacity * 10 > count * 7)
            count *= 2;
        return count;
    }

    // FNV over the codepoints. Collisions verify against stored content, so the hash only needs a fast,
    // order-sensitive spread.
    static std::uint64_t hashOf(std::span<const std::uint32_t> codePoints) noexcept
    {
        std::uint64_t h = 1469598103934665603ull; // FNV offset basis (64-bit)
        for (std::uint32_t value : codePoints) {
            h ^= value;
            h *= 1099511628211ull; // FNV prime (64-bit)
        }
        return h;
    }

    // Entry index for a matching sequence, or -1. Verifies stored content on a hash hit so a collision
    // can't return the wrong cluster.
    [[nodiscard]] int findEntry(std::uint64_t hash, std::span<const std::uint32_t> codePoints) const noexcept
    {
        if (!m_bucketCount)
            return -1;
        std::uint32_t index = m_buckets[hash & m_mask];
        while (index != emptyBucket) {
            const IndexLink& link = m_index[index];
            if (link.hash == hash && matches(m_entries[index], codePoints)) {
                return static_cast<int>(index);
            }
            index = link.next;
        }
        return -1;
    }

    void indexInsert(std::uint64_t hash, std::uint32_t entryIndex) noexcept
    {
        auto& head = m_buckets[hash & m_mask];
        m_index[entryIndex] = { hash, head };
        head = entryIndex;
    }

    // Dense-id → slot, with a linear fallback for a pool restored from a body whose ids are not dense.
    [[nodiscard]] int slotForID(std::uint16_t id) const noexcept
    {
        if (id == 0)
            return -1;
        const int guess = id - 1;
        if (guess < static_cast<int>(m_size) && m_entries[static_cast<std::size_t>(guess)].id == id)
            return guess;
        for (int slot = 0; slot < static_cast<int>(m_size); ++slot)
            if (m_entries[static_cast<std::size_t>(slot)].id == id)
                return slot;
        return -1;
    }

    static bool matches(const Entry& entry, std::span<const std::uint32_t> codePoints) noexcept
    {
        if (entry.length != static_cast<int>(codePoints.size()))
            return false;
        for (int i = 0; i < entry.length; ++i)
            if (entry.codePoints[i] != codePoints[i])
                return false;
        return true;
    }

    std::unique_ptr<Entry[]> m_entries;
    std::unique_ptr<std::uint32_t[]> m_buckets;
    std::unique_ptr<IndexLink[]> m_index;
    std::size_t m_size = 0;
    std::size_t m_capacity = 0;
    std::size_t m_bucketCount = 0;
    std::size_t m_mask = 0;
    std::uint16_t m_nextID = 1; // next id to mint (1..maxEntries, skipping 0)
    int m_head = 0; // a restored body's oldest-slot counter; this pool never advances it
};

static_assert(std::is_trivially_copyable_v<ClusterPool::Entry>, "Entry copies to the render snapshot by memcpy");

} // namespace Ubiquitty
