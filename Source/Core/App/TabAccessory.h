// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// TabAccessory.h — the one slot a tab has for what it has to say, and the
// rule that ranks what fills it.
//
// A tab in the strip and a row in a host's outline say the same things about
// what runs under them: a job's progress, a bell, the colour its program
// chose, a notification. Both draw one mark for them, chosen here, so the
// list and the strip never differ. The program's colour (OSC 21337) is not
// a second mark: it is the ink of this one, a dot when nothing louder claims
// the slot and the bell's ink when a bell did.
//
// The names stay in `Runtime`, where the tab strip's shells read them; the
// file is here so the outline, which sits below `Runtime`, can state the
// same answer on its rows.
//

#include "Base/SwiftInterop.h"

#include <cstdint>
#include <utility>

namespace Ubiquitty::App {

/// What a tab's accessory slot shows. One slot, so the things a tab has to
/// say are ranked rather than drawn over each other.
enum class TabAccessory : std::uint8_t {
    /// Nothing to say, or the reader is already looking at it.
    None UBIQUITTY_SWIFT_NAME(none),
    /// A job is reporting how far along it is.
    ProgressRing UBIQUITTY_SWIFT_NAME(progressRing),
    /// A bell rang here while the reader was somewhere else.
    Bell UBIQUITTY_SWIFT_NAME(bell),
    /// A bell rang here on a tab whose program also set a status colour, so
    /// the bell is drawn in that colour rather than the colour being lost.
    ColouredBell UBIQUITTY_SWIFT_NAME(colouredBell),
    /// The program set a status colour for this tab.
    StatusDot UBIQUITTY_SWIFT_NAME(statusDot),
    /// Something asked for the reader by name.
    NotificationDot UBIQUITTY_SWIFT_NAME(notificationDot),
    /// Output arrived that nobody has looked at.
    ActivityDots UBIQUITTY_SWIFT_NAME(activityDots),
    /// The reader is holding the modifier that selects tabs by number.
    ShortcutHint UBIQUITTY_SWIFT_NAME(shortcutHint),
};

// The Swift and web mirrors of an outline row pin these numbers.
static_assert(std::to_underlying(TabAccessory::None) == 0 && std::to_underlying(TabAccessory::ProgressRing) == 1 && std::to_underlying(TabAccessory::Bell) == 2 && std::to_underlying(TabAccessory::ColouredBell) == 3);
static_assert(std::to_underlying(TabAccessory::StatusDot) == 4 && std::to_underlying(TabAccessory::NotificationDot) == 5 && std::to_underlying(TabAccessory::ActivityDots) == 6 && std::to_underlying(TabAccessory::ShortcutHint) == 7);

/// What one tab has to say about itself, for the slot to rank.
struct TabAccessoryFacts {
    bool isSelected; // the reader is looking at this tab
    bool hasProgress; // a pane is reporting progress
    bool hasStatusColour; // the program set a status colour
    bool hasUnheardBell; // a bell rang while the reader was elsewhere
    bool hasUnseenNotification; // something asked for the reader
    bool hasUnseenActivity; // output arrived unread
    bool isShortcutHintHeld; // the tab-selecting modifier is down
    bool hasShortcut; // this position answers to one at all
    bool isShortcutHintOff; // the reader turned the shortcut hint off, so the slot carries status in its place
};

/// Ranks what a tab's one accessory slot shows.
///
/// The attention marks come first, ordered by how much each wants attention: a
/// progress ring says how long, a bell asked for the reader out loud, a status
/// colour says what the program chose to say, a notification asked for the
/// reader by name, and unread output is the weakest claim of the five. A
/// selected tab shows none of them — the reader is looking at it, so there is
/// nothing left to draw them to it for.
///
/// The bell ranks under the ring rather than over it, which is the one place
/// this order departs from how loud each claim is. A ring is redrawn with every
/// report while the job runs; the bell is a latch that waits. Ranked over the ring it would
/// take the slot for the whole rest of a job the moment that job rang once —
/// `make` warning at its third file costs the next ten minutes of progress —
/// and it loses nothing by waiting, because the ring goes when the job ends,
/// which is exactly when a bell it rang wants reading.
///
/// A bell and a status colour do not compete: `ColouredBell` is drawn in the
/// colour, so ranking the bell over the dot drops nothing.
///
/// The hint is what a tab shows when it has nothing else to say. It ranks LAST
/// rather than first: its setting can leave it on permanently, and a hint that
/// outranked the marks would then mean no tab could ever report anything. A
/// number is recoverable by counting along the strip; unread output is not.
///
/// A reader who turned the shortcut hint off has given the slot nothing to
/// fall back to, so it carries the tab's status instead, on the selected tab
/// as on the rest: the status dot shows whenever the program set a colour,
/// and a bell rang while the reader was elsewhere keeps its mark on the
/// selected tab too, until the reader comes to it — a selected tab in a window
/// that was not key when the bell rang. Two marks stay out of that slot. The
/// ring does not show on the selected tab, whose progress the horizontal bar
/// in its pill already draws, and the unread-output ellipsis does not show at
/// all: with no number to give way to it would sit on nearly every tab that
/// prints anything, and say nothing the status dot does not.
///
/// - Parameter facts: What this tab has to say.
/// - Returns: The one thing the slot shows.
[[nodiscard]] inline TabAccessory tabAccessory(const TabAccessoryFacts& facts)
{
    if (facts.isShortcutHintOff) {
        if (facts.hasProgress && !facts.isSelected)
            return TabAccessory::ProgressRing;
        if (facts.hasUnheardBell)
            return facts.hasStatusColour ? TabAccessory::ColouredBell : TabAccessory::Bell;
        if (facts.hasStatusColour)
            return TabAccessory::StatusDot;
        if (facts.hasUnseenNotification && !facts.isSelected)
            return TabAccessory::NotificationDot;
        return TabAccessory::None;
    }
    if (!facts.isSelected) {
        if (facts.hasProgress)
            return TabAccessory::ProgressRing;
        if (facts.hasUnheardBell)
            return facts.hasStatusColour ? TabAccessory::ColouredBell : TabAccessory::Bell;
        if (facts.hasStatusColour)
            return TabAccessory::StatusDot;
        if (facts.hasUnseenNotification)
            return TabAccessory::NotificationDot;
        if (facts.hasUnseenActivity)
            return TabAccessory::ActivityDots;
    }
    if (facts.isShortcutHintHeld && facts.hasShortcut)
        return TabAccessory::ShortcutHint;
    return TabAccessory::None;
}

} // namespace Ubiquitty::App
