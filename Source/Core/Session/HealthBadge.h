// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// What a remote-status badge says about a link, from the one state every
// transport reports.
//
// A badge driven by a transport's own phases is one badge per backend, and two
// of them disagree about when a link is worth flagging. It is driven by
// `Health` — the portable axis a daemon session, an ssh shell and a future
// carrier all report — and the two facts decided here are the parts of the
// badge that are policy rather than drawing: whether the link is in its steady
// state, which a badge draws in the chrome's own ink so the indicator that is
// always there is not always shouting, and whether the carrier the bytes ride
// is worth naming, which it is only once they ride one.
//
// The verbs and the tints are not here. They are the frontend's own voice and
// its platform's colour vocabulary, the way `Clipboard::Scope` leaves the words
// of a clipboard badge to the frontend that draws it.

#include "Base/SwiftInterop.h"
#include "Core/Session/Health.h"
#include "Core/Session/RemoteSessionCore.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Session {

/// The link state a connection phase reports.
///
/// `Idle` folds into `Connecting`: a session whose attach has not started is,
/// to the person looking at the window, one that has not connected yet, and a
/// badge has no pre-start state to show. Every frontend folds it the same way,
/// so the fold is here rather than in each of them.
///
/// - Parameter state: The connection phase.
/// - Returns: The link state a badge renders.
[[nodiscard]] constexpr Health healthFor(State state)
{
    switch (state) {
    case State::Connected:
        return Health::Connected;
    case State::Deploying:
        return Health::Deploying;
    case State::Reconnecting:
        return Health::Reconnecting;
    case State::Failed:
        return Health::Failed;
    case State::Idle:
    case State::Connecting:
        break;
    }
    return Health::Connecting;
}

/// What a remote-status badge shows about a link beside the host's name.
struct HealthBadge {
    /// Whether the link is in its steady state, which a badge draws in the
    /// chrome's own ink rather than in a tint that flags it.
    bool isSteady = false;
    /// Whether the badge names the carrier the bytes ride.
    ///
    /// A carrier is a fact about bytes that are flowing: a link still being
    /// stood up has none to name, and one named before it carries anything
    /// would describe the attempt rather than the session.
    bool shouldNameCarrier = false;
};

/// What a badge shows for one link state.
///
/// - Parameter health: The link state.
/// - Returns: The badge's two facts.
[[nodiscard]] constexpr HealthBadge badgeFor(Health health)
{
    const bool isConnected = health == Health::Connected;
    return HealthBadge { isConnected, isConnected };
}

/// Which host a pane's session lives on, as the chrome names it.
///
/// A pane whose bytes come from a ubiquitty-host is a host session wherever
/// that host runs: the daemon on this machine keeps the shell across a quit as
/// a remote one does, so its panes are labelled too — as this machine, which a
/// plain local shell is not. The words are the frontend's ("This Mac"); the
/// place is the fact.
enum class HostPlace : std::uint8_t {
    None = 0, // a plain local shell: nothing to name
    ThisMachine = 1, // the daemon on this machine
    Remote = 2, // a daemon on another machine
};

/// The facts a frontend holds about where a pane's session is hosted.
struct HostFacts {
    /// Whether the pane is a thin client of a ubiquitty-host session, attached
    /// or still being re-attached: a reconnecting pane still lives on its host.
    bool isHostSession = false;
    /// Whether that host is the daemon on this machine.
    bool isOnThisMachine = false;
};

/// The place a pane's session lives on.
///
/// - Parameter facts: What the frontend knows about the pane's session.
/// - Returns: `None` for a plain local shell, else which host.
[[nodiscard]] constexpr HostPlace hostPlaceFor(HostFacts facts)
{
    if (!facts.isHostSession)
        return HostPlace::None;
    return facts.isOnThisMachine ? HostPlace::ThisMachine : HostPlace::Remote;
}

/// What the title row's host chip shows.
struct HostChip {
    /// Whether the chip stands in the title row at all. Every host session
    /// wears one in every link state, the steady one included: the chip is
    /// the window's answer to "which machine is this", not news about the
    /// link.
    bool isShown = false;
    /// Whether the link is steady, drawn in the chrome's own ink (or the
    /// host's tag colour) rather than a state colour.
    bool isSteady = false;
    /// Whether the chip names the carrier. The daemon on this machine is
    /// reached over its own socket, so it has no network carrier to name.
    bool shouldNameCarrier = false;
};

/// What the host chip shows for a pane's place and its link state.
///
/// - Parameters:
///   - place: Where the pane's session lives.
///   - health: The link state.
/// - Returns: The chip's three facts.
[[nodiscard]] constexpr HostChip hostChipFor(HostPlace place, Health health)
{
    if (place == HostPlace::None)
        return {};
    const HealthBadge badge = badgeFor(health);
    return HostChip { true, badge.isSteady, badge.shouldNameCarrier && place == HostPlace::Remote };
}

/// Whether a window's pane headers name each pane's host, gathered one pane at
/// a time.
///
/// A window whose panes all live in one place has said where in its title row,
/// and a header repeating it takes room from every pane to say nothing. Only a
/// window mixing places — panes of two hosts, or a host session beside a plain
/// shell — has headers that need the name to tell the panes apart.
class PaneHostMix {
public:
    /// Counts one pane.
    ///
    /// - Parameters:
    ///   - place: Where the pane's session lives.
    ///   - key: The host's pairing key or label. Read only for a remote host:
    ///     a plain shell and the daemon on this machine are each one place
    ///     whatever they are called.
    void add(HostPlace place, const std::string& key)
    {
        const std::string_view identity = place == HostPlace::Remote ? std::string_view { key } : std::string_view {};
        if (!m_hasPane) {
            m_hasPane = true;
            m_place = place;
            m_key = identity;
            return;
        }
        if (place != m_place || identity != m_key)
            m_isMixed = true;
    }

    /// Whether the panes counted live in more than one place.
    [[nodiscard]] bool shouldNameHost() const { return m_isMixed; }

private:
    bool m_hasPane = false;
    bool m_isMixed = false;
    HostPlace m_place = HostPlace::None;
    std::string m_key;
};

} // namespace Ubiquitty::Session
