// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/App/StatusRollup.h — what a pane, a tab, a window and a
// workspace say about themselves, from one ordering.
//
// A pane's program reports in several vocabularies: a status record (OSC 7501,
// which is also how an agent says it is working or waiting on someone), a
// progress report (OSC 9;4), an exit status (OSC 133;D), whether it sits at a
// prompt, a bell, a notification, an indicator colour (OSC 21337), a tab colour
// and a badge (iTerm2). A listing shows one mark per node, and a node above a
// pane shows its most urgent child. Each vocabulary already orders itself
// (`ProgramState` by its enum, progress by `progressRank`); this is the one
// place they are ordered against each other, so a tab strip, a session tree
// and a status line cannot disagree about which pane is the one to look at.
//
// Pure: plain values in, plain values out, no terminal and no platform.
// `App::paneStatusFacts` (ProgramActivity.h) reads the facts off a Terminal.
//

#include "Core/Color.h"
#include "Core/OSC/Activity.h"
#include "Core/OSC/ProgressIndicator.h"
#include "Core/Themes/StatusColors.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace Ubiquitty::App {

/// What a node is doing, from least to most urgent.
///
/// The order is the roll-up's: a node above a pane takes the greatest kind
/// among its children. Read from the top, it answers "which of these wants a
/// person first".
///
/// - `Failed`: something went wrong and is still showing: a status record in
///   `Error`, a progress report in its error state, or a prompt whose last
///   command exited non-zero.
/// - `Blocked`: a program is waiting on a person (a permission, a question, a
///   credential). Under `Failed` because a failure is over and a block is not,
///   which is the order `ProgramState` already gives the two.
/// - `Attention`: a bell rang or a notification was posted and nobody has seen
///   it. Over `Done` because it asked for someone by name and a finished job
///   did not; under `Blocked` because a block says what it needs.
/// - `Done`: a program reported that it finished.
/// - `Working`: a program reported work in progress, by a status record or a
///   progress report (a paused one included: it is held, not over).
/// - `Running`: a program holds the terminal and has reported nothing. Apart
///   from `Working` so an editor left open is not drawn as a job in flight.
/// - `Idle`: a shell at its prompt, or a program that reported itself idle.
/// - `None`: no pane at all.
enum class StatusKind : std::uint8_t {
    None,
    Idle,
    Running,
    Working,
    Done,
    Attention,
    Blocked,
    Failed,
};

/// Who holds a pane's terminal: the shell at its prompt, or a program it started.
enum class TerminalHolder : std::uint8_t {
    Program,
    Prompt,
};

/// One pane's status facts, as its terminal and its host hold them.
struct PaneStatusFacts {
    /// The primary status record's state (`ProgramStatusStore::primary`), or
    /// `None` without a record. An agent's status is such a record.
    OSC::ProgramState programState = OSC::ProgramState::None;
    /// What a `Blocked` record waits for.
    OSC::ProgramBlockKind blockKind = OSC::ProgramBlockKind::None;
    /// The OSC 9;4 report.
    OSC::ProgressSample progress;
    /// The last command's exit status, absent when none was reported.
    std::optional<int> exitStatus;
    /// Whether a shell, rather than a program it started, holds the terminal.
    bool isAtPrompt = false;
    /// A bell nobody has seen.
    bool hasUnseenBell = false;
    /// A notification nobody has seen.
    bool hasUnseenNotification = false;
    /// The colour the program chose for its indicator (OSC 21337).
    std::optional<ColorRGBA> indicatorColor;
    /// The colour the program asked its tab to take (iTerm2).
    std::optional<ColorRGBA> tabColor;
    /// Whether the program set a badge (iTerm2).
    bool hasBadge = false;
};

/// What one node shows: a pane's own status, or its children's rolled up.
struct RolledStatus {
    /// The most urgent kind at or under the node.
    StatusKind kind = StatusKind::None;
    /// How many panes at or under the node are of that kind; zero only for `None`.
    std::uint32_t count = 0;
    /// How many panes are at or under the node.
    std::uint32_t paneCount = 0;
    /// What the first most-urgent pane waits for, when `kind` is `Blocked`.
    OSC::ProgramBlockKind blockKind = OSC::ProgramBlockKind::None;
    /// The panes' progress as one sample (`OSC::aggregateProgress`).
    OSC::ProgressSample progress;
    /// Whether any pane has a bell nobody has seen.
    bool hasUnseenBell = false;
    /// Whether any pane has a notification nobody has seen.
    bool hasUnseenNotification = false;
    /// The indicator colour of the first most-urgent pane that chose one.
    std::optional<ColorRGBA> indicatorColor;
    /// The tab colour of the first most-urgent pane that asked for one.
    std::optional<ColorRGBA> tabColor;
    /// How many panes carry a badge.
    std::uint32_t badgeCount = 0;
};

/// The theme slot a kind is drawn in.
///
/// - Parameter kind: What a node is doing.
/// - Returns: The slot whose colour marks it; the neutral one for a node with
///   nothing to report.
[[nodiscard]] constexpr Themes::StatusColorSlot statusColorSlot(StatusKind kind)
{
    switch (kind) {
    case StatusKind::Failed:
        return Themes::StatusColorSlot::Failed;
    case StatusKind::Blocked:
        return Themes::StatusColorSlot::Blocked;
    case StatusKind::Attention:
        return Themes::StatusColorSlot::Attention;
    case StatusKind::Done:
        return Themes::StatusColorSlot::Done;
    case StatusKind::Working:
        return Themes::StatusColorSlot::Working;
    case StatusKind::Running:
    case StatusKind::Idle:
    case StatusKind::None:
        break;
    }
    return Themes::StatusColorSlot::Neutral;
}

/// A status mark a frontend draws outside the grid whose colour is not the
/// status kind's own: a progress bar in its three tints, and the dot that says
/// something asked for the reader.
enum class StatusMark : std::uint8_t {
    /// A progress bar or ring carrying the accent.
    Progress,
    /// A progress bar dimmed: its window is in the background, or it belongs
    /// to an unfocused pane of a split.
    ProgressInactive,
    /// A progress bar reporting an error.
    ProgressError,
    /// The dot left by a notification nobody has seen.
    Notification,
};

/// Which of these marks a platform has a colour of its own for.
///
/// A fact the frontend reports, not a choice it makes: macOS has an accent, a
/// dimmed grey, an alert red and a system blue; a platform without one leaves
/// it false and the theme's colour is drawn.
struct PlatformStatusColors {
    bool hasProgress = false; // bool-ok: names the one fact it reports
    bool hasProgressInactive = false; // bool-ok: names the one fact it reports
    bool hasProgressError = false; // bool-ok: names the one fact it reports
    bool hasNotification = false; // bool-ok: names the one fact it reports
};

/// What a status mark is drawn in.
struct StatusMarkInk {
    /// Whether the platform's own colour for the mark is the one to draw.
    bool isPlatformColor = false; // bool-ok: names the one condition it reports
    /// The theme's colour for the mark; what is drawn when `isPlatformColor`
    /// is false.
    ColorRGBA color {};
};

/// The theme slot a status mark belongs to.
///
/// - Parameter mark: The mark.
/// - Returns: Its slot.
[[nodiscard]] constexpr Themes::StatusColorSlot statusMarkSlot(StatusMark mark)
{
    switch (mark) {
    case StatusMark::Progress:
        return Themes::StatusColorSlot::Progress;
    case StatusMark::ProgressInactive:
        return Themes::StatusColorSlot::Neutral;
    case StatusMark::ProgressError:
        return Themes::StatusColorSlot::Failed;
    case StatusMark::Notification:
        return Themes::StatusColorSlot::Attention;
    }
    return Themes::StatusColorSlot::Neutral;
}

/// The progress a pane shows: its own OSC 9;4 report, or with none, a
/// program's working status as progress: the percent its record carries, or
/// with none, progress of unknown length.
///
/// A program that says it is working is saying what a spinner says, or with a
/// percent what a bar says, so the slots that carry progress, a tab's mark,
/// the selected tab's bar and the content band, carry it too. A report of the
/// pane's own, determinate or not, is the more exact of the two and wins.
///
/// - Parameters:
///   - reported: The pane's OSC 9;4 state and value.
///   - programStatusKind: The pane's program status, as a ``StatusKind``.
///   - programProgress: A working record's percent, or -1 for none.
/// - Returns: The sample the progress slots show.
[[nodiscard]] constexpr OSC::ProgressSample progressWithProgramStatus(OSC::ProgressSample reported, std::uint8_t programStatusKind, std::int8_t programProgress)
{
    if (reported.state != OSC::ProgressNone || programStatusKind != std::to_underlying(StatusKind::Working))
        return reported;
    if (programProgress >= 0 && programProgress <= 100)
        return { .state = OSC::ProgressNormal, .value = static_cast<std::uint8_t>(programProgress) };
    return { .state = OSC::ProgressIndeterminate, .value = 0 };
}

/// The mark a progress tint role is drawn as.
///
/// - Parameter tint: The role ``OSC::progressTint`` ranked.
/// - Returns: The mark.
[[nodiscard]] constexpr StatusMark progressStatusMark(OSC::ProgressTint tint)
{
    switch (tint) {
    case OSC::ProgressTint::Error:
        return StatusMark::ProgressError;
    case OSC::ProgressTint::Inactive:
        return StatusMark::ProgressInactive;
    case OSC::ProgressTint::Accent:
        break;
    }
    return StatusMark::Progress;
}

/// Decides what a status mark is drawn in.
///
/// A colour the theme names for the mark's slot always wins: a theme that says
/// what failed looks like means it everywhere. Where the theme names none, a
/// platform that has a colour of its own for the mark draws that, and one
/// that has none draws the theme's derived colour.
///
/// - Parameters:
///   - mark: The mark.
///   - colors: The pane's theme's status colours.
///   - platform: Which marks the platform has its own colour for.
/// - Returns: The platform's colour or the theme's.
[[nodiscard]] constexpr StatusMarkInk statusMarkInk(StatusMark mark, const Themes::StatusColors& colors, const PlatformStatusColors& platform)
{
    const Themes::StatusColorSlot slot = statusMarkSlot(mark);
    bool doesPlatformHaveOne = false;
    switch (mark) {
    case StatusMark::Progress:
        doesPlatformHaveOne = platform.hasProgress;
        break;
    case StatusMark::ProgressInactive:
        doesPlatformHaveOne = platform.hasProgressInactive;
        break;
    case StatusMark::ProgressError:
        doesPlatformHaveOne = platform.hasProgressError;
        break;
    case StatusMark::Notification:
        doesPlatformHaveOne = platform.hasNotification;
        break;
    }
    return { .isPlatformColor = doesPlatformHaveOne && !colors.isNamed(slot), .color = colors.color(slot) };
}

/// A run of rolled statuses, named so it crosses into Swift.
using RolledStatusSpan = std::span<const RolledStatus>;

/// The kind one pane's facts amount to.
///
/// - Parameter facts: The pane's facts.
/// - Returns: The most urgent kind any of them supports; `Idle` or `Running`
///   when none says more.
[[nodiscard]] constexpr StatusKind paneStatusKind(const PaneStatusFacts& facts)
{
    const bool didLastCommandFail = facts.isAtPrompt && facts.exitStatus && *facts.exitStatus != 0;
    if (facts.programState == OSC::ProgramState::Error || facts.progress.state == OSC::ProgressError || didLastCommandFail)
        return StatusKind::Failed;
    if (facts.programState == OSC::ProgramState::Blocked)
        return StatusKind::Blocked;
    if (facts.hasUnseenBell || facts.hasUnseenNotification)
        return StatusKind::Attention;
    if (facts.programState == OSC::ProgramState::Done)
        return StatusKind::Done;
    if (facts.programState == OSC::ProgramState::Working || facts.progress.state != OSC::ProgressNone)
        return StatusKind::Working;
    // A program that calls itself idle is idle whoever holds the terminal.
    if (facts.programState == OSC::ProgramState::Idle || facts.isAtPrompt)
        return StatusKind::Idle;
    return StatusKind::Running;
}

/// One pane's status as a node.
///
/// - Parameter facts: The pane's facts.
/// - Returns: The leaf the roll-up folds.
[[nodiscard]] constexpr RolledStatus paneStatus(const PaneStatusFacts& facts)
{
    RolledStatus status;
    status.kind = paneStatusKind(facts);
    status.count = 1;
    status.paneCount = 1;
    status.blockKind = status.kind == StatusKind::Blocked ? facts.blockKind : OSC::ProgramBlockKind::None;
    status.progress = facts.progress;
    status.hasUnseenBell = facts.hasUnseenBell;
    status.hasUnseenNotification = facts.hasUnseenNotification;
    status.indicatorColor = facts.indicatorColor;
    status.tabColor = facts.tabColor;
    status.badgeCount = facts.hasBadge ? 1 : 0;
    return status;
}

/// Rolls a node's children up into the node.
///
/// A tab from its panes, a window from its tabs, a workspace from its windows:
/// the fold is the same at every level and gives the same answer however the
/// panes are grouped, so a parent never has to reach past its children.
///
/// The kind is the greatest among the children and the count is how many panes
/// share it. The colours are those of the first child of that kind that has
/// one, in the order given, which is the order the node lists its children.
/// Progress is `OSC::aggregateProgress` over the children's samples, so this
/// and a frontend folding a split's panes rank progress the same way.
///
/// - Parameter children: The node's children, already rolled, in display order.
/// - Returns: The node's status; all clear for no children.
[[nodiscard]] inline RolledStatus rollUpStatus(RolledStatusSpan children)
{
    RolledStatus node;
    for (const RolledStatus& child : children) {
        if (child.kind > node.kind) {
            node.kind = child.kind;
            node.count = 0;
            node.blockKind = OSC::ProgramBlockKind::None;
            node.indicatorColor.reset();
            node.tabColor.reset();
        }
        if (child.kind == node.kind && child.kind != StatusKind::None) {
            node.count += child.count;
            if (node.blockKind == OSC::ProgramBlockKind::None)
                node.blockKind = child.blockKind;
            if (!node.indicatorColor)
                node.indicatorColor = child.indicatorColor;
            if (!node.tabColor)
                node.tabColor = child.tabColor;
        }
        node.paneCount += child.paneCount;
        node.hasUnseenBell = node.hasUnseenBell || child.hasUnseenBell;
        node.hasUnseenNotification = node.hasUnseenNotification || child.hasUnseenNotification;
        node.badgeCount += child.badgeCount;
        // Folded pairwise: the aggregate of an aggregate and one more sample is
        // the aggregate of them all, since a sample's value counts only while
        // its state is the determinate one.
        const std::array<OSC::ProgressSample, 2> pair { node.progress, child.progress };
        node.progress = OSC::aggregateProgress(pair);
    }
    return node;
}

} // namespace Ubiquitty::App
