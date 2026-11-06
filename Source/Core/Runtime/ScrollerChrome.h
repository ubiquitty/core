// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Color.h"
#include "Core/Runtime/RuntimeLink.h"

namespace Ubiquitty::Runtime {

/// What a session's scroller shows, for a shell that draws one.
///
/// Every fact here is read off the published snapshot rather than the live
/// model, because a scroller is chrome: it is sized, tinted and shown on the
/// main thread, between drains, where the model lock is the wrong thing to
/// take.
struct ScrollerChrome {
    /// The history the bar may scroll through.
    int scrollbackLines = 0;
    /// Whether the knob must be drawn light.
    bool prefersLightKnob = false;
    /// Whether the application asks for a bar at all.
    ///
    /// False is rxvt's DEC private mode ?30 reset. It is not an extent: the
    /// history behind the bar is untouched and comes back with the bar.
    bool isBarVisible = true;
};

/// How far this session's view may scroll back, from the published snapshot.
///
/// The snapshot carries two counts and only one of them is a limit.
/// `scrollbackCount` is the storage fact, the rows the ring retains, and it
/// stays true on the alternate screen, where the primary's history is still
/// held and none of it is reachable. `history.retainedRows` is what
/// `Screen::scrollableRowCount` answered, the gate applied once in Core, and
/// that is the number a clamp wants.
///
/// Named rather than left as a field read because the field is spelled
/// `retainedRows`, which reads like the storage question a caller must not
/// ask here: a frontend reaching for `scrollbackCount` instead lets a wheel
/// notch on the alternate screen walk the viewport into history that is not
/// behind it, while the bar sized by ``scrollerChrome`` shows none of it.
///
/// - Parameter snapshot: The session's published snapshot.
/// - Returns: The greatest reachable view offset, 0 on the alternate screen.
[[nodiscard]] constexpr int viewOffsetLimit(const RuntimeSnapshot& snapshot) { return snapshot.history.retainedRows; }

/// Derives what a scroller shows from a session's published snapshot.
///
/// Three policies, each of which a shell gets wrong by having its own opinion.
///
/// The extent is ``viewOffsetLimit`` and nothing else. The bar's range and the
/// range the view may reach are one number, and a caller supplying its own
/// count is how they came apart: the alternate screen has no history while
/// the snapshot carries the primary screen's raw count regardless, so a bar
/// sized from that count drags a full-screen program's view through rows
/// that are not behind it. This is what keeps vim and less from growing a
/// draggable scrollbar over a screen that cannot scroll.
///
/// The knob's tint follows the terminal's own ground rather than the system
/// appearance, because an overlay scroller floats over the grid: a dark theme
/// in a light window, or the reverse, gets an invisible knob from any answer
/// the window can give. The background read is the EFFECTIVE one — DECSCNM's
/// whole-frame swap already applied — so a program that inverts the screen
/// gets a knob against what it actually painted.
///
/// The third is the application's. rxvt's DEC private mode ?30 takes the bar
/// away outright, and a shell that reads the mode for itself answers at its
/// own moment — one arm inside the parser, the other on the next drain.
/// Answered here, off the published snapshot, both arms hide the same bar at
/// the same point, and a hard reset that restores the mode restores the bar
/// with no second path to remember it.
///
/// - Parameter snapshot: The session's published snapshot.
/// - Returns: The extent, the tint, and whether the application wants a bar.
[[nodiscard]] inline ScrollerChrome scrollerChrome(const RuntimeSnapshot& snapshot)
{
    ScrollerChrome chrome;
    chrome.scrollbackLines = viewOffsetLimit(snapshot);
    const double lightness = Ubiquitty::perceivedLightness<double>(static_cast<double>(snapshot.defaultBackgroundRed),
        static_cast<double>(snapshot.defaultBackgroundGreen), static_cast<double>(snapshot.defaultBackgroundBlue));
    chrome.prefersLightKnob = lightness < 0.5;
    chrome.isBarVisible = snapshot.modes.isScrollbarVisible;
    return chrome;
}

} // namespace Ubiquitty::Runtime
