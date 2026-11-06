// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_showcase_session_tree.cpp — the showcase host holds what it says it
// holds.
//
// Frontends draw their pictures and run their scenarios from this host, so a
// kind of row it stopped holding is a kind of row no picture shows and no
// scenario clicks. Each thing its header lists is found here in the outline a
// frontend would be handed.

#include "Core/App/SessionOutline.h"
#include "Core/App/SessionOutlineText.h"
#include "Core/App/StatusRollup.h"
#include "Core/OSC/ProgressIndicator.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/ShowcaseSessionTree.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace Ubiquitty::App;
using Ubiquitty::Session::TreeWorkspaceState;
namespace OSC = Ubiquitty::OSC;

namespace {

constexpr std::uint64_t hostNow = 1'700'000'000'000;

SessionOutline showcaseOutline()
{
    const UbiquittyTest::ShowcaseHost host;
    OutlineConfiguration configuration;
    configuration.viewer.fallbackName = "shell";
    return buildSessionOutline(host.tree(hostNow), configuration);
}

template<typename Predicate>
bool anyRow(const SessionOutline& outline, Predicate&& predicate)
{
    return std::ranges::any_of(outline.rows, predicate);
}

} // namespace

TEST(theShowcaseHostHoldsEveryKindOfRowAListingDraws)
{
    const SessionOutline outline = showcaseOutline();
    if (std::getenv("UBQ_PRINT_SHOWCASE"))
        std::fputs(outlineText(outline).c_str(), stderr);

    // Every level, and a level of one child folded into the row above it.
    for (const ListedLevel level : { ListedLevel::Workspace, ListedLevel::Window, ListedLevel::Tab, ListedLevel::Pane })
        CHECK(anyRow(outline, [level](const OutlineRow& row) { return row.level == level; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.level == ListedLevel::Workspace && row.foldedPane; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.level == ListedLevel::Tab && row.foldedPane; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.level == ListedLevel::Workspace && row.isExpandable; }));

    // Every status a pane can have, on a row that is or stands for one pane.
    for (const StatusKind kind : { StatusKind::Idle, StatusKind::Running, StatusKind::Working, StatusKind::Done, StatusKind::Attention, StatusKind::Blocked, StatusKind::Failed })
        CHECK_MSG(anyRow(outline, [kind](const OutlineRow& row) { return row.status.paneCount == 1 && row.status.kind == kind; }), std::string(statusKindWord(kind)).c_str());
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.status.kind == StatusKind::Blocked && row.status.blockKind == OSC::ProgramBlockKind::Permission; }));
    CHECK_MSG(anyRow(outline, [](const OutlineRow& row) { return row.status.paneCount > 1 && row.status.count < row.status.paneCount; }), "a parent whose worst status is not all of its panes'");

    // Progress in each state, a bell, a notification, the colours and a badge.
    for (const std::uint8_t state : { OSC::ProgressNormal, OSC::ProgressError, OSC::ProgressIndeterminate, OSC::ProgressPaused })
        CHECK(anyRow(outline, [state](const OutlineRow& row) { return row.status.paneCount == 1 && row.status.progress.state == state; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.status.paneCount == 1 && row.status.hasUnseenBell; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.status.paneCount == 1 && row.status.hasUnseenNotification; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return !row.attentionTitle.empty() && !row.attentionBody.empty(); }));
    // What a row's actions read: a session to open, a workspace to restore,
    // a row that cannot be opened, a tab that may move, and a level's own
    // label and tags.
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.open.opening == Ubiquitty::Session::AttachOpening::BySession && row.open.paneID != 0; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.open.opening == Ubiquitty::Session::AttachOpening::ByRestoring; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.open.opening == Ubiquitty::Session::AttachOpening::Cannot; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.tabPlace && row.tabPlace->canMoveLater; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return std::ranges::any_of(row.own, [](const auto& level) { return !level.label.empty(); }); }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return std::ranges::any_of(row.own, [](const auto& level) { return !level.tags.empty(); }); }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.status.tabColor.has_value(); }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.status.indicatorColor.has_value(); }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return !row.badge.empty(); }));

    // Names, labels, what runs beside a name, and tags by hand.
    CHECK(anyRow(outline, [](const OutlineRow& row) { return !row.labelChips.empty(); }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return !row.secondaryTitle.empty(); }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.tags.size() > 1; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.isHostCurrent; }));

    // Ages from seconds to days, a pane never typed into, and attendance of
    // people and of an agent.
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.sinceOutput.isKnown && row.sinceOutput.seconds < 5; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.sinceOutput.isKnown && row.sinceOutput.seconds > 2 * 86'400; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.sinceOutput.isKnown && !row.sinceInput.isKnown; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.attendance.people > 1; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.attendance.agents > 0; }));

    // A workspace that ends when detached, one restored and one restorable
    // with recorded rows under it.
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.endsWhenDetached; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.level == ListedLevel::Workspace && row.workspaceState == TreeWorkspaceState::RestoredAfterHostRestart; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.level == ListedLevel::Workspace && row.workspaceState == TreeWorkspaceState::Restorable; }));
    CHECK(anyRow(outline, [](const OutlineRow& row) { return row.recordedKey.has_value(); }));
}

TEST(theShowcaseHostsAgesFollowTheClockItIsAskedAt)
{
    const UbiquittyTest::ShowcaseHost host;
    OutlineConfiguration configuration;
    const SessionOutline early = buildSessionOutline(host.tree(hostNow), configuration);
    const SessionOutline late = buildSessionOutline(host.tree(hostNow + 3'600'000), configuration);
    REQUIRE(early.rows.size() == late.rows.size() && !early.rows.empty());
    for (std::size_t index = 0; index < early.rows.size(); ++index)
        CHECK(early.rows[index].sinceOutput == late.rows[index].sinceOutput);
}

UBIQUITTY_TEST_MAIN()
