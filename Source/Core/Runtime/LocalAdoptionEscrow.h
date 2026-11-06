// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// An unresolved local adoption retains its complete session, not a descriptor
// detached from its model and callback owners. Normal process exit must be
// cancelled while an unresolved record remains; forced process death cannot
// preserve an uncommitted master in any in-process owner.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>

#include "Core/Runtime/TerminalSessionReference.h"

namespace Ubiquitty::Runtime {

/// Releases one reference acquired through the session's shared-reference API.
struct RetainedTerminalSessionDeleter {
    void operator()(TerminalSession* session) const { releaseTerminalSession(session); }
};

using RetainedTerminalSession = std::unique_ptr<TerminalSession, RetainedTerminalSessionDeleter>;

/// Why a bounded session retention request could not be fulfilled.
enum class LocalAdoptionEscrowError : std::uint8_t {
    InvalidOwner,
    Full,
    IdentityExhausted,
    NotFound,
    Unresolved,
};

/// Owns a fixed number of inactive sessions until ownership is proven.
/// All ownership leaves the mutex before a reference can be destroyed.
class LocalAdoptionEscrow {
    friend class TerminalSession;

public:
    static constexpr std::size_t capacity = 16;

    /// Returns the synchronized process owner; it performs no background work.
    [[nodiscard]] static LocalAdoptionEscrow& singleton()
    {
        static LocalAdoptionEscrow escrow;
        return escrow;
    }

private:
    /// Reserves retention before any local I/O is frozen or descriptor is sent.
    /// - Parameter session: The complete owner, acquired before this call.
    /// - Returns: A never-reused identity, or refusal without a retained owner.
    [[nodiscard]] std::expected<std::uint64_t, LocalAdoptionEscrowError> reserve(RetainedTerminalSession session)
    {
        if (!session)
            return std::unexpected(LocalAdoptionEscrowError::InvalidOwner);
        const std::scoped_lock hold(m_mutex);
        if (m_nextID == std::numeric_limits<std::uint64_t>::max())
            return std::unexpected(LocalAdoptionEscrowError::IdentityExhausted);
        for (auto& record : m_records) {
            if (record.owner)
                continue;
            record.id = m_nextID++;
            record.isUnresolved = true;
            record.owner = std::move(session);
            return record.id;
        }
        return std::unexpected(LocalAdoptionEscrowError::Full);
    }

    /// Marks exact Local evidence without dropping the session a missing view left.
    void confirmLocal(std::uint64_t id)
    {
        const std::scoped_lock hold(m_mutex);
        if (auto* record = find(id))
            record->isUnresolved = false;
    }

    /// Claims a protecting owner for one explicit bounded resolution call.
    [[nodiscard]] std::expected<RetainedTerminalSession, LocalAdoptionEscrowError> retain(std::uint64_t id)
    {
        const std::scoped_lock hold(m_mutex);
        auto* record = find(id);
        if (!record)
            return std::unexpected(LocalAdoptionEscrowError::NotFound);
        retainTerminalSession(record->owner.get());
        return RetainedTerminalSession(record->owner.get());
    }

    /// Transfers a proven-local session to an explicit reopen or close owner.
    [[nodiscard]] std::expected<RetainedTerminalSession, LocalAdoptionEscrowError> takeLocal(std::uint64_t id)
    {
        const std::scoped_lock hold(m_mutex);
        auto* record = find(id);
        if (!record)
            return std::unexpected(LocalAdoptionEscrowError::NotFound);
        if (record->isUnresolved)
            return std::unexpected(LocalAdoptionEscrowError::Unresolved);
        return std::move(record->owner);
    }

    /// Transfers retention after pre-send refusal or exact committed evidence.
    /// The caller keeps a protecting reference until its operation returns.
    [[nodiscard]] RetainedTerminalSession release(std::uint64_t id)
    {
        const std::scoped_lock hold(m_mutex);
        auto* record = find(id);
        return record ? std::move(record->owner) : RetainedTerminalSession {};
    }

public:
    /// Counts records that forbid normal application termination.
    [[nodiscard]] std::size_t unresolvedCount() const
    {
        const std::scoped_lock hold(m_mutex);
        std::size_t count = 0;
        for (const auto& record : m_records) {
            if (record.owner && record.isUnresolved)
                ++count;
        }
        return count;
    }

private:
    struct Record {
        std::uint64_t id = 0;
        bool isUnresolved = false;
        RetainedTerminalSession owner;
    };

    [[nodiscard]] Record* find(std::uint64_t id)
    {
        if (id) {
            for (auto& record : m_records) {
                if (record.owner && record.id == id)
                    return &record;
            }
        }
        return nullptr;
    }

    LocalAdoptionEscrow() = default;
    mutable std::mutex m_mutex;
    std::array<Record, capacity> m_records;
    std::uint64_t m_nextID = 1;
};

} // namespace Ubiquitty::Runtime
