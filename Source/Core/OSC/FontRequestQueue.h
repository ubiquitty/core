// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Compiler.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::OSC {

/// Identifies one application attempt without exposing a reusable queue index.
///
/// Copies share identity, not authority to consume twice. Keeping an old receipt
/// alive prevents its identity from aliasing a new attempt, even after a reset.
class FontRequestReceipt {
public:
    [[nodiscard]] bool isValid() const { return m_identity != nullptr; }

private:
    friend class FontRequestQueue;
    struct Identity { };
    std::shared_ptr<const Identity> m_identity;
};

/// Names where an operation's size comes from.
enum class UBIQUITTY_CLOSED_ENUM FontSizeSource : std::uint8_t {
    /// The operation's own axes: an absolute size, a relative bump, or neither.
    Requested,
    /// The configured size, the family drawn kept: a reset of the size alone.
    Configured,
};

/// Owns the raw axes of one accepted font operation.
struct FontRequestOperation {
    std::string family;
    double sizePoints { 0.0 };
    double sizeDelta { 0.0 };
    FontSizeSource sizeSource { FontSizeSource::Requested };
};

/// Carries one operation independently of parser-owned storage.
struct FontRequestLease {
    FontRequestReceipt receipt;
    FontRequestOperation operation;
};

enum class UBIQUITTY_CLOSED_ENUM FontAdoptionOutcome : std::uint8_t {
    Applied,
    Refused,
};

/// Preserves accepted font operations until their frontend reports an outcome.
///
/// The owner holds the model lock for every access. Only one head can be in
/// flight; later operations resolve against actual, acknowledged font state.
class FontRequestQueue {
public:
    /// Stable state after every accepted operation has received an outcome.
    struct Continuation {
        std::string_view actualFamily;
        double actualSizePoints = 0;
        std::uint64_t generation = 0;
        std::uint64_t completedGeneration = 0;
        std::uint64_t queryGeneration = 0;
    };

    // The window-operation queue admits eight requests per terminal. Font
    // operations use the same bound, including the outstanding application.
    static constexpr std::size_t maxPendingRequests = 8;

    FontRequestQueue() = default;
    FontRequestQueue(const FontRequestQueue&) = delete;
    FontRequestQueue& operator=(const FontRequestQueue&) = delete;
    FontRequestQueue(FontRequestQueue&&) = default;
    FontRequestQueue& operator=(FontRequestQueue&&) = default;

    /// Refuses overflow before allocating or changing arrival/query state.
    [[nodiscard]] bool append(std::string_view family, double sizePoints, double sizeDelta)
    {
        if (!std::isfinite(sizePoints) || !std::isfinite(sizeDelta))
            return false;
        return admit({ std::string(family), sizePoints, sizeDelta, FontSizeSource::Requested });
    }

    /// Queues a return to the configured size that keeps the family drawn.
    ///
    /// Refuses overflow as ``append`` does, and leaves the accepted query
    /// family alone: the family does not change.
    [[nodiscard]] bool appendConfiguredSize() { return admit({ {}, 0.0, 0.0, FontSizeSource::Configured }); }

    /// Associates the accepted query family with the latest admitted operation.
    void noteAcceptedQueryFamily() { m_queryGeneration = m_generation; }

    [[nodiscard]] std::uint64_t generation() const { return m_generation; }
    [[nodiscard]] std::size_t pendingCount() const { return m_requests.size(); }
    [[nodiscard]] bool canBegin() const { return !m_requests.empty() && !m_inFlight.isValid(); }
    [[nodiscard]] bool canPublishActualFamily() const { return m_completedGeneration >= m_queryGeneration; }
    [[nodiscard]] const std::string& actualFamily() const { return m_actualFamily; }
    [[nodiscard]] double actualSizePoints() const { return m_actualSizePoints; }

    /// Borrows stable state without transferring a frontend application lease.
    /// - Returns: Nothing while any operation or lease remains outstanding.
    [[nodiscard]] std::optional<Continuation> continuation() const
    {
        if (!m_requests.empty() || m_inFlight.isValid())
            return std::nullopt;
        return Continuation { m_actualFamily, m_actualSizePoints, m_generation, m_completedGeneration, m_queryGeneration };
    }

    /// Restores stable state into an idle queue without minting a receipt.
    /// - Parameter state: An admitted continuation whose borrowed family remains alive during the call.
    /// - Returns: False without mutation for inconsistent counters or font facts.
    [[nodiscard]] bool restoreContinuation(const Continuation& state)
    {
        if (!m_requests.empty() || m_inFlight.isValid() || state.completedGeneration != state.generation
            || state.queryGeneration > state.generation
            || (state.actualFamily.empty() ? state.actualSizePoints != 0.0 : !isActualFontValid(state.actualFamily, state.actualSizePoints)))
            return false;
        m_actualFamily.assign(state.actualFamily);
        m_actualSizePoints = state.actualSizePoints;
        m_generation = state.generation;
        m_completedGeneration = state.completedGeneration;
        m_queryGeneration = state.queryGeneration;
        return true;
    }

    /// Leases the oldest operation for the runtime's resolution policy.
    [[nodiscard]] FontRequestLease begin()
    {
        if (!canBegin()) {
            return {};
        }
        m_inFlight.m_identity = std::make_shared<const FontRequestReceipt::Identity>();
        return { m_inFlight, m_requests.front().operation };
    }

    /// Consumes exactly the leased head; stale or duplicate receipts change nothing.
    [[nodiscard]] bool acknowledge(const FontRequestReceipt& receipt, FontAdoptionOutcome outcome,
        std::string_view actualFamily, double actualSizePoints)
    {
        if (!receipt.isValid() || receipt.m_identity != m_inFlight.m_identity || m_requests.empty()) {
            return false;
        }
        if (outcome == FontAdoptionOutcome::Applied) {
            if (!isActualFontValid(actualFamily, actualSizePoints)) {
                return false;
            }
            m_actualFamily.assign(actualFamily);
            m_actualSizePoints = actualSizePoints;
        }
        m_completedGeneration = m_requests.front().generation;
        m_requests.erase(m_requests.begin());
        m_inFlight = {};
        return true;
    }

    /// Publishes an external zoom/configuration change and invalidates any lease.
    ///
    /// The pending operation stays queued and must be resolved against this new
    /// actual state. A late acknowledgment cannot overwrite the external change.
    [[nodiscard]] bool publishActualFont(std::string_view family, double sizePoints)
    {
        if (!isActualFontValid(family, sizePoints)) {
            return false;
        }
        m_actualFamily.assign(family);
        m_actualSizePoints = sizePoints;
        m_inFlight = {};
        return true;
    }

private:
    struct Operation {
        FontRequestOperation operation;
        std::uint64_t generation;
    };

    [[nodiscard]] bool admit(FontRequestOperation&& operation)
    {
        if (m_requests.size() >= maxPendingRequests || m_generation == std::numeric_limits<std::uint64_t>::max())
            return false;
        m_requests.push_back({ std::move(operation), m_generation + 1 });
        ++m_generation;
        return true;
    }

    static bool isActualFontValid(std::string_view family, double sizePoints)
    {
        return !family.empty() && std::isfinite(sizePoints) && sizePoints > 0.0;
    }

    std::vector<Operation> m_requests;
    FontRequestReceipt m_inFlight;
    std::string m_actualFamily;
    double m_actualSizePoints { 0.0 };
    std::uint64_t m_generation { 0 };
    std::uint64_t m_completedGeneration { 0 };
    std::uint64_t m_queryGeneration { 0 };
};

} // namespace Ubiquitty::OSC
