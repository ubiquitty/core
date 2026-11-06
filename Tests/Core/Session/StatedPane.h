// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// StatedPane.h — a pane as a session-tree test states it.
//
// A tree is built from the two records a host sends for a pane. A test about
// a tree's shape, its roll-up or a listing says what a pane shows — its title
// inputs, its status as the roll-up reads it, when it was last active — and
// this turns that into the records a host would have sent for such a pane.
// The tree then reads them back through its one reader, so a statement here
// that the records cannot carry fails the test that made it.
//

#include "Core/App/StatusRollup.h"
#include "Core/App/TitleCompose.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Session/PaneActivity.h"
#include "Core/Session/SessionTree.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace UbiquittyTest {

/// What a test says about one pane.
struct StatedPane {
    /// The session drawing into the pane; zero when none does.
    std::uint32_t sessionID = 0;
    /// The pane as the screen plane names it, and its Structural generation.
    Ubiquitty::Protocol::Sync::ID128 paneID;
    std::uint64_t structuralGeneration = 0;
    /// The host's half of the pane's title inputs. What a layout decides
    /// (`paneLabel`, `tabTitle`, `windowTitle`) and what a record derives
    /// (`programStatus`) are not a host's to state and are not carried.
    Ubiquitty::App::PaneTitleFacts title;
    /// The pane's status as the roll-up reads it. `hasBadge` follows
    /// `badgeText`.
    Ubiquitty::App::PaneStatusFacts status;
    Ubiquitty::Session::PaneActivity activity;
    std::string badgeText;
    std::string program;
    std::uint64_t createdUnixSeconds = 0;
    bool isAdopted = false;
    std::vector<Ubiquitty::Session::TreeAttendee> attendees;
    /// The words of the newest notification, read while
    /// `status.hasUnseenNotification` is set.
    std::string notificationTitle;
    std::string notificationBody;

    /// The records a host would send for the pane, as a tree is built from.
    ///
    /// - Returns: The pane's facts for `buildSessionTree`.
    [[nodiscard]] Ubiquitty::Session::TreePaneFacts treeFacts() const
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::Session::TreePaneFacts out;
        out.sessionID = sessionID;
        out.paneID = paneID;
        out.structuralGeneration = structuralGeneration;
        out.attendees = attendees;
        PaneFacts& facts = out.facts;
        facts.programTitle = title.programTitle;
        facts.currentDirectory = title.currentDirectory;
        facts.homeDirectory = title.homeDirectory;
        facts.processChain = title.processChain;
        facts.processChainWithArguments = title.processChainWithArguments;
        facts.shellCommandName = title.shellCommandName;
        facts.terminalName = title.ttyName;
        facts.hostName = title.hostName;
        facts.columns = static_cast<std::uint16_t>(std::clamp(title.columns, 0, 0xFFFF));
        facts.rows = static_cast<std::uint16_t>(std::clamp(title.rows, 0, 0xFFFF));
        facts.end = { .didEnd = title.end.didEnd, .hasStatus = title.end.hasStatus, .wasSignalled = title.end.wasSignalled, .code = title.end.code };
        facts.foreground.name = program;
        facts.isAtPrompt = status.isAtPrompt;
        facts.progress = { .state = status.progress.state, .value = status.progress.value };
        if (status.indicatorColor)
            facts.indicator = IndicatorFacts { .red = status.indicatorColor->red, .green = status.indicatorColor->green, .blue = status.indicatorColor->blue, .label = {} };
        if (status.tabColor)
            facts.tabColor = TabColorFacts { .red = status.tabColor->red, .green = status.tabColor->green, .blue = status.tabColor->blue };
        facts.badgeText = badgeText;
        if (status.exitStatus)
            facts.exitStatus = *status.exitStatus;
        facts.activity = { .lastOutputUnixMilliseconds = activity.lastOutputUnixMilliseconds, .lastInputUnixMilliseconds = activity.lastInputUnixMilliseconds };
        facts.createdUnixSeconds = createdUnixSeconds;
        facts.isAdopted = isAdopted;
        if (status.programState != Ubiquitty::OSC::ProgramState::None) {
            Ubiquitty::OSC::ProgramStatus record;
            record.state = status.programState;
            record.kind = status.blockKind;
            out.records.store.records.push_back(std::move(record));
        }
        out.facts.attention.hasUnseenBell = status.hasUnseenBell;
        out.facts.attention.hasUnseenNotification = status.hasUnseenNotification;
        if (status.hasUnseenNotification) {
            out.facts.attention.notificationTitle = notificationTitle;
            out.facts.attention.notificationBody = notificationBody;
        }
        return out;
    }

    /// Lets a test's pane answer `buildSessionTree` where a host's facts are
    /// asked for.
    operator Ubiquitty::Session::TreePaneFacts() const { return treeFacts(); } // NOLINT(google-explicit-constructor)
};

/// Compares two optional colours channel by channel.
///
/// - Parameters:
///   - a: One colour.
///   - b: The other.
/// - Returns: Whether both are absent or both are the same colour.
[[nodiscard]] inline bool sameColor(const std::optional<Ubiquitty::ColorRGBA>& a, const std::optional<Ubiquitty::ColorRGBA>& b)
{
    if (!a || !b)
        return !a && !b;
    return a->red == b->red && a->green == b->green && a->blue == b->blue && a->alpha == b->alpha;
}

/// Compares two rolled statuses member by member.
///
/// - Parameters:
///   - a: One status.
///   - b: The other.
/// - Returns: Whether they are the same status.
[[nodiscard]] inline bool sameStatus(const Ubiquitty::App::RolledStatus& a, const Ubiquitty::App::RolledStatus& b)
{
    return a.kind == b.kind && a.count == b.count && a.paneCount == b.paneCount && a.blockKind == b.blockKind && a.progress.state == b.progress.state
        && a.progress.value == b.progress.value && a.hasUnseenBell == b.hasUnseenBell && a.hasUnseenNotification == b.hasUnseenNotification
        && sameColor(a.indicatorColor, b.indicatorColor) && sameColor(a.tabColor, b.tabColor) && a.badgeCount == b.badgeCount;
}

} // namespace UbiquittyTest
