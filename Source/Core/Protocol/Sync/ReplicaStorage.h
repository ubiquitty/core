// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/GridBounds.h"
#include "Core/Pen.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

enum class ReplicaStorageError : std::uint8_t {
    OverCapacity,
    AllocationRefused,
};

class ReplicaStorageBuilder;

/// Immutable dictionaries and hashes retained by live and saved replicas.
/// Copies retain the same owners without allocating; only an unpublished
/// builder can mutate the arrays. Borrowed spans last until the final retaining
/// handle is released. Each allocation is charged once, not once per handle.
class ReplicaStorage {
public:
    ReplicaStorage() = default;
    ReplicaStorage(const ReplicaStorage& other) noexcept
        : m_data(retain(other.m_data.get()))
    {
    }
    ReplicaStorage& operator=(const ReplicaStorage& other) noexcept
    {
        if (this != &other)
            m_data.reset(retain(other.m_data.get()));
        return *this;
    }
    ReplicaStorage(ReplicaStorage&&) noexcept = default;
    ReplicaStorage& operator=(ReplicaStorage&&) noexcept = default;

    [[nodiscard]] std::span<const Pen> pens() const
    {
        return m_data ? std::span<const Pen>(m_data->pens.get(), m_data->penCount) : std::span<const Pen> {};
    }

    [[nodiscard]] std::array<std::span<const std::uint64_t>, 2> rowHashes() const
    {
        if (!m_data)
            return {};
        return { std::span<const std::uint64_t>(m_data->rows[0].get(), m_data->rowCounts[0]),
            std::span<const std::uint64_t>(m_data->rows[1].get(), m_data->rowCounts[1]) };
    }

    /// Requested header and array capacities, excluding allocator bookkeeping.
    [[nodiscard]] std::size_t allocationBytes() const { return m_data ? m_data->allocationBytes : 0; }
    [[nodiscard]] std::size_t penCapacity() const { return m_data ? m_data->penCapacity : 0; }

    friend bool operator==(const ReplicaStorage& a, const ReplicaStorage& b)
    {
        const auto first = a.pens();
        const auto second = b.pens();
        if (first.size() != second.size() || (!first.empty() && std::memcmp(first.data(), second.data(), first.size_bytes())))
            return false;
        const auto firstRows = a.rowHashes();
        const auto secondRows = b.rowHashes();
        return std::ranges::equal(firstRows[0], secondRows[0]) && std::ranges::equal(firstRows[1], secondRows[1]);
    }

private:
    friend class ReplicaStorageBuilder;

    struct Data {
        mutable std::atomic<std::size_t> references { 1 };
        std::unique_ptr<Pen[]> pens;
        std::array<std::unique_ptr<std::uint64_t[]>, 2> rows;
        std::size_t penCount = 0;
        std::size_t penCapacity = 0;
        std::array<std::size_t, 2> rowCounts {};
        std::size_t allocationBytes = 0;
    };

    struct Release {
        void operator()(const Data* data) const noexcept
        {
            if (!data)
                return;
            // The last handle cannot be retained concurrently through another
            // handle. Ordinary frame replacement therefore needs no RMW;
            // only genuinely shared snapshots use the decrement below.
            if (data->references.load(std::memory_order_acquire) == 1 || data->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
                std::default_delete<const Data> {}(data);
        }
    };

    static const Data* retain(const Data* data) noexcept
    {
        // A live handle occupies addressable storage, so there cannot be
        // size_t-max simultaneous retaining handles.
        if (data)
            data->references.fetch_add(1, std::memory_order_relaxed);
        return data;
    }

    explicit ReplicaStorage(std::unique_ptr<Data> data) noexcept
        : m_data(data.release())
    {
    }

    std::unique_ptr<const Data, Release> m_data;
};

/// Sole mutable owner until publication. Every borrow is invalidated by a
/// growing append or publication; a published array is never mutated again.
class ReplicaStorageBuilder {
public:
    ReplicaStorageBuilder() = default;
    ReplicaStorageBuilder(ReplicaStorageBuilder&&) noexcept = default;
    ReplicaStorageBuilder& operator=(ReplicaStorageBuilder&&) noexcept = default;
    ReplicaStorageBuilder(const ReplicaStorageBuilder&) = delete;
    ReplicaStorageBuilder& operator=(const ReplicaStorageBuilder&) = delete;

    /// The most pens a dictionary holds: the wire's 32-bit dictionary count,
    /// or fewer where an array of that many would exceed the largest object.
    static constexpr std::size_t maxPenCapacity = std::min<std::size_t>(std::numeric_limits<std::uint32_t>::max(),
        static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(Pen));
    /// The most row hashes a screen holds.
    static constexpr std::size_t maxRowCount = static_cast<std::size_t>(maxRows);

    [[nodiscard]] static std::expected<std::size_t, ReplicaStorageError> storageBytesForCapacity(std::size_t penCapacity, std::array<std::size_t, 2> rowCounts)
    {
        static_assert(maxPenCapacity <= (std::numeric_limits<std::size_t>::max() - sizeof(ReplicaStorage::Data) - 2 * maxRowCount * sizeof(std::uint64_t)) / sizeof(Pen));
        std::size_t bytes = sizeof(ReplicaStorage::Data);
        if (penCapacity > maxPenCapacity)
            return std::unexpected(ReplicaStorageError::OverCapacity);
        bytes += penCapacity * sizeof(Pen);
        for (const auto count : rowCounts) {
            if (count > maxRowCount)
                return std::unexpected(ReplicaStorageError::OverCapacity);
            bytes += count * sizeof(std::uint64_t);
        }
        return bytes;
    }

    /// Creates the final owners at their exact initial capacities.
    [[nodiscard]] static std::expected<ReplicaStorageBuilder, ReplicaStorageError> create(std::size_t penCount, std::array<std::size_t, 2> rowCounts, std::size_t maxPens)
    {
        // Checked before any allocation, and again where each count sizes an
        // array so the bound is visible there.
        if (penCount > maxPens || penCount > maxPenCapacity || rowCounts[0] > maxRowCount || rowCounts[1] > maxRowCount)
            return std::unexpected(ReplicaStorageError::OverCapacity);
        const auto bytes = storageBytesForCapacity(penCount, rowCounts);
        if (!bytes)
            return std::unexpected(bytes.error());
        ReplicaStorageBuilder result;
        result.m_data.reset(new (std::nothrow) ReplicaStorage::Data);
        if (!result.m_data)
            return std::unexpected(ReplicaStorageError::AllocationRefused);
        auto& data = *result.m_data;
        data.penCount = penCount;
        data.penCapacity = penCount;
        data.rowCounts = rowCounts;
        data.allocationBytes = *bytes;
        result.m_maxPens = std::min(maxPens, maxPenCapacity);
        if (penCount) {
            data.pens.reset(new (std::nothrow) Pen[penCount] {});
            if (!data.pens)
                return std::unexpected(ReplicaStorageError::AllocationRefused);
        }
        for (std::size_t screen = 0; screen < 2; ++screen) {
            const auto rowCount = rowCounts[screen];
            if (!rowCount)
                continue;
            if (rowCount > maxRowCount)
                return std::unexpected(ReplicaStorageError::OverCapacity);
            data.rows[screen].reset(new (std::nothrow) std::uint64_t[rowCount] {});
            if (!data.rows[screen])
                return std::unexpected(ReplicaStorageError::AllocationRefused);
        }
        return result;
    }

    [[nodiscard]] std::span<Pen> pens() const
    {
        return m_data ? std::span<Pen>(m_data->pens.get(), m_data->penCount) : std::span<Pen> {};
    }

    [[nodiscard]] std::array<std::span<std::uint64_t>, 2> rowHashes() const
    {
        if (!m_data)
            return {};
        return { std::span<std::uint64_t>(m_data->rows[0].get(), m_data->rowCounts[0]),
            std::span<std::uint64_t>(m_data->rows[1].get(), m_data->rowCounts[1]) };
    }

    /// Appends within the configured dictionary bound, preserving the owner
    /// and its count if allocation is refused. Growth overlap is the previous
    /// allocation plus the reported new pen capacity.
    [[nodiscard]] std::expected<void, ReplicaStorageError> appendPen(const Pen& pen)
    {
        assert(m_data);
        auto& data = *m_data;
        if (data.penCount >= m_maxPens)
            return std::unexpected(ReplicaStorageError::OverCapacity);
        const Pen value = pen;
        if (data.penCount == data.penCapacity) {
            const auto capacity = data.penCapacity > m_maxPens / 2 ? m_maxPens : std::max(std::size_t { 1 }, 2 * data.penCapacity);
            const auto bytes = storageBytesForCapacity(capacity, data.rowCounts);
            if (!bytes)
                return std::unexpected(bytes.error());
            std::unique_ptr<Pen[]> pens(new (std::nothrow) Pen[capacity] {});
            if (!pens)
                return std::unexpected(ReplicaStorageError::AllocationRefused);
            std::ranges::copy(this->pens(), pens.get());
            data.pens = std::move(pens);
            data.penCapacity = capacity;
            data.allocationBytes = *bytes;
        }
        data.pens[data.penCount++] = value;
        return {};
    }

    /// Seals and transfers the final owners without allocation or copying.
    [[nodiscard]] ReplicaStorage publish() && noexcept { return ReplicaStorage(std::move(m_data)); }

private:
    std::unique_ptr<ReplicaStorage::Data> m_data;
    std::size_t m_maxPens = 0;
};

} // namespace Ubiquitty::Protocol::Sync
