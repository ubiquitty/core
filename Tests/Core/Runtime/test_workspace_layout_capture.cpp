// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_workspace_layout_capture — a window as session restore stores it
// (Core/Runtime/WorkspaceSession.h `captureLayoutAtSessionGrids`,
// `tabCaptureFacts`, `handSurfaceToLocalDaemon`).
//
// The stored string is what a restored window rebuilds its panes from, so the
// two claims are the ones a restore depends on: a window with nothing to
// rebuild stores nothing, and a split one stores an arrangement the restore
// reads back as the same split. The extents come from each pane's own session
// rather than a constant, because the ratios are re-derived from them — a
// capture that measured every pane the same would bring back an even split
// the reader never had.
//
// And the facts the capture decides a tab by: a lone running shell is one a
// quit can hand to the daemon, a split is one it hands over whole or not at
// all, and a window already shared into the daemon is stored as the session
// the restore rejoins — stored as a fresh shell, its shell would run on in the
// daemon with no window.

#include "Core/Runtime/WorkspaceSession.h"
#include "Core/Session/AdoptTransaction.h"
#include "Core/Session/RestorePlan.h"
#include "Core/Session/SessionShare.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Ubiquitty::Runtime;
namespace S = Ubiquitty::Session;

namespace {

std::vector<PostedWorkToken>& postedWork()
{
    static std::vector<PostedWorkToken> queue;
    return queue;
}

void queuePostedWork(PostedWorkToken token) { postedWork().push_back(token); }
void queueDelayedWork(double, PostedWorkToken token) { postedWork().push_back(token); }

PostedWorkLoop queueingLoop() { return { .runSoon = &queuePostedWork, .runAfter = &queueDelayedWork }; }

void discardPostedWork()
{
    for (const PostedWorkToken token : postedWork())
        discardPostedSessionWork(token);
    postedWork().clear();
}

/// How many times the stand-in for the platform's handoff ran, and what the
/// daemon it plays answers: `answer` to the first offer and the next id to
/// each after it, as a daemon numbers the sessions it adopts, until the offer
/// numbered `refusedFromCall`, which it refuses with every one after.
struct HandoffRecord {
    int callCount = 0;
    std::uint32_t answer = 0;
    int refusedFromCall = 0;
};

HandoffRecord& handoffRecord()
{
    static HandoffRecord record;
    return record;
}

/// What a completed exchange returns: the daemon reserved `sessionID` on
/// Prepare, the Commit was answered Committed, and the attempt that proves it
/// is handed back with the ownership the session checks it against.
S::ShareHandoffResult committedHandoff(std::uint32_t sessionID)
{
    const S::AdoptOperation operation { .operationID = { .low = 0x51, .high = 0x52 }, .daemonInstanceID = 17 };
    const S::AdoptTarget target { .sessionID = sessionID, .paneID = { .low = 0x61, .high = 0x62 } };
    auto attempt = std::make_unique<S::AdoptClientAttempt>(operation);
    if (!attempt->observe({ .operation = operation, .disposition = S::AdoptDisposition::Prepared, .target = target })
        || !attempt->beginCommit()
        || !attempt->observe({ .operation = operation, .disposition = S::AdoptDisposition::Committed, .target = target }))
        return {};
    return { .ownership = S::ShareOwnership::Committed, .adopted = { .sessionID = sessionID, .daemonInstanceID = 17 }, .attempt = std::move(attempt) };
}

S::ShareHandoffResult recordHandoff(const char*, std::span<const std::byte>, int, const S::ShareHandoffLimits&)
{
    ++handoffRecord().callCount;
    // A daemon that answers a session has committed the master; one that
    // answers nothing leaves the local endpoint its own, as a channel that
    // could not be selected does.
    const HandoffRecord& record = handoffRecord();
    if (record.answer == 0 || (record.refusedFromCall != 0 && record.callCount >= record.refusedFromCall))
        return {};
    return committedHandoff(record.answer + static_cast<std::uint32_t>(record.callCount - 1));
}

std::uintptr_t handoffAddress()
{
    const S::AdoptHandoff handoff = &recordHandoff;
    return reinterpret_cast<std::uintptr_t>(handoff);
}

S::CaptureConditions atQuit()
{
    return S::CaptureConditions { .occasion = S::CaptureOccasion::Quit, .isLocalDaemonReady = true };
}

// Releases the model and every session it retains.
struct ModelHandle {
    WorkspaceSession* model = nullptr;
    ModelHandle()
        : model(WorkspaceSession::make(WorkspaceSession::IngestPolicy::CallerDrained, DrawnText::SizedAndProportional))
    {
    }
    ~ModelHandle()
    {
        if (model != nullptr)
            releaseWorkspaceSession(model);
    }
};

} // namespace

TEST(testAnUnsplitWindowStoresNoLayout)
{
    ModelHandle handle;
    WorkspaceSession& model = *handle.model;

    const auto surface = model.addWindowSurface();
    CHECK(surface != Ubiquitty::Session::Layout::invalidSurface);
    CHECK(model.captureLayoutAtSessionGrids(surface).empty());
}

TEST(testASplitWindowStoresALayoutTheRestoreRebuilds)
{
    ModelHandle handle;
    WorkspaceSession& model = *handle.model;

    const auto surface = model.addWindowSurface();
    const auto split = model.splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Vertical);
    CHECK(split.createdPane != Ubiquitty::Session::Layout::invalidPane);

    const std::string layout = model.captureLayoutAtSessionGrids(surface);
    CHECK(layout.starts_with("0000,"));

    // Restored by the next launch's model, which holds none of these panes.
    ModelHandle relaunched;
    const auto restored = relaunched.model->addWindowSurfaceRestoring(layout);
    CHECK(restored != Ubiquitty::Session::Layout::invalidSurface);
    CHECK(relaunched.model->paneCount(restored) == 2);
}

// Each leaf is measured by the session behind it: a pane resized on its own
// is stored at its own grid, which is what the restore's ratio comes from.
TEST(testEachPaneIsStoredAtItsOwnSessionsGrid)
{
    ModelHandle handle;
    WorkspaceSession& model = *handle.model;

    const auto surface = model.addWindowSurface();
    const auto split = model.splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Vertical);
    TerminalSession* created = model.session(split.createdPane);
    CHECK(created != nullptr);
    if (created == nullptr)
        return;
    created->resizeGrid(11, 37, Ubiquitty::Modes::ReflowMode::Fixed);

    const std::string layout = model.captureLayoutAtSessionGrids(surface);
    CHECK(layout.find("37x11") != std::string::npos);
}

// A lone running shell is what a quit hands the daemon, and so is a split,
// whole: every pane in leaf order, with the first leaf's session the one a
// restore attaches through. A split already held, by a quit that was then
// cancelled, is handed over again by offering nothing.
TEST(testALoneShellAndAWholeSplitAreHandedToTheDaemon)
{
    ModelHandle handle;
    WorkspaceSession& model = *handle.model;

    const auto lone = model.addWindowSurface();
    REQUIRE(lone != Ubiquitty::Session::Layout::invalidSurface);
    const S::TabCaptureFacts loneFacts = model.tabCaptureFacts(lone);
    CHECK(loneFacts.isShareableLocalSession);
    CHECK(!loneFacts.isRemote);
    CHECK(loneFacts.paneLayout.empty());
    CHECK(S::planCapturedTab(loneFacts, atQuit()) == S::TabCaptureAction::ReHomes);

    const auto split = model.addWindowSurface();
    REQUIRE(split != Ubiquitty::Session::Layout::invalidSurface);
    (void)model.splitFocusedPane(split, Ubiquitty::Panes::Orientation::Vertical);
    const S::TabCaptureFacts splitFacts = model.tabCaptureFacts(split);
    CHECK(splitFacts.isShareableLocalSession);
    CHECK(!splitFacts.isRemote);
    CHECK(!splitFacts.paneLayout.empty());
    CHECK(S::planCapturedTab(splitFacts, atQuit()) == S::TabCaptureAction::ReHomes);

    handoffRecord() = {};
    handoffRecord().answer = 57;
    const std::uint32_t splitSession = model.handSurfaceToLocalDaemon(split, "/tmp/ubq-layout-capture.sock", handoffAddress());
    CHECK_MSG(handoffRecord().callCount == 2, "a split was handed over in part");
    const S::SharedWorkspace held = model.captureSharedSurface(split);
    REQUIRE(held.sessions.size() == 2);
    CHECK_EQ(held.sessions[0], 57u);
    CHECK_EQ(held.sessions[1], 58u);
    CHECK_EQ(splitSession, held.sessions.front());
    CHECK(model.isSurfaceHeldByLocalDaemon(split));
    const S::TabCaptureFacts heldFacts = model.tabCaptureFacts(split);
    CHECK_MSG(heldFacts.isRemote, "a split the daemon holds whole was taken for local shells");
    CHECK_MSG(S::planCapturedTab(heldFacts, atQuit()) == S::TabCaptureAction::ReHomes, "a held split is no longer kept whole by the next quit");
    CHECK_EQ(model.handSurfaceToLocalDaemon(split, "/tmp/ubq-layout-capture.sock", handoffAddress()), splitSession);
    CHECK_MSG(handoffRecord().callCount == 2, "a pane the daemon holds was offered to it again");

    CHECK_EQ(model.handSurfaceToLocalDaemon(lone, "/tmp/ubq-layout-capture.sock", handoffAddress()), 59u);
    CHECK_EQ(handoffRecord().callCount, 3);
    CHECK_MSG(!model.tabCaptureFacts(lone).isShareableLocalSession, "a shell handed over is offered to the daemon again");
    discardPostedWork();
}

// A daemon that refuses a pane part way holds the split only in part: the walk
// offers it nothing after the refusal, names no session to rejoin, and the tab
// is no thin client, so the panes still local count as ending with the quit.
TEST(testASplitTheDaemonRefusesPartWayIsNotHeld)
{
    ModelHandle handle;
    WorkspaceSession& model = *handle.model;

    const auto split = model.addWindowSurface();
    REQUIRE(split != Ubiquitty::Session::Layout::invalidSurface);
    (void)model.splitFocusedPane(split, Ubiquitty::Panes::Orientation::Vertical);
    (void)model.splitFocusedPane(split, Ubiquitty::Panes::Orientation::Horizontal);
    REQUIRE(model.paneCount(split) == 3);

    handoffRecord() = {};
    handoffRecord().answer = 70;
    handoffRecord().refusedFromCall = 2;
    CHECK_EQ(model.handSurfaceToLocalDaemon(split, "/tmp/ubq-layout-capture.sock", handoffAddress()), 0u);
    CHECK_MSG(handoffRecord().callCount == 2, "the walk offered a pane after the daemon refused one");
    CHECK(!model.isSurfaceHeldByLocalDaemon(split));
    CHECK(model.captureSharedSurface(split).sessions.empty());
    const S::TabCaptureFacts facts = model.tabCaptureFacts(split);
    CHECK_MSG(!facts.isRemote, "a split held in part was taken for a thin client");
    CHECK_EQ(TerminalSession::unresolvedLocalAdoptionCount(), 0u);
    discardPostedWork();
}

// A window shared into the daemon is stored as the adopted session, reached
// through this machine's bridge, and the restore rejoins it there.
TEST(testASharedWindowIsStoredAsTheSessionTheRestoreRejoins)
{
    const char* previous = std::getenv("UBIQUITTY_HOME");
    const std::string saved = previous ? previous : "";
    const std::string home = "/tmp/ubq-layout-" + std::to_string(::getpid());
    ::mkdir(home.c_str(), 0700);
    ::setenv("UBIQUITTY_HOME", home.c_str(), 1);

    // The binary's daemon, answering on the socket a share looks for.
    const std::string binary = "/usr/bin/true";
    const std::string socketPath = S::localDaemonSocketPath(binary);
    REQUIRE(!socketPath.empty());
    ::unlink(socketPath.c_str());
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketPath.c_str());
    REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener, 4) == 0);

    {
        ModelHandle handle;
        WorkspaceSession& model = *handle.model;
        const auto surface = model.addWindowSurface();
        REQUIRE(surface != Ubiquitty::Session::Layout::invalidSurface);
        TerminalSession* shell = model.session(model.focusedPane(surface));
        REQUIRE(shell != nullptr);

        handoffRecord() = {};
        handoffRecord().answer = 42;
        REQUIRE(shell->shareIntoLocalDaemon(binary, handoffAddress(), queueingLoop()) == S::ShareOutcome::Shared);

        const S::TabCaptureFacts facts = model.tabCaptureFacts(surface);
        CHECK_MSG(facts.isRemote, "a shared window was taken for a local shell");
        CHECK_EQ(facts.remoteSessionID, 42u);
        CHECK_STR_EQ(facts.remoteHostLabel, S::localHostLabel);
        CHECK_STR_EQ(facts.remoteBridgeCommand, S::localBridgeCommand("'/usr/bin/true'"));
        CHECK(!facts.isShareableLocalSession);

        const S::TabCaptureAction action = S::planCapturedTab(facts, atQuit());
        CHECK(action == S::TabCaptureAction::KeepsRemoteSession);
        const S::RestoredTab stored = S::storedTab(facts, action, S::LocalDaemonTarget {}, 0);
        const S::RestoredTabPlan plan = S::planRestoredTab(stored, "'/usr/bin/true'");
        CHECK_MSG(plan.action == S::RestoreAction::Reattach, "the restore opens a fresh shell where the shared one was");
        CHECK_EQ(plan.sessionID, 42u);
        CHECK_STR_EQ(plan.transport.shellCommand, S::localTransportPlan("'/usr/bin/true'").shellCommand);

        // Shared already: a quit offers nothing and names the session the
        // daemon holds.
        const int handoffsBeforeQuit = handoffRecord().callCount;
        CHECK_EQ(model.handSurfaceToLocalDaemon(surface, socketPath, handoffAddress()), 42u);
        CHECK_EQ(handoffRecord().callCount, handoffsBeforeQuit);
        discardPostedWork();
    }
    discardPostedWork();

    ::close(listener);
    ::unlink(socketPath.c_str());
    ::rmdir(home.c_str());
    if (previous)
        ::setenv("UBIQUITTY_HOME", saved.c_str(), 1);
    else
        ::unsetenv("UBIQUITTY_HOME");
}

TEST(testExactArrangementAppendsWithoutReusingLivePaneIDs)
{
    ModelHandle handle;
    auto& model = *handle.model;
    const auto original = model.addWindowSurface();
    const auto first = model.focusedPane(original);
    auto* firstSession = model.session(first);
    REQUIRE(firstSession != nullptr);
    firstSession->applyTriggerTitle("Saved pane title");
    const auto second = model.splitFocusedPane(original, Ubiquitty::Panes::Orientation::Vertical).createdPane;
    REQUIRE(second != S::Layout::invalidPane);
    REQUIRE(model.setDividerRatio(original, model.paneTree(original)->root(), 0.37f));
    model.toggleZoomFocused(original);
    REQUIRE(model.zoomedPane(original) == second);
    const auto archive = model.captureWindowArrangement(original);
    S::Workspace snapshot;
    REQUIRE(S::deserializeWorkspaceText(archive, snapshot));
    CHECK_EQ(snapshot.windows[0].tabs[0].focusedPaneIndex, 1);
    CHECK_EQ(snapshot.windows[0].zoomedPaneIndex, 1);
    CHECK_STR_EQ(snapshot.windows[0].tabs[0].root.children[0].title, "Saved pane title");

    const auto restored = model.addWindowSurfaceFromArrangement(archive);
    REQUIRE(restored != S::Layout::invalidSurface);
    CHECK(restored != original);
    CHECK(model.session(first) == firstSession);
    CHECK(model.focusedPane(restored) != first);
    CHECK(model.focusedPane(restored) != second);
    CHECK_EQ(model.zoomedPane(restored), model.focusedPane(restored));
    CHECK(model.paneTree(restored)->node(model.paneTree(restored)->root()).ratio == 0.37f);
    CHECK_EQ(model.paneCount(restored), 2);
    CHECK_EQ(model.paneCount(original), 2);
    CHECK(model.addWindowSurfaceFromArrangement("damaged archive") == S::Layout::invalidSurface);
    CHECK(model.session(first) == firstSession);
}

TEST(testRemoteArrangementNeverBecomesALocalShell)
{
    ModelHandle handle;
    const auto original = handle.model->addWindowSurface();
    const auto pane = handle.model->focusedPane(original);
    auto* originalSession = handle.model->session(pane);
    S::Workspace snapshot;
    S::WorkspaceWindow window;
    window.tabs.emplace_back();
    window.tabs[0].root.origin.transport = Ubiquitty::Transport::OriginKind::Daemon;
    snapshot.windows.push_back(std::move(window));
    CHECK(handle.model->addWindowSurfaceFromArrangement(S::serializeWorkspaceText(snapshot)) == S::Layout::invalidSurface);
    CHECK(handle.model->session(pane) == originalSession);
}

UBIQUITTY_TEST_MAIN()
