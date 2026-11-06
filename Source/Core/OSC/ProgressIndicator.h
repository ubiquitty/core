// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/OSC/ProgressIndicator.h — the OSC 9;4 progress-indicator presentation
// policy shared by every frontend's window/pane/tab chrome.
//
// Core parses OSC 9;4 into a per-Terminal (state, value) pair (Core/TerminalOperations.h,
// Core/OSC/Notify.cpp). Deciding where that pair is shown, and in what color, is the
// same reasoning on every platform — it depends only on the live layout (is the
// window split? tabbed? which pane is focused? is the window active?), never on
// any AppKit / GTK type. A frontend that derives it inline diverges from the
// others on, say, whether an unfocused split pane dims to gray.
//
// So it lives here as pure policy: which slot lights up, how a bar is tinted (as a
// role the frontend maps to a concrete color), and how per-pane progress folds
// into one value for a whole-window slot (an inactive tab's ring). The actual
// rendering — CALayer comet on macOS, overlay quads on Linux — stays per-platform;
// only the decisions are shared. Pure scalars, no platform include, so the host
// test (Tests/Core/test_progressindicator.cpp) links against bare libc++.
//

#include <cstdint>
#include <span>

namespace Ubiquitty::OSC {

/// The OSC 9;4 progress states, as they travel on the wire.
///
// A plain enum, not an enum class, so the
// uint8_t the parser stores and the frontends poll compares against the names
// directly.
//
// This is the only definition. It lived in Core/TerminalOperations.h, which a presentation
// header cannot include without taking the whole terminal model with it, so the
// values were copied here as a second enum and a comment noting they had to
/// match. Two spellings of one wire format, kept in step by hand. Declared here
/// instead: the policy that reads the state and the parser that writes it
/// share a definition, and neither drags the other's dependencies along.
enum ProgressState : uint8_t {
    ProgressNone = 0,
    ProgressNormal = 1, // determinate: value is 0..100 percent
    ProgressError = 2, // attention signal — always red, full bar
    ProgressIndeterminate = 3, // unknown duration — the animated comet
    ProgressPaused = 4, // determinate fill, held
};

/// The colour role a visible progress bar or ring carries.
///
/// The frontend maps each role to a concrete colour: `Accent` to the app accent
/// (the focused pane of the active window), `Inactive` to a dimmed gray (an
/// unfocused split pane, or any bar while the window is backgrounded), `Error`
/// to red — an attention signal, never dimmed.
enum class ProgressTint : uint8_t {
    Accent,
    Inactive,
    Error,
};

// Each of the six below names one axis the visibility and tint rules read.
// They are enums rather than bools because the calls take several at once, and
// `progressTint(state, true, false, true)` cannot be checked by eye at the call
// site — a transposed pair reads as a legitimate call and dims the wrong bar.

/// Whether a pane holds focus within its window.
enum class PaneFocusState : uint8_t {
    Unfocused,
    Focused
};
/// Whether a window is split into several panes.
enum class PaneLayout : uint8_t {
    SinglePane,
    MultiPane
};
/// Whether a window is the active one on screen.
enum class WindowActivityState : uint8_t {
    Background,
    Active
};
/// Whether a window carries more than one tab.
enum class TabLayout : uint8_t {
    SingleTab,
    Tabbed
};
/// Whether a tab is split into several panes.
enum class SplitLayout : uint8_t {
    Unsplit,
    Split
};
/// Whether a tab is the selected one in its group.
enum class TabSelectionState : uint8_t {
    Inactive,
    Active
};

/// One pane's progress, as ``getProgress`` hands it back.
struct ProgressSample {
    uint8_t state = ProgressNone;
    uint8_t value = 0;
};

/// A run of per-pane samples, for the fold below.
///
/// Named rather than spelled at the parameter: an unnamed `std::span` does not
/// cross into Swift, and the shell that folds a split's panes is a Swift one.
using ProgressSampleSpan = std::span<const ProgressSample>;

/// Returns the aggregation precedence of one progress state.
///
/// When several panes report at once an error must always surface, then a
/// spinning comet, then a determinate fill, then a paused one. `ProgressNone`
/// ranks 0 so it never wins.
///
/// Exposed rather than kept inside ``aggregateProgress`` so a frontend ordering
/// its own slots ranks them the same way; two orderings of one precedence
/// disagree only on the frames where it matters.
///
/// - Parameter state: A ``ProgressState`` value, as stored on the wire.
/// - Returns: A rank from 0 to 4, higher winning.
inline int progressRank(uint8_t state)
{
    switch (state) {
    case ProgressError:
        return 4;
    case ProgressIndeterminate:
        return 3;
    case ProgressNormal:
        return 2;
    case ProgressPaused:
        return 1;
    default:
        return 0;
    }
}

/// Returns the tint role one pane's or window's bar should carry.
///
/// `Error` always reads red regardless of focus or window activity — it is an
/// alert, not an accent. Otherwise a bar dims to gray when its window is
/// backgrounded, or when it belongs to an unfocused pane of a split; the
/// focused pane of the active window carries the accent.
///
/// The lone-window content bar and the active tab-button bar are the degenerate
/// single-pane case: pass `Focused` and `SinglePane`, and they track window
/// activity alone.
///
/// - Parameters:
///   - state: A ``ProgressState`` value.
///   - paneFocus: Whether this pane holds focus.
///   - paneLayout: Whether the window is split.
///   - windowActivity: Whether the window is active.
/// - Returns: The role the frontend resolves to a colour.
inline ProgressTint progressTint(uint8_t state, PaneFocusState paneFocus, PaneLayout paneLayout, WindowActivityState windowActivity)
{
    if (state == ProgressError)
        return ProgressTint::Error;
    if (windowActivity == WindowActivityState::Background)
        return ProgressTint::Inactive;
    if (paneLayout == PaneLayout::MultiPane && paneFocus == PaneFocusState::Unfocused)
        return ProgressTint::Inactive;
    return ProgressTint::Accent;
}

/// Whether the window's content-top bar should show progress.
///
/// Only an unsplit, untabbed window with live progress. When split, the
/// per-pane bars carry it; when tabbed, the tab button and ring do — so the
/// three slots partition the cases rather than overlapping.
///
/// - Parameters:
///   - state: A ``ProgressState`` value.
///   - tabLayout: Whether the window carries more than one tab.
///   - splitLayout: Whether the window is split.
/// - Returns: `true` when this slot owns the display.
inline bool contentBarVisible(uint8_t state, TabLayout tabLayout, SplitLayout splitLayout) { return state != ProgressNone && tabLayout == TabLayout::SingleTab && splitLayout == SplitLayout::Unsplit; }

/// Whether the active tab button's bar should show progress.
///
/// The selected tab of a multi-tab group, unsplit, with live progress. A split
/// tab's panes carry their own bars and a lone window uses the content bar.
///
/// - Parameters:
///   - state: A ``ProgressState`` value.
///   - tabSelection: Whether this tab is the selected one.
///   - tabLayout: Whether the window carries more than one tab.
///   - splitLayout: Whether the tab is split.
/// - Returns: `true` when this slot owns the display.
inline bool tabButtonBarVisible(uint8_t state, TabSelectionState tabSelection, TabLayout tabLayout, SplitLayout splitLayout) { return state != ProgressNone && tabSelection == TabSelectionState::Active && tabLayout == TabLayout::Tabbed && splitLayout == SplitLayout::Unsplit; }

/// Whether a pane's own bar should show its progress.
///
/// Only in a split window. An unsplit one shows its single pane's report in
/// the content band or on its tab, and a second bar for the same report is two
/// indicators for one download — which is what makes the four slots a
/// partition rather than a pile.
///
/// - Parameters:
///   - state: A ``ProgressState`` value.
///   - splitLayout: Whether the window is split.
/// - Returns: `true` when this slot owns the display.
inline bool paneBarVisible(uint8_t state, SplitLayout splitLayout) { return state != ProgressNone && splitLayout == SplitLayout::Split; }

/// Folds per-pane progress into the one sample a stand-in slot displays.
///
/// For a slot representing a whole window or tab — an inactive tab's ring,
/// which cannot show the panes' own bars. The kind is the highest-ranked
/// non-`None` pane by ``progressRank``.
///
/// For a determinate result the value is the LEAST-complete determinate pane,
/// so the ring reads done only once every determinate pane does; taking the
/// greatest would report a finished window while a pane still runs. Other kinds
/// carry value 0, and an all-`None` or empty input yields `{ProgressNone, 0}`.
///
/// - Parameter samples: One entry per pane the slot stands in for.
/// - Returns: The single state and value to display.
inline ProgressSample aggregateProgress(ProgressSampleSpan samples)
{
    ProgressSample result;
    bool haveDeterminate = false;
    uint8_t leastComplete = 100;
    for (const ProgressSample& sample : samples) {
        if (sample.state == ProgressNone)
            continue;
        if (progressRank(sample.state) > progressRank(result.state))
            result.state = sample.state;
        if (sample.state == ProgressNormal) {
            haveDeterminate = true;
            if (sample.value < leastComplete)
                leastComplete = sample.value;
        }
    }
    result.value = (result.state == ProgressNormal && haveDeterminate) ? leastComplete : 0;
    return result;
}

} // namespace Ubiquitty::OSC
