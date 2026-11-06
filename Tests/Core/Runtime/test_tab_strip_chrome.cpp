// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/TabStripChrome.h"
#include "Harness.h"

// test_tab_strip_chrome.cpp — keeping the grid when a native tab strip toggles
// (Core/Runtime/TabStripChrome.h).
//
// The strip takes its height out of the content view, so a window that does
// nothing loses a row of terminal to it. Moving the bottom edge down by that
// height keeps every row where it was. What the cases below are really about
// is the exceptions: a window at the bottom of the screen, a difference too
// large to be a strip at all, and a size the user has taken over. Bare libc++;
// no platform code.

using Ubiquitty::Runtime::maxPlausibleTabStripHeight;
using Ubiquitty::Runtime::TabStripFrameFacts;
using Ubiquitty::Runtime::tabStripGrowth;
using Ubiquitty::Runtime::tabStripUndo;

namespace {

// A window with room under it, losing 28 points of content to the strip.
TabStripFrameFacts stripAppeared()
{
    return { .previousContentHeight = 500, .currentContentHeight = 472, .roomBelow = 400, .grownAmount = 0 };
}

} // namespace

TEST(testTheWindowGrowsByWhatTheStripTook)
{
    CHECK(tabStripGrowth(stripAppeared()) == 28);
}

// A window already at the bottom of the screen keeps its frame and gives up the
// row: moving it off-screen is worse than reflowing.
TEST(testTheGrowthIsCappedByTheRoomBelow)
{
    TabStripFrameFacts facts = stripAppeared();
    facts.roomBelow = 10;
    CHECK(tabStripGrowth(facts) == 10);
    facts.roomBelow = 0;
    CHECK(tabStripGrowth(facts) == 0);
    facts.roomBelow = -5; // a window already off the visible area
    CHECK(tabStripGrowth(facts) == 0);
}

// The reselect-tab-0 window jump: a backgrounded tab receives no resize while
// another is selected, so its remembered height can predate the tab group, and
// one catch-up frame change carries the whole stint's difference.
TEST(testAnImplausibleDifferenceIsACatchUpAndGrowsNothing)
{
    TabStripFrameFacts facts = stripAppeared();
    facts.currentContentHeight = facts.previousContentHeight - (maxPlausibleTabStripHeight + 1);
    CHECK(tabStripGrowth(facts) == 0);

    // At the boundary it is still a strip.
    facts.currentContentHeight = facts.previousContentHeight - maxPlausibleTabStripHeight;
    CHECK(tabStripGrowth(facts) == maxPlausibleTabStripHeight);
}

// A content view that grew, or did not move, is not a strip appearing.
TEST(testOnlyAContentViewThatShrankGrowsTheWindow)
{
    TabStripFrameFacts facts = stripAppeared();
    facts.currentContentHeight = facts.previousContentHeight;
    CHECK(tabStripGrowth(facts) == 0);
    facts.currentContentHeight = facts.previousContentHeight + 20;
    CHECK(tabStripGrowth(facts) == 0);
}

// Before the first layout there is no baseline to measure against.
TEST(testAWindowWithNoBaselineGrowsNothing)
{
    TabStripFrameFacts facts = stripAppeared();
    facts.previousContentHeight = 0;
    CHECK(tabStripGrowth(facts) == 0);
    facts = stripAppeared();
    facts.currentContentHeight = 0;
    CHECK(tabStripGrowth(facts) == 0);
}

TEST(testTheUndoGivesBackOnlyWhatThisWindowGrew)
{
    TabStripFrameFacts facts { .previousContentHeight = 472, .currentContentHeight = 500, .roomBelow = 372, .grownAmount = 28 };
    CHECK(tabStripUndo(facts, 600) == 28);

    // A user resize zeroed the grow: the person who dragged the window owns its
    // size, and taking height back to undo a move they overrode argues with them.
    facts.grownAmount = 0;
    CHECK(tabStripUndo(facts, 600) == 0);
}

// The move cannot consume the window: a frame shorter than what was grown keeps
// what it has rather than collapsing to nothing.
TEST(testTheUndoRefusesToConsumeTheWindow)
{
    TabStripFrameFacts facts { .previousContentHeight = 472, .currentContentHeight = 500, .roomBelow = 372, .grownAmount = 28 };
    CHECK(tabStripUndo(facts, 28) == 0);
    CHECK(tabStripUndo(facts, 27) == 0);
    CHECK(tabStripUndo(facts, 29) == 28);
}

// The strip's mark: output on a tab nobody is looking at is news, and the echo
// of a resize the user just performed is not.
TEST(testOutputMarksOnlyATabNobodyIsLookingAt)
{
    using Ubiquitty::Runtime::doesOutputMarkUnseenActivity;
    using Ubiquitty::Runtime::UnseenActivityFacts;

    CHECK(doesOutputMarkUnseenActivity({ .isAlreadyMarked = false, .isForeground = false, .isWithinResizeSettle = false }));
    CHECK_MSG(!doesOutputMarkUnseenActivity({ .isAlreadyMarked = false, .isForeground = true, .isWithinResizeSettle = false }),
        "a tab being looked at has nothing unseen on it");
    CHECK_MSG(!doesOutputMarkUnseenActivity({ .isAlreadyMarked = true, .isForeground = false, .isWithinResizeSettle = false }),
        "a tab already marked decides nothing, so a flood costs one comparison");
    CHECK_MSG(!doesOutputMarkUnseenActivity({ .isAlreadyMarked = false, .isForeground = false, .isWithinResizeSettle = true }),
        "a resize's own prompt redraw is not a program's news");
}

// The three settings the hints answer to.
TEST(tabShortcutHintsFollowTheirSetting)
{
    using Ubiquitty::Preferences::TabShortcutHints;
    using Ubiquitty::Runtime::areTabShortcutHintsVisible;

    CHECK(!areTabShortcutHintsVisible(TabShortcutHints::Off, true));
    CHECK(!areTabShortcutHintsVisible(TabShortcutHints::Off, false));
    CHECK(areTabShortcutHintsVisible(TabShortcutHints::Always, false));
    CHECK(areTabShortcutHintsVisible(TabShortcutHints::Always, true));
    // The one that reads the modifier: the question is only asked while
    // reaching for the key.
    CHECK(!areTabShortcutHintsVisible(TabShortcutHints::OnCommand, false));
    CHECK(areTabShortcutHintsVisible(TabShortcutHints::OnCommand, true));
}

// A bell marks the tab it rang in unless somebody was there to hear it.
TEST(aBellMarksTheTabNobodyWasLookingAt)
{
    using Ubiquitty::Runtime::doesBellMarkUnheard;

    CHECK(doesBellMarkUnheard(false));
    CHECK_MSG(!doesBellMarkUnheard(true),
        "a bell the reader was present for has already done its job");
}

// What the one accessory slot shows, when a tab has several things to say.
TEST(tabAccessoryRanksWhatATabHasToSay)
{
    using Ubiquitty::Runtime::TabAccessory;
    using Ubiquitty::Runtime::tabAccessory;
    using Ubiquitty::Runtime::TabAccessoryFacts;

    TabAccessoryFacts quiet {};
    CHECK(tabAccessory(quiet) == TabAccessory::None);

    // A tab the reader is looking at wears no mark drawing them to it.
    TabAccessoryFacts selected {};
    selected.isSelected = true;
    selected.hasUnseenActivity = true;
    selected.hasUnseenNotification = true;
    selected.hasProgress = true;
    CHECK(tabAccessory(selected) == TabAccessory::None);

    // The five attention marks, each outranking the one below it.
    TabAccessoryFacts loud {};
    loud.hasProgress = true;
    loud.hasUnheardBell = true;
    loud.hasStatusColour = true;
    loud.hasUnseenNotification = true;
    loud.hasUnseenActivity = true;
    CHECK_MSG(tabAccessory(loud) == TabAccessory::ProgressRing,
        "a live ring outranks a latch that can wait for the job to end");
    loud.hasProgress = false;
    CHECK(tabAccessory(loud) == TabAccessory::ColouredBell);
    loud.hasUnheardBell = false;
    CHECK(tabAccessory(loud) == TabAccessory::StatusDot);
    loud.hasStatusColour = false;
    CHECK(tabAccessory(loud) == TabAccessory::NotificationDot);
    loud.hasUnseenNotification = false;
    CHECK(tabAccessory(loud) == TabAccessory::ActivityDots);

    // The bell's two faces. A colour the program asked for is not dropped by
    // the bell outranking the dot — the bell is drawn in it — and a bell
    // with no colour behind it is the plain one.
    TabAccessoryFacts rang {};
    rang.hasUnheardBell = true;
    CHECK(tabAccessory(rang) == TabAccessory::Bell);
    rang.hasStatusColour = true;
    CHECK(tabAccessory(rang) == TabAccessory::ColouredBell);

    // A bell in the tab the reader is looking at was heard when it rang.
    rang.isSelected = true;
    CHECK(tabAccessory(rang) == TabAccessory::None);

    // The hint is what a tab shows having nothing else to say. It ranks LAST:
    // its setting can leave it on permanently, and a hint that outranked the
    // marks would mean no tab could ever report anything.
    TabAccessoryFacts asked {};
    asked.isShortcutHintHeld = true;
    asked.hasShortcut = true;
    CHECK(tabAccessory(asked) == TabAccessory::ShortcutHint);
    asked.hasUnseenActivity = true;
    CHECK_MSG(tabAccessory(asked) == TabAccessory::ActivityDots,
        "unread output outranks a number the reader can count to");

    // The selected tab has no marks to outrank it, so it answers the hint.
    asked.isSelected = true;
    CHECK(tabAccessory(asked) == TabAccessory::ShortcutHint);

    // A position no shortcut reaches shows no hint, whatever is held: a label
    // there would name a key that does nothing.
    TabAccessoryFacts unreachable {};
    unreachable.isShortcutHintHeld = true;
    CHECK(tabAccessory(unreachable) == TabAccessory::None);
}

// With the hint turned off, the slot carries the tab's status in its place.
TEST(tabAccessoryCarriesTheStatusWhereTheHintIsOff)
{
    using Ubiquitty::Runtime::TabAccessory;
    using Ubiquitty::Runtime::tabAccessory;
    using Ubiquitty::Runtime::TabAccessoryFacts;

    // The status dot shows on every tab, the selected one included.
    TabAccessoryFacts status {};
    status.isShortcutHintOff = true;
    status.hasShortcut = true;
    status.hasStatusColour = true;
    CHECK(tabAccessory(status) == TabAccessory::StatusDot);
    status.isSelected = true;
    CHECK_MSG(tabAccessory(status) == TabAccessory::StatusDot, "the selected tab shows its status where its number would be");

    // Unread output earns no ellipsis there, selected or not.
    TabAccessoryFacts unread {};
    unread.isShortcutHintOff = true;
    unread.hasUnseenActivity = true;
    CHECK(tabAccessory(unread) == TabAccessory::None);
    unread.hasStatusColour = true;
    CHECK_MSG(tabAccessory(unread) == TabAccessory::StatusDot, "output seen or not, the status is what the slot says");

    // The selected tab draws its progress as the bar in its pill, so the ring
    // stays for the tabs behind it.
    TabAccessoryFacts working {};
    working.isShortcutHintOff = true;
    working.hasProgress = true;
    CHECK(tabAccessory(working) == TabAccessory::ProgressRing);
    working.isSelected = true;
    CHECK(tabAccessory(working) == TabAccessory::None);
    working.hasStatusColour = true;
    CHECK(tabAccessory(working) == TabAccessory::StatusDot);

    // A bell that rang while the reader was elsewhere keeps its mark, on the
    // selected tab of a window that was not key too, until it is looked at.
    TabAccessoryFacts rang {};
    rang.isShortcutHintOff = true;
    rang.isSelected = true;
    rang.hasUnheardBell = true;
    CHECK(tabAccessory(rang) == TabAccessory::Bell);
    rang.hasStatusColour = true;
    CHECK(tabAccessory(rang) == TabAccessory::ColouredBell);
    rang.hasUnheardBell = false;
    CHECK(tabAccessory(rang) == TabAccessory::StatusDot);

    // A notification still marks only the tabs the reader is not on.
    TabAccessoryFacts asked {};
    asked.isShortcutHintOff = true;
    asked.hasUnseenNotification = true;
    CHECK(tabAccessory(asked) == TabAccessory::NotificationDot);
    asked.isSelected = true;
    CHECK(tabAccessory(asked) == TabAccessory::None);

    // A hint left on keeps the ranking above whatever the hint is doing.
    TabAccessoryFacts on {};
    on.hasUnseenActivity = true;
    CHECK(tabAccessory(on) == TabAccessory::ActivityDots);
}

UBIQUITTY_TEST_MAIN()
