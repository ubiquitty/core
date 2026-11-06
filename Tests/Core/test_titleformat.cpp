// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_titleformat.cpp — pins Title::buildRichTitle, the config-gated
// rich-title weave shared by the macOS (processTitleWithDimensionsIncluded /
// paneTitleString) and Linux (updateWindowTitle) frontends.
//
// buildRichTitle drops the segments whose show-flag is off (and every empty
// segment), then joins the survivors with one em dash in the fixed order base —
// directory — app title — status — chain — shell — profile — tty — dims. This test
// pins the gating: each flag independently hides its segment even when a
// non-empty value was passed, the dimensions flag toggles "cols×rows", the
// base title is gated by its own text (no flag), and an all-hidden title
// comes back empty so the caller's app-name fallback fires.

#include "Core/Title/Format.h"

#include <cstdio>
#include <string>
#include <string_view>

using Ubiquitty::PAL::Process::TitleSeparator;
using Ubiquitty::Title::buildRichTitle;
using Ubiquitty::Title::joinedLeadingPartLength;
using Ubiquitty::Title::joinedPrefixLengthBefore;
using Ubiquitty::Title::joinTitle;
using Ubiquitty::Title::resolveTitle;
using Ubiquitty::Title::SessionEnd;
using Ubiquitty::Title::TitleSegments;
using Ubiquitty::Title::TitleShowFlags;
// The window title carries a bare dash (the macOS titlebar spaces it with kern);
// every other surface carries the spaces, so both joins are pinned here.
static void expectTitle(const TitleShowFlags& show, const TitleSegments& segments, int columns, int rows, const std::string& want, const char* label)
{
    const std::string got = buildRichTitle(show, segments, columns, rows, TitleSeparator::Bare);
    if (got != want) {
        char ubqMessage[512];
        std::snprintf(ubqMessage, sizeof(ubqMessage), R"(%s: buildRichTitle = "%s", want "%s")", label, got.c_str(), want.c_str());
        CHECK_MSG(false, ubqMessage);
    }
}

// The classic four-segment inputs every original pin used.
static TitleSegments classic(std::string_view lead, std::string_view osc, std::string_view chain)
{
    return { .baseTitle = {}, .lead = lead, .osc = osc, .shellCommand = {}, .chain = chain, .profileName = {}, .ttyName = {}, .programStatus = {} };
}

TEST(titleformat)
{

    const char* x = "\xc3\x97"; // "×" (U+00D7)

    const TitleShowFlags all { .showsDirectory = true, .showsAppTitle = true, .showsShellCommand = false, .showsProcess = true, .showsProfileName = false, .showsTTYName = false, .showsDimensions = true, .showsProgramStatus = false };
    TitleShowFlags allWithoutDimensions = all;
    allWithoutDimensions.showsDimensions = false;

    // Everything on, dims on: full four-segment weave.
    expectTitle(all, classic("Projects", "Claude Code", "vim"), 80, 24, std::string("Projects—Claude Code—vim—80") + x + "24", "all segments + dims");

    // Everything on, dims off (the tab/pane label): no trailing dimensions.
    expectTitle(allWithoutDimensions, classic("Projects", "Claude Code", "vim"), 80, 24, "Projects—Claude Code—vim", "all segments, no dims");

    // Each flag independently hides its non-empty segment.
    {
        TitleShowFlags flags = allWithoutDimensions;
        flags.showsDirectory = false;
        expectTitle(flags, classic("Projects", "Claude Code", "vim"), 80, 24, "Claude Code—vim", "directory flag off drops lead");
    }
    {
        TitleShowFlags flags = allWithoutDimensions;
        flags.showsAppTitle = false;
        expectTitle(flags, classic("Projects", "Claude Code", "vim"), 80, 24, "Projects—vim", "appTitle flag off drops osc");
    }
    {
        TitleShowFlags flags = allWithoutDimensions;
        flags.showsProcess = false;
        expectTitle(flags, classic("Projects", "Claude Code", "vim"), 80, 24, "Projects—Claude Code", "process flag off drops chain");
    }

    // Dimensions flag toggles just the trailing segment.
    {
        TitleShowFlags flags = all;
        flags.showsAppTitle = false;
        flags.showsProcess = false;
        expectTitle(flags, classic("Projects", "", ""), 80, 24, std::string("Projects—80") + x + "24", "dims-only with lead");
    }

    // Empty segments are dropped before the dimensions segment is appended.
    expectTitle(all, classic("", "Claude Code", ""), 80, 24, std::string("Claude Code—80") + x + "24", "empty lead/chain treated as absent");

    // All segments hidden → empty string, so the caller's fallback fires.
    expectTitle(TitleShowFlags {}, classic("Projects", "Claude Code", "vim"), 80, 24, "", "everything off → empty");

    // Directory shown but empty value, app title present → no leading separator.
    expectTitle(allWithoutDimensions, classic("", "Claude Code", ""), 80, 24, "Claude Code", "empty shown lead does not emit a separator");

    // Base title leads every other segment and needs no flag.
    {
        TitleSegments segments = classic("Projects", "", "vim");
        segments.baseTitle = "Work";
        expectTitle(all, segments, 80, 24, std::string("Work—Projects—vim—80") + x + "24", "base title leads, no flag");
    }

    // The shell command FOLLOWS the process chain (the "-zsh — zsh" ruling);
    // tty follows the shell — the full-matrix order pin.
    {
        TitleShowFlags flags = all;
        flags.showsShellCommand = true;
        flags.showsProfileName = true;
        flags.showsTTYName = true;
        flags.showsDimensions = false;
        TitleSegments segments = classic("src", "hi", "vim");
        segments.shellCommand = "zsh";
        segments.profileName = "Work";
        segments.ttyName = "ttys003";
        expectTitle(flags, segments, 0, 0, "src—hi—vim—zsh—Work—ttys003", "chain-then-shell/profile/tty order");
    }

    // A gated-on but empty shell/tty never emits a separator.
    {
        TitleShowFlags flags = allWithoutDimensions;
        flags.showsShellCommand = true;
        flags.showsTTYName = true;
        expectTitle(flags, classic("src", "", "vim"), 0, 0, "src—vim", "empty shell/tty stay absent");
    }
    // The tab/pane/Linux surfaces carry the spaces in the string itself.
    {
        const std::string got = buildRichTitle(all, classic("Projects", "Claude Code", "vim"), 80, 24, TitleSeparator::Spaced);
        const std::string want = std::string("Projects \xe2\x80\x94 Claude Code \xe2\x80\x94 vim \xe2\x80\x94 80") + x + "24";
        if (got != want) {
            char ubqMessage[512];
            std::snprintf(ubqMessage, sizeof(ubqMessage), R"(spaced join: buildRichTitle = "%s", want "%s")", got.c_str(), want.c_str());
            CHECK_MSG(false, ubqMessage);
        }
    }

    // ---- a session whose process is gone -------------------------------
    //
    // The reported defect: a pane kept open after `exit` went on calling itself
    // "-zsh", naming a process that had left. Both process parts must go, and
    // nothing may take their place: a word in that position reads as the name
    // of a program, since that is what every other segment there is. What is
    // left is what is still true — the folder. The end is said in the terminal
    // itself instead, as a line of its own.
    {
        SessionEnd ended;
        ended.didEnd = true;
        TitleShowFlags flags = allWithoutDimensions;
        flags.showsShellCommand = true;
        TitleSegments segments = classic("~", "", "-zsh");
        segments.shellCommand = "zsh";
        const std::string got = buildRichTitle(flags, segments, 0, 0, TitleSeparator::Bare, ended);
        CHECK_MSG(got == "~", "an ended session shows the folder alone, with no stand-in for the process");
    }

    // The folder is not the only survivor: whatever else is true still shows,
    // so this is a drop of the process parts and not a collapse to one segment.
    {
        SessionEnd ended;
        ended.didEnd = true;
        TitleShowFlags flags = allWithoutDimensions;
        flags.showsShellCommand = true;
        TitleSegments segments = classic("~", "Build", "-zsh");
        segments.shellCommand = "zsh";
        const std::string got = buildRichTitle(flags, segments, 0, 0, TitleSeparator::Bare, ended);
        CHECK_MSG(got == "~—Build", "an ended session keeps every part that is not a process");
    }

    // Still running: nothing about this changes, which is the regression this
    // whole path most has to avoid.
    {
        TitleShowFlags flags = allWithoutDimensions;
        flags.showsShellCommand = true;
        TitleSegments segments = classic("~", "", "mc ▸ vi");
        segments.shellCommand = "zsh";
        const std::string got = buildRichTitle(flags, segments, 0, 0, TitleSeparator::Bare);
        CHECK_MSG(got == "~—mc ▸ vi—zsh", "a live session is untouched by the end path");
    }

    // The parts are keyed, so a frontend styling by role finds the marker in
    // its own field rather than by reading the joined sentence.
    {
        SessionEnd ended;
        ended.didEnd = true;
        ended.hasStatus = true;
        ended.code = 2;
        const auto parts = resolveTitle(allWithoutDimensions, classic("~", "", "-zsh"), 0, 0, ended);
        CHECK_MSG(parts.chain.empty(), "the chain part is empty, not merely unjoined");
        CHECK_MSG(parts.shellCommand.empty(), "the shell part is empty, not merely unjoined");
        CHECK_MSG(parts.directory == "~", "the parts that are still true survive");
    }
}

// Where the leading run ends, so a surface can dim the folder without going
// back to the finished line to look for punctuation.
TEST(testTheLeadingRunLengthIsCoresAnswerAndNotASearch)
{
    // Its own flags: the other case's are local to that test, and a shared
    // fixture between two tests is a way for one to move the other.
    TitleShowFlags shown {};
    shown.showsDirectory = true;
    shown.showsAppTitle = true;
    shown.showsShellCommand = true;
    shown.showsProcess = true;
    shown.showsProfileName = true;
    shown.showsTTYName = true;
    shown.showsDimensions = false;

    const auto lengthFor = [&shown](const TitleSegments& segments, TitleSeparator separator) {
        const auto parts = resolveTitle(shown, segments, 0, 0);
        return joinedLeadingPartLength(parts, separator);
    };

    // Both spellings of the separator, since the two surfaces use different ones.
    {
        const TitleSegments segments = classic("src", "", "vim");
        // "src" is 3 bytes; the bare dash is 3 bytes of UTF-8.
        CHECK(lengthFor(segments, TitleSeparator::Bare) == 6);
        // The spaced form is " — ", five bytes.
        CHECK(lengthFor(segments, TitleSeparator::Spaced) == 8);
    }

    // The length must index the JOINED line, not merely be arithmetic: slicing
    // there has to land exactly on the boundary between the two parts.
    {
        const TitleSegments segments = classic("~/Projects", "", "mc ▸ vi");
        const auto parts = resolveTitle(shown, segments, 0, 0);
        const std::string joined = joinTitle(parts, TitleSeparator::Spaced);
        const std::size_t run = joinedLeadingPartLength(parts, TitleSeparator::Spaced);
        CHECK(run <= joined.size());
        CHECK_STR_EQ(joined.substr(0, run), "~/Projects — ");
        CHECK_STR_EQ(joined.substr(run), "mc ▸ vi");
    }

    // Nothing to dim differently when there is no second part: no separator was
    // emitted, so the whole line is the leading part.
    {
        CHECK(lengthFor(classic("src", "", ""), TitleSeparator::Bare) == 0);
        CHECK(lengthFor(classic("", "", ""), TitleSeparator::Bare) == 0);
    }

    // A leading part that is also a prefix of what follows: the search this
    // replaces could not tell those apart, and the length still has to be the
    // first part's own, not the longest match.
    {
        const TitleSegments segments = classic("src", "", "src");
        const auto parts = resolveTitle(shown, segments, 0, 0);
        const std::string joined = joinTitle(parts, TitleSeparator::Spaced);
        CHECK_STR_EQ(joined.substr(0, joinedLeadingPartLength(parts, TitleSeparator::Spaced)), "src — ");
    }
}

// A program's status label is a part like the others: its switch gates it,
// it follows the app title before process and dimensions, and it goes when the session
// ends, because it described what a program that is gone was doing.
TEST(testAProgramStatusFollowsTheAppTitleAndGoesWithItsSession)
{
    TitleShowFlags shown {};
    shown.showsDirectory = true;
    shown.showsProcess = true;
    shown.showsDimensions = true;
    shown.showsProgramStatus = true;

    TitleSegments segments = classic("src", "", "vim");
    segments.programStatus = "Waiting";

    {
        const auto parts = resolveTitle(shown, segments, 80, 24);
        CHECK_STR_EQ(parts.programStatus, "Waiting");
        CHECK_STR_EQ(joinTitle(parts, TitleSeparator::Spaced), "src — Waiting — vim — 80×24");
        CHECK(joinedPrefixLengthBefore(parts, Ubiquitty::Title::TitlePart::ProgramStatus, TitleSeparator::Spaced) == std::string("src").size());
        CHECK(joinedPrefixLengthBefore(parts, Ubiquitty::Title::TitlePart::ProgramStatus, TitleSeparator::Bare) == std::string("src").size());
    }

    // A multibyte app title is before status, and the offset counts bytes in
    // the exact spelling the row joins rather than visible characters.
    {
        TitleShowFlags withAppTitle = shown;
        withAppTitle.showsAppTitle = true;
        TitleSegments named = segments;
        named.osc = "Étude";
        const auto parts = resolveTitle(withAppTitle, named, 80, 24);
        CHECK_STR_EQ(joinTitle(parts, TitleSeparator::Bare), "src—Étude—Waiting—vim—80×24");
        CHECK(joinedPrefixLengthBefore(parts, Ubiquitty::Title::TitlePart::ProgramStatus, TitleSeparator::Bare) == std::string("src—Étude").size());
    }

    // Off hides it even though a program set one.
    {
        TitleShowFlags hidden = shown;
        hidden.showsProgramStatus = false;
        const auto parts = resolveTitle(hidden, segments, 80, 24);
        CHECK(parts.programStatus.empty());
        CHECK_STR_EQ(joinTitle(parts, TitleSeparator::Spaced), "src — vim — 80×24");
    }

    // No label is no part, not an empty separator at the end of the line.
    {
        TitleSegments unlabelled = segments;
        unlabelled.programStatus = {};
        CHECK_STR_EQ(joinTitle(resolveTitle(shown, unlabelled, 80, 24), TitleSeparator::Spaced), "src — vim — 80×24");
    }

    // An ended session keeps the folder and drops everything a departed
    // program said about itself: its name, and the status it reported.
    {
        SessionEnd ended;
        ended.didEnd = true;
        const auto parts = resolveTitle(shown, segments, 80, 24, ended);
        CHECK_MSG(parts.programStatus.empty(), "a status names work nobody is doing once its program has ended");
        CHECK_STR_EQ(joinTitle(parts, TitleSeparator::Spaced), "src — 80×24");
    }
}

TEST(testATabLabelLeadsWithItsPinThenItsHost)
{
    using Ubiquitty::Title::markTabLabel;
    using Ubiquitty::Title::tabLabelMarkPrefix;
    using Ubiquitty::Title::TabLabelMarks;

    // A plain local tab earns no mark, and its label is untouched.
    CHECK_STR_EQ(markTabLabel("src — zsh", TabLabelMarks {}), "src — zsh");
    CHECK(tabLabelMarkPrefix(TabLabelMarks {}).empty());

    // A tab on a host leads with the host, so two hosts in one strip read
    // apart down the column of labels.
    TabLabelMarks hosted;
    hosted.hostName = "Studio";
    CHECK_STR_EQ(markTabLabel("~ — zsh", hosted), "Studio — ~ — zsh");

    // The pin still leads; the host follows it.
    TabLabelMarks both = hosted;
    both.isPinned = true;
    CHECK_STR_EQ(markTabLabel("~ — zsh", both), "📌 Studio — ~ — zsh");

    // The prefix is exactly what the marks put before the label, so a bake
    // measures the label's folder run past it rather than searching for it.
    const std::string marked = markTabLabel("src — zsh", both);
    const std::string prefix = tabLabelMarkPrefix(both);
    CHECK(marked.starts_with(prefix));
    CHECK_STR_EQ(marked.substr(prefix.size()), "src — zsh");
    CHECK(joinedLeadingPartLength(marked.substr(prefix.size()), "src", TitleSeparator::Spaced) > 0);
}

TEST(testATabLabelCarriesNoTagText)
{
    using Ubiquitty::Title::markTabLabel;
    using Ubiquitty::Title::TabLabelMarks;

    // A window's tag is drawn by the strip at the tab's leading edge, so the
    // label text names only the pin and the host: a tag that came and went
    // would otherwise shift the centred label.
    TabLabelMarks pinnedOnHost;
    pinnedOnHost.isPinned = true;
    pinnedOnHost.hostName = "Studio";
    CHECK_STR_EQ(markTabLabel("~ — zsh", pinnedOnHost), "📌 Studio — ~ — zsh");
    CHECK(markTabLabel("~ — zsh", TabLabelMarks {}) == "~ — zsh");
}

TEST(testATabLabelThatNamesItsHostSaysTheNameOnce)
{
    using Ubiquitty::Title::labelNamesHost;
    using Ubiquitty::Title::marksForLabel;
    using Ubiquitty::Title::markTabLabel;
    using Ubiquitty::Title::tabLabelMarkPrefix;
    using Ubiquitty::Title::TabLabelMarks;

    TabLabelMarks hosted;
    hosted.hostName = "Studio";
    // A label that fell back to the host's own name: the name once.
    CHECK_STR_EQ(markTabLabel("Studio", hosted), "Studio");
    CHECK_STR_EQ(markTabLabel(" studio ", hosted), " studio ");
    // A title that opens or ends with it, as a word.
    CHECK(labelNamesHost("Studio — zsh", "Studio"));
    CHECK(labelNamesHost("zsh — studio", "Studio"));
    CHECK(labelNamesHost("ada@host: STUDIO", "studio"));
    // A name inside a longer word is not the host's.
    CHECK(!labelNamesHost("Studio2 — zsh", "Studio"));
    CHECK(!labelNamesHost("build-server — zsh", "build"));
    CHECK(!labelNamesHost("~ — zsh", "Studio"));
    CHECK(!labelNamesHost("Studio", ""));
    CHECK_STR_EQ(markTabLabel("~ — zsh", hosted), "Studio — ~ — zsh");

    // The pin stays when the host goes: it is not a name.
    TabLabelMarks pinned = hosted;
    pinned.isPinned = true;
    CHECK_STR_EQ(markTabLabel("Studio", pinned), "📌 Studio");
    // The prefix a bake measures past is the one the marked label carries.
    const TabLabelMarks drawn = marksForLabel("Studio", pinned);
    CHECK(markTabLabel("Studio", pinned).starts_with(tabLabelMarkPrefix(drawn)));
    CHECK(drawn.hostName.empty() && drawn.isPinned);
}

UBIQUITTY_TEST_MAIN()
