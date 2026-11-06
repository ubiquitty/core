// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Envelope.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

/// One owner's memory ceiling, which the owners holding memory under it
/// charge themselves (`MemoryCharge`). A connection's receive side, a client's
/// retained replicas and a host's continuation decoding each have one; the
/// figures are Docs/sync.md §7.9's. All access is on the owner's serialized
/// thread.
class MemoryBudget {
public:
    /// Creates an empty ledger.
    ///
    /// - Parameter limit: The most bytes its charges may hold together; zero
    ///   admits nothing.
    explicit MemoryBudget(std::size_t limit)
        : m_limit(limit)
    {
    }
    MemoryBudget(const MemoryBudget&) = delete;
    MemoryBudget& operator=(const MemoryBudget&) = delete;

    /// Returns the ceiling.
    ///
    /// - Returns: The most bytes the charges may hold together.
    [[nodiscard]] std::size_t limit() const { return m_limit; }
    /// Returns the bytes the live charges hold.
    ///
    /// - Returns: Their sum, which a settled charge may push past the limit.
    [[nodiscard]] std::size_t heldBytes() const { return m_held; }
    /// Returns the room a new charge may take.
    ///
    /// - Returns: Unheld capacity, zero when the charges fill or overfill it.
    [[nodiscard]] std::size_t availableBytes() const { return m_held >= m_limit ? 0 : m_limit - m_held; }

private:
    friend class MemoryCharge;
    std::size_t m_limit;
    std::size_t m_held = 0;
};

/// The bytes one owner holds under a `MemoryBudget`. Growth is admitted
/// before the owner allocates; destruction or a move releases the charge, so
/// it cannot outlive or be counted twice for what it charges.
class MemoryCharge {
public:
    /// Creates an empty charge that holds nothing.
    MemoryCharge() = default;
    /// Creates an empty charge against a ledger.
    ///
    /// - Parameter budget: The ledger it charges; null charges nothing and admits nothing.
    explicit MemoryCharge(std::shared_ptr<MemoryBudget> budget)
        : m_budget(std::move(budget))
    {
    }
    MemoryCharge(const MemoryCharge&) = delete;
    MemoryCharge& operator=(const MemoryCharge&) = delete;
    MemoryCharge(MemoryCharge&& other) noexcept
        : m_budget(std::move(other.m_budget))
        , m_bytes(std::exchange(other.m_bytes, 0))
    {
    }
    MemoryCharge& operator=(MemoryCharge&& other) noexcept
    {
        if (this != &other) {
            settle(0);
            m_budget = std::move(other.m_budget);
            m_bytes = std::exchange(other.m_bytes, 0);
        }
        return *this;
    }
    /// Releases the charge.
    ~MemoryCharge() { settle(0); }

    /// Returns the ledger this charge holds against.
    ///
    /// - Returns: The ledger, or null for a charge with none.
    [[nodiscard]] const std::shared_ptr<MemoryBudget>& budget() const { return m_budget; }
    /// Returns the bytes this charge holds.
    ///
    /// - Returns: The charge.
    [[nodiscard]] std::size_t bytes() const { return m_bytes; }

    /// Moves the charge to `bytes` when the ledger has room for the growth;
    /// shrinking always succeeds. Call before allocating what grows.
    ///
    /// - Parameter bytes: The whole charge wanted.
    /// - Returns: False, with the charge unchanged, when growth does not fit
    ///   or the charge has no ledger.
    [[nodiscard]] bool resize(std::size_t bytes)
    {
        if (bytes <= m_bytes) {
            settle(bytes);
            return true;
        }
        if (!m_budget || bytes - m_bytes > m_budget->availableBytes())
            return false;
        settle(bytes);
        return true;
    }

    /// Records bytes another owner already holds and this charge mirrors,
    /// without admission: a value that was admitted when it was allocated,
    /// or one the ledger must see even though it was not. The ledger may then
    /// hold more than its limit, and admits nothing new until it does not.
    ///
    /// - Parameter bytes: The whole charge held.
    void settle(std::size_t bytes)
    {
        if (!m_budget) {
            m_bytes = 0;
            return;
        }
        m_budget->m_held = m_budget->m_held - m_bytes + bytes;
        m_bytes = bytes;
    }

private:
    std::shared_ptr<MemoryBudget> m_budget;
    std::size_t m_bytes = 0;
};

/// Retained source storage under an existing outbound limit. The connection
/// and the daemon each own a budget; reservations keep both alive after a
/// connection is removed. All access is on the owner's serialized thread.
/// This is not the scheduler's materialized-bulk allowance and introduces no
/// default cap: the owner supplies its existing outbound high-water values.
class BulkSourceBudget {
public:
    /// Creates an empty ledger bounded by its owner's outbound allowance.
    ///
    /// - Parameter limit: Maximum bytes held by this ledger and other outbound owners.
    explicit BulkSourceBudget(std::size_t limit)
        : m_limit(limit)
    {
    }
    BulkSourceBudget(const BulkSourceBudget&) = delete;
    BulkSourceBudget& operator=(const BulkSourceBudget&) = delete;

    /// Returns the bytes reserved by live immutable sources.
    ///
    /// - Returns: The full retained charge, including sent prefixes.
    [[nodiscard]] std::size_t heldBytes() const { return m_held; }

    /// Room after retained sources and every other live outbound owner. The
    /// caller includes transport queues, interactive/run owners and output
    /// scratch in `otherBytes`, not merely their unsent logical suffixes.
    ///
    /// - Parameter otherBytes: Storage held outside this source ledger.
    /// - Returns: Unreserved capacity, or zero if existing storage exceeds the limit.
    [[nodiscard]] std::size_t availableBytes(std::size_t otherBytes) const
    {
        return otherBytes > m_limit || m_held > m_limit - otherBytes ? 0 : m_limit - otherBytes - m_held;
    }

private:
    friend class BulkSourceReservation;
    std::size_t m_limit;
    std::size_t m_held = 0;
};

/// The exact two outbound ledgers and their other held storage at admission.
/// Both budgets must be distinct and non-null. The caller must use these same
/// retained charges in later output admission/enforcement, not reserve once
/// and then let unrelated output grow into the reserved room.
struct BulkSourceAdmission {
    std::shared_ptr<BulkSourceBudget> connection;
    std::shared_ptr<BulkSourceBudget> aggregate;
    std::size_t otherConnectionBytes = 0;
    std::size_t otherAggregateBytes = 0;
};

/// A pre-allocation charge. Moving it moves the obligation; destruction
/// releases it even when a source outlives its connection's ordinary owner.
class BulkSourceReservation {
public:
    BulkSourceReservation(const BulkSourceReservation&) = delete;
    BulkSourceReservation& operator=(const BulkSourceReservation&) = delete;
    BulkSourceReservation(BulkSourceReservation&& other) noexcept
        : m_connection(std::move(other.m_connection))
        , m_aggregate(std::move(other.m_aggregate))
        , m_bytes(std::exchange(other.m_bytes, 0))
    {
    }
    BulkSourceReservation& operator=(BulkSourceReservation&& other) noexcept
    {
        if (this != &other) {
            release();
            m_connection = std::move(other.m_connection);
            m_aggregate = std::move(other.m_aggregate);
            m_bytes = std::exchange(other.m_bytes, 0);
        }
        return *this;
    }
    /// Releases the remaining charge in both ledgers.
    ~BulkSourceReservation() { release(); }

    /// Reserves in both ledgers before any source-sized allocation. Failure
    /// changes neither ledger, including when only the aggregate is full.
    ///
    /// - Parameters:
    ///   - admission: The two ledgers and other outbound storage at admission.
    ///   - bytes: The complete charge requested before allocating source storage.
    /// - Returns: The reservation, or no value without changing either ledger.
    [[nodiscard]] static std::optional<BulkSourceReservation> acquire(const BulkSourceAdmission& admission, std::size_t bytes)
    {
        if (!bytes || !admission.connection || !admission.aggregate || admission.connection == admission.aggregate
            || bytes > admission.connection->availableBytes(admission.otherConnectionBytes)
            || bytes > admission.aggregate->availableBytes(admission.otherAggregateBytes))
            return std::nullopt;
        admission.connection->m_held += bytes;
        admission.aggregate->m_held += bytes;
        return BulkSourceReservation(admission.connection, admission.aggregate, bytes);
    }

    /// Returns the charge held by this reservation.
    ///
    /// - Returns: Reserved bytes, or zero after moving or releasing the charge.
    [[nodiscard]] std::size_t bytes() const { return m_bytes; }

    /// Gives back the part of the charge an owner turned out not to need.
    ///
    /// - Parameter bytes: The whole charge kept, at least one byte and at most
    ///   the charge held; anything else leaves the charge unchanged.
    void shrink(std::size_t bytes)
    {
        if (!bytes || bytes >= m_bytes)
            return;
        m_connection->m_held -= m_bytes - bytes;
        m_aggregate->m_held -= m_bytes - bytes;
        m_bytes = bytes;
    }

private:
    BulkSourceReservation(std::shared_ptr<BulkSourceBudget> connection, std::shared_ptr<BulkSourceBudget> aggregate, std::size_t bytes)
        : m_connection(std::move(connection))
        , m_aggregate(std::move(aggregate))
        , m_bytes(bytes)
    {
    }
    /// Removes this reservation's charge exactly once from both ledgers.
    void release()
    {
        if (!m_bytes)
            return;
        m_connection->m_held -= m_bytes;
        m_aggregate->m_held -= m_bytes;
        m_bytes = 0;
    }

    std::shared_ptr<BulkSourceBudget> m_connection;
    std::shared_ptr<BulkSourceBudget> m_aggregate;
    std::size_t m_bytes = 0;
};

/// A chunk's encoding scratch and destination frame may coexist with the
/// source. Reserving both maximum frames also covers its small queued head.
/// The destination becomes the carrier's charged owner before another source
/// can be admitted; the scheduler must not accumulate frames in its output.
inline constexpr std::size_t bulkSourceFrameReserveBytes = 2 * (maxEnvelopeHeaderBytes + maxFrameBodyBytes);

/// Immutable bytes lent to the scheduler without a second payload vector.
/// A concrete source reserves its allocation, object and frame scratch before
/// allocating. Its bytes outlive every borrowed chunk; derived destruction
/// frees them before this base releases the reservation. `bytes` is a pure,
/// non-reentrant query and returns the same span throughout the lifetime.
class BulkSource {
public:
    virtual ~BulkSource() = default;
    BulkSource(const BulkSource&) = delete;
    BulkSource& operator=(const BulkSource&) = delete;

    /// Borrows immutable source bytes without allocating or reentering the owner.
    ///
    /// - Returns: The same stable span until this source is destroyed.
    [[nodiscard]] virtual std::span<const std::uint8_t> bytes() const = 0;
    /// Returns the complete storage charge retained for this source.
    ///
    /// - Returns: Pixel, object and frame-scratch reservation bytes.
    [[nodiscard]] std::size_t retainedBytes() const { return m_reservation.bytes(); }

protected:
    explicit BulkSource(BulkSourceReservation reservation)
        : m_reservation(std::move(reservation))
    {
    }

private:
    BulkSourceReservation m_reservation;
};

} // namespace Ubiquitty::Protocol::Sync
