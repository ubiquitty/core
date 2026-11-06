// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Runtime/TabStripChrome.h — keeping the grid when the tab strip
// toggles.
//
// A native tab strip appearing takes its height out of the content view, so a
// window that does nothing loses a row of terminal to it, and gets the row back
// when the strip goes. Moving the window's bottom edge down by the strip's
// height keeps every row where it was, which is what a reader notices the
// absence of: opening a second tab must not reflow the text in the first.
//
// The arithmetic is small and the exceptions are what matter, so it is stated
// once here rather than per shell.
//

#include "Base/SwiftInterop.h"
#include "Core/App/TabAccessory.h"
#include "Core/Preferences/Preferences.h"

#include <cstdint>

namespace Ubiquitty::Runtime {

// The accessory rule is the list's as well as the strip's, so it lives with
// the outline (Core/App/TabAccessory.h). The strip's callers name it here.
using App::tabAccessory;
using App::TabAccessory;
using App::TabAccessoryFacts;

/// The heights a window reports when its tab strip appears or goes.
struct TabStripFrameFacts {
    /// The content height the window last settled at.
    double previousContentHeight = 0;
    /// The content height the window reports now.
    double currentContentHeight = 0;
    /// How far the window's bottom edge can travel down before it leaves the
    /// screen's usable area.
    double roomBelow = 0;
    /// How far this window's bottom was last moved down for the strip. Zero
    /// when it was not, and zeroed by a user resize, which hands the size back
    /// to the person holding the mouse.
    double grownAmount = 0;
};

/// The largest height a tab strip is believed to have.
///
/// A backgrounded tab receives no resize while another tab is selected, so its
/// remembered content height can be arbitrarily old: the window that existed
/// before the group did carries a baseline from before tabbing entirely. When
/// AppKit reconciles that window on reselection, one catch-up frame change
/// arrives carrying the whole backgrounded stint's difference, and reading it
/// as a strip height grows the window by hundreds of points.
inline constexpr double maxPlausibleTabStripHeight = 44.0;

/// How far the window's bottom edge moves down as the strip appears.
///
/// The height the strip took, capped by the room below: a window already at the
/// bottom of the screen keeps its frame and gives up the row, because moving it
/// off-screen is worse than reflowing. A difference beyond a plausible strip
/// height is the stale catch-up described above and grows nothing.
///
/// - Parameter facts: What the window reports about the change.
/// - Returns: The distance to move down, zero for no move.
[[nodiscard]] constexpr double tabStripGrowth(const TabStripFrameFacts& facts)
{
    if (facts.previousContentHeight <= 0 || facts.currentContentHeight <= 0)
        return 0;
    const double wanted = facts.previousContentHeight - facts.currentContentHeight;
    if (wanted <= 0 || wanted > maxPlausibleTabStripHeight)
        return 0;
    const double capped = wanted < facts.roomBelow ? wanted : facts.roomBelow;
    return capped > 0 ? capped : 0;
}

/// How far the window's bottom edge moves back up as the strip goes.
///
/// Only what this window grew, and only while it still has the height to give
/// back. A window whose grow was zeroed by a user resize keeps its frame: the
/// person who dragged it owns the size, and taking their height back to undo a
/// move they overrode is the shell arguing with them.
///
/// - Parameters:
///   - facts: What the window reports about the change.
///   - currentWindowHeight: The window's full height, which the move cannot
///     consume.
/// - Returns: The distance to move up, zero for no move.
[[nodiscard]] constexpr double tabStripUndo(const TabStripFrameFacts& facts, double currentWindowHeight)
{
    if (facts.grownAmount <= 0 || currentWindowHeight - facts.grownAmount <= 0)
        return 0;
    return facts.grownAmount;
}

/// What a pane reports when its output lands, for the tab strip's mark.
struct UnseenActivityFacts {
    /// Whether this tab already carries the mark, which is the answer on its own.
    bool isAlreadyMarked = false;
    /// Whether the user is looking at this tab: its window takes keys and it
    /// is the selected tab of its group.
    bool isForeground = false;
    /// Whether a content resize settled recently enough that its prompt
    /// redraw is still arriving.
    bool isWithinResizeSettle = false;
};

/// Whether output arriving now marks this tab as carrying unseen activity.
///
/// A tab the user is looking at has nothing unseen on it, which is the whole
/// of the mark's meaning. A tab already marked decides nothing, so a flood
/// costs one comparison per drain rather than a strip rebuilt per chunk.
///
/// The resize window is the case worth stating. Resizing a window sends
/// SIGWINCH to every pane in it, and a shell answers by redrawing its prompt;
/// a tab that went to the background as part of that same resize would be
/// marked for output the user caused and never read. So output arriving
/// inside the settle window is the resize's echo, not a program's news.
///
/// - Parameter facts: What the pane reports about the arrival.
/// - Returns: Whether to mark the tab.
[[nodiscard]] constexpr bool doesOutputMarkUnseenActivity(const UnseenActivityFacts& facts)
{
    return !facts.isAlreadyMarked && !facts.isForeground && !facts.isWithinResizeSettle;
}

/// How long after a content resize a pane's output is still the resize's own
/// echo rather than news worth marking.
inline constexpr double resizeSettleSuppressionSeconds = 0.5;

/// Whether a bell ringing now leaves a mark for the reader to find later.
///
/// The one thing that matters is whether anybody was there to hear it: a bell
/// in the tab the reader is looking at has already done its whole job on the
/// screen in front of them, and a mark for it would still be sitting there
/// after they had answered it.
///
/// None of `doesOutputMarkUnseenActivity`'s other guards apply. A bell is not
/// idempotent-suppressible the way the activity mark is — re-marking an
/// already-marked tab costs one store and keeps the two paths from disagreeing
/// about what a second bell means — and the resize settle window does not cover
/// it either: a shell redrawing its prompt after SIGWINCH writes cells, it does
/// not ring. A bell inside that window is a program's, so it is news.
///
/// - Parameter isForeground: Whether the reader is looking at this tab.
/// - Returns: Whether to mark the tab.
[[nodiscard]] constexpr bool doesBellMarkUnheard(bool isForeground) // bool-ok: one fact, named where it is passed
{
    return !isForeground;
}

/// Whether the tab-number hints are showing at all.
///
/// Three settings and one fact. `OnCommand` is the interesting one: the hint
/// answers "which number is this tab", a question only asked while reaching for
/// the key, so it appears with the modifier and goes when it is released.
///
/// - Parameters:
///   - mode: What the reader asked for.
///   - isCommandHeld: Whether the tab-selecting modifier is down.
/// - Returns: Whether a tab that has a shortcut should show it.
[[nodiscard]] inline bool areTabShortcutHintsVisible(Preferences::TabShortcutHints mode, bool isCommandHeld) // bool-ok: one fact, named where it is passed
{
    switch (mode) {
    case Preferences::TabShortcutHints::Off:
        return false;
    case Preferences::TabShortcutHints::Always:
        return true;
    case Preferences::TabShortcutHints::OnCommand:
        return isCommandHeld;
    }
    return false;
}

} // namespace Ubiquitty::Runtime
