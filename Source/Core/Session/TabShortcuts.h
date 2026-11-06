// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The ⌘1..⌘9 tab keyboard-shortcut policy: which 1-based positions get a numbered
// shortcut (producer), and which tab a pressed shortcut selects (consumer),
// including the ⌘9 "wrap to last tab" rule for >9 tabs. Producer and consumer must
// agree on the rule, so it lives here once. Pure integer logic, no platform types.

namespace Ubiquitty::Session {

// The highest 1-based tab position that gets a numbered shortcut. ⌘1..⌘9 cover
// positions 1..9; ⌘9 additionally serves as "the last tab" once a window holds
// more than nine tabs (the consumer's wrap rule below).
inline constexpr int maxTabShortcutPosition = 9;

// PRODUCER. Whether 1-based `position` gets its own ⌘<position> shortcut menu
// item. Positions 1..9 do; everything past nine is reachable only by selecting
// ⌘9 on a window with more than nine tabs (which jumps to the last tab).
inline bool tabPositionHasShortcut(int position) { return position >= 1 && position <= maxTabShortcutPosition; }

// CONSUMER. The 0-based tab index a pressed ⌘<position> shortcut selects, given the
// window's tab count. A position within range → that tab; PAST the last tab →
// clamp to the last tab; ⌘9 stays "last tab" even with >9 tabs. Returns -1 only
// when there is no tab to select (no tabs, or non-positive position).
inline int tabIndexForShortcut(int position, int tabCount)
{
    if (tabCount <= 0 || position < 1)
        return -1;
    // ⌘9 is the dedicated "last tab" shortcut once a window exceeds nine tabs.
    if (position == maxTabShortcutPosition && tabCount > maxTabShortcutPosition)
        return tabCount - 1;
    // Within range → that tab; past the last tab → clamp to the last tab.
    return position <= tabCount ? position - 1 : tabCount - 1;
}

/// LABEL. The shortcut position a tab answers to, or none.
///
/// The inverse of ``tabIndexForShortcut``, and here for the same reason that
/// one is: a tab showing "⌘3" while ⌘3 selects a different tab is the pair
/// disagreeing. Past nine tabs only the last keeps a label, and it is ⌘9 —
/// every position from the ninth to the second-to-last is reachable by no
/// shortcut at all, so labelling one would name a key that does nothing.
///
/// - Parameters:
///   - index: The 0-based tab position.
///   - tabCount: How many tabs the window holds.
/// - Returns: The 1-based shortcut position, or 0 when that tab has none.
inline int tabShortcutPositionForIndex(int index, int tabCount)
{
    if (index < 0 || index >= tabCount)
        return 0;
    const int position = index + 1;
    if (tabCount > maxTabShortcutPosition)
        return index == tabCount - 1 ? maxTabShortcutPosition : (position < maxTabShortcutPosition ? position : 0);
    return tabPositionHasShortcut(position) ? position : 0;
}

} // namespace Ubiquitty::Session
