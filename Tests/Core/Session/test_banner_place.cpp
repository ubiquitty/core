// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// test_banner_place.cpp — the line a banner says its place in
// (Core/Session/SessionOrigin.h, `describeBannerPlace`).
//
// A consent prompt labels and quotes every level of a session's origin,
// because a title there must not be able to pass for the prompt's own
// sentences. A banner has one glance and one line, and the same form spent it
// on kind words and quotation marks, said the title twice — once as the
// window's and once as the tab's — and left the host inside the tab title
// where the system's cut removed it.
//
// What the cases hold to:
//
//   The levels run outermost first — host, workspace, title, pane — joined by
//   the level mark and nothing else.
//
//   A level that says nothing is not written: an empty one, one repeating the
//   level before it, and the pane of a tab that has only one.
//
//   A local session that is the only pane of the only tab of the only window
//   has no place to state, and the line is empty.
//
//   Every name is untrusted. None can add a level, reorder the line, break it,
//   or grow until the host is pushed off it.
//

#include "Harness.h"

#include "Core/Session/SessionOrigin.h"

#include <string>

namespace {

using Ubiquitty::Session::BannerPlaceFacts;
using Ubiquitty::Session::describeBannerPlace;

/// A remote pane among several, with every level stated.
BannerPlaceFacts remoteSplit()
{
    BannerPlaceFacts facts;
    facts.host = "Studio";
    facts.workspace = "Main";
    facts.windowTitle = "~ — ssh build-box";
    facts.tabTitle = "~ — ssh build-box";
    facts.paneNumber = 4;
    facts.windowCount = 1;
    facts.tabCount = 3;
    facts.paneCount = 4;
    return facts;
}

/// A local shell with nothing beside it.
BannerPlaceFacts localAlone()
{
    BannerPlaceFacts facts;
    facts.windowTitle = "~ — make";
    facts.tabTitle = "~ — make";
    facts.paneNumber = 1;
    return facts;
}

std::string repeated(std::string_view unit, int count)
{
    std::string out;
    for (int index = 0; index < count; ++index)
        out += unit;
    return out;
}

} // namespace

TEST(aLocalSessionWithNothingBesideItHasNoPlace)
{
    CHECK_STR_EQ(describeBannerPlace(localAlone()), std::string());
    CHECK_STR_EQ(describeBannerPlace(BannerPlaceFacts {}), std::string());
}

TEST(aSecondTabWindowOrPaneGivesALocalSessionAPlace)
{
    BannerPlaceFacts tabs = localAlone();
    tabs.tabCount = 2;
    CHECK_STR_EQ(describeBannerPlace(tabs), std::string("~ — make"));

    BannerPlaceFacts windows = localAlone();
    windows.windowCount = 2;
    CHECK_STR_EQ(describeBannerPlace(windows), std::string("~ — make"));

    BannerPlaceFacts panes = localAlone();
    panes.paneCount = 2;
    panes.paneNumber = 2;
    CHECK_STR_EQ(describeBannerPlace(panes), std::string("~ — make › 2"));
}

TEST(aRemoteSessionAlwaysStatesItsHostFirst)
{
    BannerPlaceFacts facts = remoteSplit();
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~ — ssh build-box › 4"));

    // One window, one tab, one pane — and still a place, because the host is
    // the part a person with two machines open needs.
    facts.tabCount = 1;
    facts.paneCount = 1;
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~ — ssh build-box"));

    facts.workspace.clear();
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › ~ — ssh build-box"));
}

TEST(aTitleAProgramSetIsUsedAsTheTabShowsIt)
{
    BannerPlaceFacts facts = localAlone();
    facts.tabCount = 2;
    facts.windowTitle = "editor: notes.txt";
    facts.tabTitle = "editor: notes.txt";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("editor: notes.txt"));
}

TEST(aWindowAndATabWithOneTitleAreOnePart)
{
    BannerPlaceFacts facts = remoteSplit();
    facts.windowTitle = "Deploys";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › Deploys › ~ — ssh build-box › 4"));

    // Sameness is judged on what would be written, not on the bytes handed in.
    facts.windowTitle = "~  —  ssh build-box\n";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~ — ssh build-box › 4"));

    facts.windowTitle.clear();
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~ — ssh build-box › 4"));
    facts.windowTitle = facts.tabTitle;
    facts.tabTitle.clear();
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~ — ssh build-box › 4"));
}

TEST(aWindowAPersonNamedIsKnownByThatName)
{
    BannerPlaceFacts facts = remoteSplit();
    // The composer weaves a window's name in among its parts; the place line
    // says the name, and the tab follows as it always does.
    facts.windowName = "Deploys";
    facts.windowTitle = "~ — Deploys — ssh build-box";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › Deploys › ~ — ssh build-box › 4"));

    // A name is a name like the others: sanitized, bounded, and written once
    // when the tab is called the same.
    facts.windowName = "Deploys › prod\x07";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › Deploys prod › ~ — ssh build-box › 4"));
    facts.windowName = "~ — ssh build-box";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~ — ssh build-box › 4"));

    // A local window with a name and nothing beside it still has no place.
    BannerPlaceFacts alone = localAlone();
    alone.windowName = "Scratch";
    CHECK_STR_EQ(describeBannerPlace(alone), std::string());
    alone.tabCount = 2;
    CHECK_STR_EQ(describeBannerPlace(alone), std::string("Scratch › ~ — make"));
}

TEST(aTitleHoldingTheOtherWholeIsTheOneWritten)
{
    // The window's title is woven around the tab's: the tab says nothing more.
    BannerPlaceFacts facts = remoteSplit();
    facts.windowTitle = "~ — evals — claude";
    facts.tabTitle = "evals";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~ — evals — claude › 4"));

    // A tab title that holds the window's says the window's too.
    facts.windowTitle = "evals";
    facts.tabTitle = "~ — evals — claude";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~ — evals — claude › 4"));

    // Holding is judged on whole words, so part of a word is not a repeat.
    facts.windowTitle = "~ — cmake";
    facts.tabTitle = "make";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~ — cmake › make › 4"));

    // A name a person gave the window stays, whatever the tab holds.
    facts.windowName = "evals";
    facts.windowTitle = "~ — evals — claude";
    facts.tabTitle = "~ — evals — claude";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › evals › ~ — evals — claude › 4"));

    // Judged before either is cut: a long window title woven around a long
    // tab title is the one part, however each would be shortened.
    BannerPlaceFacts local = localAlone();
    local.tabCount = 2;
    local.windowTitle = "~ — Custom evals in Langfuse — claude";
    local.tabTitle = "Custom evals in Langfuse";
    const std::string line = describeBannerPlace(local);
    CHECK(line.find("Custom evals in La") != std::string::npos);
    CHECK(line.find(" › ") == std::string::npos);
}

TEST(aLevelRepeatingTheOneBeforeItIsWrittenOnce)
{
    BannerPlaceFacts facts = remoteSplit();
    facts.workspace = "Studio";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › ~ — ssh build-box › 4"));

    // A window that fell back to its host's name says nothing the host did not.
    facts = remoteSplit();
    facts.workspace.clear();
    facts.windowTitle = "Studio";
    facts.tabTitle = "Studio";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › 4"));
}

TEST(thePaneIsNamedOnlyAmongSeveralAndByItsLabelWhenItHasOne)
{
    BannerPlaceFacts facts = remoteSplit();
    facts.paneLabel = "logs";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~ — ssh build-box › logs"));

    facts.paneCount = 1;
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~ — ssh build-box"));

    // A pane nobody numbered is not given a number here.
    facts.paneCount = 2;
    facts.paneLabel.clear();
    facts.paneNumber = 0;
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~ — ssh build-box"));
}

TEST(aNameCannotAddALevelOrCarryAControl)
{
    BannerPlaceFacts facts = remoteSplit();
    // The level mark inside a name would read as two levels.
    facts.tabTitle = "notes › root › 1";
    facts.windowTitle = facts.tabTitle;
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › notes root 1 › 4"));

    // Quotation marks are only characters here: nothing is quoted, so nothing
    // can be closed early.
    facts.tabTitle = "say “hi” \"there\"";
    facts.windowTitle = facts.tabTitle;
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › say “hi” \"there\" › 4"));

    // C0, DEL and C1 become blanks, and blanks fold.
    facts.tabTitle = "a\x1b[31mb\x07\x7f  c\r\n\td\xc2\x85"
                     "e";
    facts.windowTitle = facts.tabTitle;
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › a [31mb c d e › 4"));

    // A right-to-left override would draw the rest of the line backwards; the
    // bidirectional controls and zero-width padding are dropped outright.
    facts.tabTitle = "safe\xe2\x80\xae"
                     "gpj.exe\xe2\x81\xa6\xe2\x80\x8b\xef\xbb\xbf!";
    facts.windowTitle = facts.tabTitle;
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › safegpj.exe! › 4"));

    // Bytes that are not UTF-8 are dropped, including a scalar cut short.
    facts.tabTitle = "ok\xff\xc3";
    facts.windowTitle = facts.tabTitle;
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ok › 4"));

    // The host's own name is a name like the others.
    facts = remoteSplit();
    facts.host = "Studio › Main › root\x1b";
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio Main root › Main › ~ — ssh build-box › 4"));
}

TEST(aLongNameIsCutToItsOwnBoundAndCannotPushTheOthersOut)
{
    using namespace Ubiquitty::Session;
    BannerPlaceFacts facts = remoteSplit();
    facts.tabTitle = repeated("x", 200);
    facts.windowTitle = facts.tabTitle;
    const std::string line = describeBannerPlace(facts);
    CHECK_STR_EQ(line, "Studio › Main › " + repeated("x", maxBannerTitleColumns - 1) + "… › 4");

    facts = remoteSplit();
    facts.host = repeated("h", 200);
    facts.workspace = repeated("w", 200);
    CHECK_STR_EQ(describeBannerPlace(facts),
        repeated("h", maxBannerHostColumns - 1) + "… › " + repeated("w", maxBannerWorkspaceColumns - 1) + "… › ~ — ssh build-box › 4");
}

TEST(theWholeLineIsBoundedByShorteningFromTheEndAndNeverTheHost)
{
    using namespace Ubiquitty::Session;
    BannerPlaceFacts facts;
    facts.host = repeated("h", 200);
    facts.workspace = repeated("w", 200);
    facts.windowTitle = repeated("n", 200);
    facts.tabTitle = repeated("t", 200);
    facts.paneLabel = repeated("p", 200);
    facts.paneCount = 2;
    const std::string line = describeBannerPlace(facts);
    // Every part at its own bound is 108 columns and four marks; the line's
    // bound takes the excess from the last part back, each to the floor, and
    // stops as soon as the line fits.
    const std::string expected = repeated("h", maxBannerHostColumns - 1) + "… › "
        + repeated("w", 11) + "… › "
        + repeated("n", minBannerPartColumns - 1) + "… › "
        + repeated("t", minBannerPartColumns - 1) + "… › "
        + repeated("p", minBannerPartColumns - 1) + "…";
    CHECK_STR_EQ(line, expected);
    CHECK_EQ(Ubiquitty::Title::bannerTextColumns(line), maxBannerPlaceColumns);

    // The common long case: one long title beside short names loses only its
    // own tail.
    facts = remoteSplit();
    facts.tabTitle = "~/Projects/terminal/Source/Core/Session — editor SessionOrigin.h";
    facts.windowTitle = facts.tabTitle;
    CHECK_STR_EQ(describeBannerPlace(facts), std::string("Studio › Main › ~/Projects/terminal/Source/Core… › 4"));
}

TEST(aCutFallsBetweenGraphemeClustersAndCountsWideOnesTwice)
{
    using namespace Ubiquitty::Session;
    BannerPlaceFacts facts = remoteSplit();
    // Twenty wide ideographs are forty columns: fifteen fit beside the
    // ellipsis, a sixteenth would not.
    facts.tabTitle = repeated("漢", 20);
    facts.windowTitle = facts.tabTitle;
    CHECK_STR_EQ(describeBannerPlace(facts), "Studio › Main › " + repeated("漢", 15) + "… › 4");

    // An odd column left before the ellipsis is not filled with half a
    // character.
    facts.tabTitle = "a" + repeated("漢", 20);
    facts.windowTitle = facts.tabTitle;
    CHECK_STR_EQ(describeBannerPlace(facts), "Studio › Main › a" + repeated("漢", 15) + "… › 4");

    // A family joined by zero-width joiners is one cluster: it stays whole or
    // goes whole.
    const std::string family = "👨‍👩‍👧‍👦";
    facts.tabTitle = repeated("y", maxBannerTitleColumns - 2) + family + "tail";
    facts.windowTitle = facts.tabTitle;
    CHECK_STR_EQ(describeBannerPlace(facts), "Studio › Main › " + repeated("y", maxBannerTitleColumns - 2) + "… › 4");
    facts.tabTitle = repeated("y", maxBannerTitleColumns - 3) + family + "tail";
    facts.windowTitle = facts.tabTitle;
    CHECK_STR_EQ(describeBannerPlace(facts), "Studio › Main › " + repeated("y", maxBannerTitleColumns - 3) + family + "… › 4");

    // A combining mark stays with its letter.
    facts.host = repeated("e\xcc\x81", 40);
    CHECK(describeBannerPlace(facts).starts_with(repeated("e\xcc\x81", maxBannerHostColumns - 1) + "… › "));
}

TEST(theConsentFormIsUntouchedByTheBannerForm)
{
    using Ubiquitty::Session::describeSessionOrigin;
    using Ubiquitty::Session::SessionOrigin;
    SessionOrigin origin;
    origin.workspace = "Main";
    origin.window = "~ — ssh build-box";
    origin.tab = "Studio · ~ — ssh build-box";
    origin.pane = "4";
    CHECK_STR_EQ(describeSessionOrigin(origin), std::string("workspace “Main” › window “~ — ssh build-box” › tab “Studio · ~ — ssh build-box” › pane “4”"));
}

UBIQUITTY_TEST_MAIN()
