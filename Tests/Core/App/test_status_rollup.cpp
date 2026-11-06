// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_status_rollup.cpp — the one ordering of a pane's status facts against
// each other, and the fold that gives a tab, a window and a workspace their
// most urgent child (Source/Core/App/StatusRollup.h).
//
// Compiles against bare libc++: the roll-up takes plain values. Pins the order
// (failed > blocked > attention > done > working > running > idle), that each
// vocabulary's own order survives inside it, and that grouping panes into tabs
// and windows cannot change what the workspace shows.

#include "Core/App/StatusRollup.h"

#include <vector>

using namespace Ubiquitty;
using App::PaneStatusFacts;
using App::RolledStatus;
using App::StatusKind;

namespace {

PaneStatusFacts atPrompt()
{
    PaneStatusFacts facts;
    facts.isAtPrompt = true;
    return facts;
}

PaneStatusFacts withProgram(OSC::ProgramState state)
{
    PaneStatusFacts facts;
    facts.programState = state;
    return facts;
}

RolledStatus roll(const std::vector<RolledStatus>& children) { return App::rollUpStatus(children); }

bool isSame(const RolledStatus& a, const RolledStatus& b)
{
    const auto isSameColor = [](const std::optional<ColorRGBA>& left, const std::optional<ColorRGBA>& right) {
        return left.has_value() == right.has_value() && (!left || (left->red == right->red && left->green == right->green && left->blue == right->blue));
    };
    return a.kind == b.kind && a.count == b.count && a.paneCount == b.paneCount && a.blockKind == b.blockKind && a.progress.state == b.progress.state
        && a.progress.value == b.progress.value && a.hasUnseenBell == b.hasUnseenBell && a.hasUnseenNotification == b.hasUnseenNotification
        && isSameColor(a.indicatorColor, b.indicatorColor) && isSameColor(a.tabColor, b.tabColor) && a.badgeCount == b.badgeCount;
}

} // namespace

TEST(aPanesFactsAmountToTheMostUrgentKindAnyOfThemSupports)
{
    // With nothing reported, who holds the terminal decides.
    CHECK(App::paneStatusKind(atPrompt()) == StatusKind::Idle);
    CHECK(App::paneStatusKind(PaneStatusFacts {}) == StatusKind::Running);

    // The status record's own order survives inside the larger one.
    CHECK(App::paneStatusKind(withProgram(OSC::ProgramState::Idle)) == StatusKind::Idle);
    CHECK(App::paneStatusKind(withProgram(OSC::ProgramState::Working)) == StatusKind::Working);
    CHECK(App::paneStatusKind(withProgram(OSC::ProgramState::Done)) == StatusKind::Done);
    CHECK(App::paneStatusKind(withProgram(OSC::ProgramState::Blocked)) == StatusKind::Blocked);
    CHECK(App::paneStatusKind(withProgram(OSC::ProgramState::Error)) == StatusKind::Failed);
    CHECK(StatusKind::Failed > StatusKind::Blocked && StatusKind::Blocked > StatusKind::Attention && StatusKind::Attention > StatusKind::Done
        && StatusKind::Done > StatusKind::Working && StatusKind::Working > StatusKind::Running && StatusKind::Running > StatusKind::Idle
        && StatusKind::Idle > StatusKind::None);

    // Progress: its error state is a failure, every other live state is work.
    for (const std::uint8_t state : { OSC::ProgressNormal, OSC::ProgressIndeterminate, OSC::ProgressPaused }) {
        PaneStatusFacts facts = atPrompt();
        facts.progress = { state, 40 };
        CHECK(App::paneStatusKind(facts) == StatusKind::Working);
    }
    PaneStatusFacts failedProgress;
    failedProgress.progress = { OSC::ProgressError, 0 };
    CHECK(App::paneStatusKind(failedProgress) == StatusKind::Failed);

    // An exit status counts while its prompt shows it, and only a failing one.
    PaneStatusFacts failedExit = atPrompt();
    failedExit.exitStatus = 2;
    CHECK(App::paneStatusKind(failedExit) == StatusKind::Failed);
    failedExit.exitStatus = 0;
    CHECK(App::paneStatusKind(failedExit) == StatusKind::Idle);
    PaneStatusFacts staleExit;
    staleExit.exitStatus = 2;
    CHECK_MSG(App::paneStatusKind(staleExit) == StatusKind::Running, "a failure is not the status of the command that followed it");

    // Attention sits between a block and a finished job.
    PaneStatusFacts rang = withProgram(OSC::ProgramState::Done);
    rang.hasUnseenBell = true;
    CHECK(App::paneStatusKind(rang) == StatusKind::Attention);
    rang.programState = OSC::ProgramState::Blocked;
    CHECK(App::paneStatusKind(rang) == StatusKind::Blocked);
    PaneStatusFacts notified = atPrompt();
    notified.hasUnseenNotification = true;
    CHECK(App::paneStatusKind(notified) == StatusKind::Attention);

    // A program that calls itself idle is idle though it holds the terminal.
    CHECK(App::paneStatusKind(withProgram(OSC::ProgramState::Idle)) == StatusKind::Idle);

    // The leaf carries what the node above it folds.
    PaneStatusFacts blocked = withProgram(OSC::ProgramState::Blocked);
    blocked.blockKind = OSC::ProgramBlockKind::Permission;
    blocked.indicatorColor = ColorRGBA { 1, 2, 3 };
    blocked.tabColor = ColorRGBA { 4, 5, 6 };
    blocked.hasBadge = true;
    const RolledStatus leaf = App::paneStatus(blocked);
    CHECK(leaf.kind == StatusKind::Blocked && leaf.count == 1 && leaf.paneCount == 1 && leaf.blockKind == OSC::ProgramBlockKind::Permission);
    CHECK(leaf.indicatorColor && leaf.indicatorColor->blue == 3 && leaf.tabColor && leaf.tabColor->blue == 6 && leaf.badgeCount == 1);
    CHECK_MSG(App::paneStatus(withProgram(OSC::ProgramState::Done)).blockKind == OSC::ProgramBlockKind::None, "only a blocked pane says what it waits for");
}

TEST(aNodeShowsItsMostUrgentChildAndHowManyShareIt)
{
    CHECK(roll({}).kind == StatusKind::None);
    CHECK(roll({}).count == 0 && roll({}).paneCount == 0);

    PaneStatusFacts working = withProgram(OSC::ProgramState::Working);
    working.indicatorColor = ColorRGBA { 10, 10, 10 };
    PaneStatusFacts failedOne = withProgram(OSC::ProgramState::Error);
    PaneStatusFacts failedTwo = withProgram(OSC::ProgramState::Error);
    failedTwo.indicatorColor = ColorRGBA { 200, 0, 0 };
    failedTwo.tabColor = ColorRGBA { 0, 0, 200 };
    PaneStatusFacts rang = atPrompt();
    rang.hasUnseenBell = true;
    rang.hasBadge = true;

    const RolledStatus tab = roll({ App::paneStatus(working), App::paneStatus(failedOne), App::paneStatus(rang), App::paneStatus(failedTwo), App::paneStatus(atPrompt()) });
    CHECK(tab.kind == StatusKind::Failed);
    CHECK_MSG(tab.count == 2, "the count is the panes that share the most urgent kind");
    CHECK(tab.paneCount == 5);
    CHECK_MSG(tab.hasUnseenBell && !tab.hasUnseenNotification, "a bell under a failure is still reported");
    CHECK_MSG(tab.indicatorColor && tab.indicatorColor->red == 200, "the colour is the first most-urgent child's that chose one, never a calmer child's");
    CHECK(tab.tabColor && tab.tabColor->blue == 200);
    CHECK(tab.badgeCount == 1);

    // A calmer child's colour does not survive a more urgent child that has none.
    const RolledStatus plain = roll({ App::paneStatus(working), App::paneStatus(failedOne) });
    CHECK(plain.kind == StatusKind::Failed && !plain.indicatorColor);

    // What a blocked node waits for is its first blocked child's.
    PaneStatusFacts question = withProgram(OSC::ProgramState::Blocked);
    question.blockKind = OSC::ProgramBlockKind::Question;
    PaneStatusFacts permission = withProgram(OSC::ProgramState::Blocked);
    permission.blockKind = OSC::ProgramBlockKind::Permission;
    CHECK(roll({ App::paneStatus(working), App::paneStatus(question), App::paneStatus(permission) }).blockKind == OSC::ProgramBlockKind::Question);
}

TEST(progressRollsUpByTheProgressIndicatorsOwnOrder)
{
    const auto withProgress = [](std::uint8_t state, std::uint8_t value) {
        PaneStatusFacts facts;
        facts.progress = { state, value };
        return App::paneStatus(facts);
    };
    const std::vector<std::vector<OSC::ProgressSample>> cases = {
        { { OSC::ProgressNormal, 70 }, { OSC::ProgressNormal, 30 }, { OSC::ProgressNone, 0 } },
        { { OSC::ProgressNormal, 70 }, { OSC::ProgressIndeterminate, 0 }, { OSC::ProgressNormal, 10 } },
        { { OSC::ProgressPaused, 50 }, { OSC::ProgressNormal, 90 } },
        { { OSC::ProgressNormal, 5 }, { OSC::ProgressError, 0 }, { OSC::ProgressIndeterminate, 0 } },
        { { OSC::ProgressPaused, 50 }, { OSC::ProgressNone, 0 } },
        {},
    };
    for (const auto& samples : cases) {
        std::vector<RolledStatus> panes;
        for (const OSC::ProgressSample& sample : samples)
            panes.push_back(withProgress(sample.state, sample.value));
        const OSC::ProgressSample expected = OSC::aggregateProgress(samples);
        const RolledStatus node = roll(panes);
        CHECK_MSG(node.progress.state == expected.state && node.progress.value == expected.value, "the fold and aggregateProgress agree on a flat run of panes");
    }
}

TEST(aWorkingProgramShowsAsIndeterminateProgressOnlyWhereThePaneReportsNone)
{
    const auto working = std::to_underlying(StatusKind::Working);
    const OSC::ProgressSample none { OSC::ProgressNone, 0 };
    const OSC::ProgressSample shown = App::progressWithProgramStatus(none, working, -1);
    CHECK_MSG(shown.state == OSC::ProgressIndeterminate && shown.value == 0, "a working program with no report of its own and no percent shows as a spinner would");
    for (const std::int8_t percent : { std::int8_t { 0 }, std::int8_t { 40 }, std::int8_t { 100 } }) {
        const OSC::ProgressSample filled = App::progressWithProgramStatus(none, working, percent);
        CHECK_MSG(filled.state == OSC::ProgressNormal && filled.value == static_cast<std::uint8_t>(percent), "a working record's percent shows as a bar would");
    }
    for (const std::int8_t outside : { std::int8_t { -2 }, std::int8_t { 101 }, std::int8_t { 127 } }) {
        const OSC::ProgressSample unknown = App::progressWithProgramStatus(none, working, outside);
        CHECK_MSG(unknown.state == OSC::ProgressIndeterminate, "a percent outside 0 to 100 is no percent");
    }
    // The pane's own report is the more exact, whatever it says.
    for (const OSC::ProgressSample reported : { OSC::ProgressSample { OSC::ProgressNormal, 40 }, OSC::ProgressSample { OSC::ProgressPaused, 70 },
             OSC::ProgressSample { OSC::ProgressError, 0 }, OSC::ProgressSample { OSC::ProgressIndeterminate, 0 } }) {
        const OSC::ProgressSample kept = App::progressWithProgramStatus(reported, working, 70);
        CHECK_MSG(kept.state == reported.state && kept.value == reported.value, "a pane's own report wins over its program's status");
    }
    // No other status is progress.
    for (const StatusKind kind : { StatusKind::None, StatusKind::Idle, StatusKind::Running, StatusKind::Done, StatusKind::Attention, StatusKind::Blocked, StatusKind::Failed }) {
        const OSC::ProgressSample quiet = App::progressWithProgramStatus(none, std::to_underlying(kind), 50);
        CHECK_MSG(quiet.state == OSC::ProgressNone, "only working stands in for progress");
    }
}

TEST(groupingPanesIntoTabsAndWindowsDoesNotChangeWhatTheWorkspaceShows)
{
    std::vector<RolledStatus> panes;
    const OSC::ProgramState states[] = { OSC::ProgramState::None, OSC::ProgramState::Idle, OSC::ProgramState::Working, OSC::ProgramState::Done,
        OSC::ProgramState::Blocked, OSC::ProgramState::Error };
    const std::uint8_t progressStates[] = { OSC::ProgressNone, OSC::ProgressNormal, OSC::ProgressError, OSC::ProgressIndeterminate, OSC::ProgressPaused };
    // A deterministic spread of panes over every vocabulary.
    for (unsigned index = 0; index < 60; ++index) {
        PaneStatusFacts facts;
        facts.programState = states[(index * 7) % 6];
        facts.blockKind = facts.programState == OSC::ProgramState::Blocked ? static_cast<OSC::ProgramBlockKind>(1 + index % 3) : OSC::ProgramBlockKind::None;
        facts.progress = { progressStates[(index * 3) % 5], static_cast<std::uint8_t>((index * 13) % 101) };
        facts.isAtPrompt = index % 2 == 0;
        if (index % 5 == 0)
            facts.exitStatus = static_cast<int>(index % 3);
        facts.hasUnseenBell = index % 11 == 0;
        facts.hasUnseenNotification = index % 13 == 0;
        if (index % 4 == 0)
            facts.indicatorColor = ColorRGBA { static_cast<std::uint8_t>(index), 0, 0 };
        if (index % 6 == 0)
            facts.tabColor = ColorRGBA { 0, static_cast<std::uint8_t>(index), 0 };
        facts.hasBadge = index % 9 == 0;
        panes.push_back(App::paneStatus(facts));
    }
    // Every prefix, so each kind gets to be the most urgent of some set.
    for (std::size_t paneCount = 0; paneCount <= panes.size(); ++paneCount) {
        const std::vector<RolledStatus> subset(panes.begin(), panes.begin() + static_cast<std::ptrdiff_t>(paneCount));
        const RolledStatus flat = roll(subset);
        for (const std::size_t tabSize : { std::size_t { 1 }, std::size_t { 2 }, std::size_t { 3 }, std::size_t { 7 } }) {
            std::vector<RolledStatus> windows;
            std::vector<RolledStatus> tabs;
            for (std::size_t first = 0; first < subset.size(); first += tabSize) {
                const std::size_t last = std::min(subset.size(), first + tabSize);
                tabs.push_back(roll({ subset.begin() + static_cast<std::ptrdiff_t>(first), subset.begin() + static_cast<std::ptrdiff_t>(last) }));
                if (tabs.size() == 2) {
                    windows.push_back(roll(tabs));
                    tabs.clear();
                }
            }
            if (!tabs.empty())
                windows.push_back(roll(tabs));
            CHECK_MSG(isSame(roll(windows), flat), "a workspace rolled from windows of tabs shows what its panes rolled flat do");
        }
    }
}

UBIQUITTY_TEST_MAIN()
