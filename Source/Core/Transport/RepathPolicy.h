// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// When a carrier that can move between network paths moves (Docs/sync.md
// §6.9, "The client moves off a dead path").
//
// A connection whose path died is not known dead: its packets leave and
// nothing answers. Before it is declared lost the carrier opens a fresh
// socket and migrates, on the first of three signs: the socket reports an
// error, the system says the interface is gone, or data has been outstanding
// for the silence bound with no round trip. The move is tried once. Only when
// the fresh path stays as silent, or fails as the old one did, is the
// connection lost.
//
// The policy is the decision alone. It holds no socket and no clock: the
// carrier reports what happened and when, and asks.

#include <cstdint>
#include <optional>

namespace Ubiquitty::Transport {

/// How long data may stay outstanding with no round trip before a carrier
/// moves to a fresh path, in microseconds. The session layer's measured table
/// holds the same constant (`Sync::LivenessPolicy::repathMicroseconds`,
/// provisional: no recorded dead-path trace).
inline constexpr std::uint64_t repathSilenceMicroseconds = 10'000'000;

/// Why a carrier moves to a fresh path.
enum class RepathReason : std::uint8_t {
    /// A send or a receive on the socket failed.
    SocketError,
    /// The system reported that the interface in use is gone.
    PathRemoved,
    /// Data stayed outstanding for the silence bound with no round trip.
    Silence,
};

/// Decides when a carrier moves to a fresh path, and when the move has
/// failed.
class RepathPolicy {
public:
    /// Creates the policy.
    ///
    /// - Parameter silenceMicroseconds: How long data may stay outstanding
    ///   with no round trip.
    explicit constexpr RepathPolicy(std::uint64_t silenceMicroseconds = repathSilenceMicroseconds)
        : m_silenceMicroseconds(silenceMicroseconds)
    {
    }

    /// Records that data is outstanding: sent and not yet answered. It is
    /// counted from the first report since a round trip, or since nothing
    /// was.
    ///
    /// - Parameter now: The carrier's clock, in microseconds.
    constexpr void noteOutstanding(std::uint64_t now)
    {
        if (!m_outstandingSince)
            m_outstandingSince = now;
    }

    /// Records that nothing is outstanding: all that was sent was answered.
    constexpr void noteNothingOutstanding() { m_outstandingSince.reset(); }

    /// Records a round trip: the peer answered on the path in use. A move
    /// being judged has succeeded, and a sign seen before it is forgotten.
    constexpr void noteRoundTrip()
    {
        m_outstandingSince.reset();
        m_sign.reset();
        m_repathedAt.reset();
    }

    /// Records a send or a receive that failed on the socket. On the fresh
    /// path of a move being judged, the move has failed.
    constexpr void noteSocketError()
    {
        if (m_repathedAt)
            m_hasFailed = true;
        else if (!m_sign)
            m_sign = RepathReason::SocketError;
    }

    /// Records that the system reported the interface in use gone. While a
    /// move is being judged it changes nothing until that move's round trip,
    /// which forgets it: the fresh path is the answer to it.
    constexpr void notePathRemoved()
    {
        if (!m_sign)
            m_sign = RepathReason::PathRemoved;
    }

    /// Says whether the carrier should move now.
    ///
    /// - Parameter now: The carrier's clock, in microseconds.
    /// - Returns: The reason, or `nullopt` while the path shows no sign, and
    ///   while a move is being judged: it is tried once.
    [[nodiscard]] constexpr std::optional<RepathReason> due(std::uint64_t now) const
    {
        if (m_repathedAt || m_hasFailed)
            return std::nullopt;
        if (m_sign)
            return m_sign;
        if (m_outstandingSince && now - *m_outstandingSince >= m_silenceMicroseconds)
            return RepathReason::Silence;
        return std::nullopt;
    }

    /// Records that the carrier moved to a fresh path, which is judged from
    /// now: a round trip is its success.
    ///
    /// - Parameter now: The carrier's clock, in microseconds.
    constexpr void noteRepathed(std::uint64_t now)
    {
        m_repathedAt = now;
        m_outstandingSince.reset();
    }

    /// Records that the carrier could not move at all.
    constexpr void noteRepathImpossible() { m_hasFailed = true; }

    /// Says whether the move failed, so the connection is lost: the fresh
    /// path failed as a socket, or stayed silent for the bound.
    ///
    /// - Parameter now: The carrier's clock, in microseconds.
    [[nodiscard]] constexpr bool hasFailed(std::uint64_t now) const
    {
        return m_hasFailed || (m_repathedAt && now - *m_repathedAt >= m_silenceMicroseconds);
    }

    /// When the policy next has something to say with nothing else
    /// happening, for the carrier's wait.
    ///
    /// - Returns: The time, in microseconds, or `nullopt` when only an event
    ///   can change the answer.
    [[nodiscard]] constexpr std::optional<std::uint64_t> nextDueMicroseconds() const
    {
        if (m_hasFailed)
            return std::nullopt;
        if (m_repathedAt)
            return *m_repathedAt + m_silenceMicroseconds;
        if (m_outstandingSince && !m_sign)
            return *m_outstandingSince + m_silenceMicroseconds;
        return std::nullopt;
    }

private:
    std::uint64_t m_silenceMicroseconds;
    /// When data was first outstanding with no round trip since.
    std::optional<std::uint64_t> m_outstandingSince;
    /// The first sign seen that has not been acted on.
    std::optional<RepathReason> m_sign;
    /// When the carrier moved, while the move is being judged.
    std::optional<std::uint64_t> m_repathedAt;
    bool m_hasFailed = false;
};

} // namespace Ubiquitty::Transport
