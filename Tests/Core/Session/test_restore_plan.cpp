// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_restore_plan — a captured tab folded into what opening it requires.
//
// Two halves fail differently. The classification decides whether a window
// rejoins a running session or starts a new shell, and getting it wrong either
// strands a session nobody can reach or opens a second shell beside one still
// running. The transport decides whether the window reaches the daemon at all,
// and it is derived rather than stored precisely so a layout cannot pin itself
// to a carrier that has since moved.
//
// The fields a plan leaves empty matter as much as the ones it fills: a fresh
// tab that came back carrying a session id would reattach to a session the
// store only believed was live.

#include "Harness.h"

#include "Core/Session/RestorePlan.h"

#include <optional>
#include <string>
#include <vector>

namespace {

namespace S = Ubiquitty::Session;

S::RestoredTab capturedTab()
{
    S::RestoredTab tab;
    tab.sessionID = 42;
    tab.bridgeCommand = "sh -c 'SOCK=\"\"; exec ubiquitty-host --attach'";
    tab.socketName = "ubq-9911";
    tab.hostLabel = S::localHostLabel;
    tab.paneLayout = "abc,80x24,0,0{40x24,0,0,1,39x24,41,0,2}";
    return tab;
}

} // namespace

// Both halves present is the only shape that reattaches.
// The classification at each corner of its two-bool domain, apart from the plan
// that reads it: reattach asks for both a live session and a way to reach it.
TEST(testTheClassificationReattachesOnlyWithBothInputs)
{
    CHECK(S::classifyRestoreTab(true, true) == S::RestoreAction::Reattach);
    CHECK(S::classifyRestoreTab(true, false) == S::RestoreAction::FreshLocal);
    CHECK(S::classifyRestoreTab(false, true) == S::RestoreAction::FreshLocal);
    CHECK(S::classifyRestoreTab(false, false) == S::RestoreAction::FreshLocal);
}

TEST(anUpdateRelaunchRequiresEveryLocalPaneAndTheNewHostToBeSettled)
{
    S::UpdateRelaunchFacts facts;
    facts.localPaneCount = 2;
    facts.adoptedLocalPaneCount = 2;
    facts.attachedLocalPaneCount = 2;
    facts.movedLocalPaneCount = 2;
    facts.isNewHostVerified = true;
    facts.isNewHostInstalled = true;
    facts.isNewHostAnswering = true;
    CHECK(S::mayInstallUpdateAndRelaunch(facts));

    facts.adoptedLocalPaneCount = 1;
    CHECK(!S::mayInstallUpdateAndRelaunch(facts));
    facts.adoptedLocalPaneCount = 2;
    facts.attachedLocalPaneCount = 1;
    CHECK(!S::mayInstallUpdateAndRelaunch(facts));
    facts.attachedLocalPaneCount = 2;
    facts.movedLocalPaneCount = 1;
    CHECK(!S::mayInstallUpdateAndRelaunch(facts));
    facts.movedLocalPaneCount = 2;
    facts.unresolvedAdoptionCount = 1;
    CHECK(!S::mayInstallUpdateAndRelaunch(facts));
    facts.unresolvedAdoptionCount = 0;
    facts.olderHostCount = 1;
    CHECK(!S::mayInstallUpdateAndRelaunch(facts));
    facts.olderHostCount = 0;
    facts.pendingSessionCount = 1;
    CHECK(!S::mayInstallUpdateAndRelaunch(facts));
    facts.pendingSessionCount = 0;
    facts.isNewHostAnswering = false;
    CHECK(!S::mayInstallUpdateAndRelaunch(facts));
    facts.isNewHostAnswering = true;
    facts.isNewHostInstalled = false;
    CHECK(!S::mayInstallUpdateAndRelaunch(facts));
    facts.isNewHostInstalled = true;
    facts.isNewHostVerified = false;
    CHECK(!S::mayInstallUpdateAndRelaunch(facts));
}

TEST(anUpdateRelaunchWarningNamesTheTerminalsAtRisk)
{
    CHECK(S::updateRelaunchRiskDetail(1).find("1 terminal") != std::string::npos);
    CHECK(S::updateRelaunchRiskDetail(2).find("2 terminals") != std::string::npos);
    CHECK(S::updateRelaunchRiskDetail(1).find("next time you quit") != std::string::npos);
    CHECK(S::updateRelaunchRiskDetail(1, "tmux").find("1 terminal running tmux") != std::string::npos);
    CHECK(S::updateRelaunchRiskDetail(2, "tmux").find("One is running tmux.") != std::string::npos);
    CHECK(S::updateRelaunchRiskDetail(1, "tmux\nRestart Anyway").find("\n") == std::string::npos);
}

TEST(aFailedUpdateTransferInstallsOnQuitUnlessThePersonAcceptsTheRisk)
{
    S::UpdateRelaunchFacts facts;
    facts.localPaneCount = 2;
    facts.adoptedLocalPaneCount = 1;
    CHECK(S::updateRelaunchAction(facts, S::UpdateRiskChoice::Unasked) == S::UpdateRelaunchAction::AskBeforeRestart);
    CHECK(S::updateRelaunchAction(facts, S::UpdateRiskChoice::NotNow) == S::UpdateRelaunchAction::InstallOnQuit);
    CHECK(S::updateRelaunchAction(facts, S::UpdateRiskChoice::RestartAnyway) == S::UpdateRelaunchAction::InstallNow);
    facts.unresolvedAdoptionCount = 1;
    CHECK(S::updateRelaunchAction(facts, S::UpdateRiskChoice::RestartAnyway) == S::UpdateRelaunchAction::InstallOnQuit);
}

TEST(testATabReattachesOnlyWithBothASessionAndAWayToReachIt)
{
    const S::RestoredTab both = capturedTab();
    CHECK(S::planRestoredTab(both, "/bin/host").action == S::RestoreAction::Reattach);

    S::RestoredTab noBridge = capturedTab();
    noBridge.bridgeCommand.clear();
    CHECK(S::planRestoredTab(noBridge, "/bin/host").action == S::RestoreAction::FreshLocal);

    S::RestoredTab noSession = capturedTab();
    noSession.sessionID = 0;
    CHECK(S::planRestoredTab(noSession, "/bin/host").action == S::RestoreAction::FreshLocal);

    S::RestoredTab neither;
    CHECK(S::planRestoredTab(neither, "/bin/host").action == S::RestoreAction::FreshLocal);
}

// A fresh tab must carry nothing that would make a window try to rejoin: an id
// left in place would re-home to a session the store only believed was live.
TEST(testAFreshTabCarriesNoSessionNoBridgeAndNoTransport)
{
    S::RestoredTab tab = capturedTab();
    tab.sessionID = 0;

    const S::RestoredTabPlan plan = S::planRestoredTab(tab, "/bin/host");

    CHECK(plan.sessionID == 0);
    CHECK(plan.bridgeCommand.empty());
    CHECK(plan.transport.quicHost.empty());
    CHECK(plan.transport.quicBootstrapCommand.empty());
    // The layout still crosses: a fresh tab keeps its splits.
    CHECK(plan.paneLayout == tab.paneLayout);
}

// The bridge is pinned to the socket the session lives on now, not the one it
// was captured against — an updated daemon keeps serving its own sessions from
// the socket it started on.
TEST(testTheBridgeIsPinnedToTheLiveSocket)
{
    const S::RestoredTabPlan plan = S::planRestoredTab(capturedTab(), "/bin/host");

    CHECK(plan.bridgeCommand != capturedTab().bridgeCommand);
    CHECK(plan.bridgeCommand.find("ubq-9911") != std::string::npos);
}

// A tab with no socket to pin to keeps the command it was captured with, rather
// than losing the only way it has to reach the daemon.
TEST(testNoSocketLeavesTheCapturedCommandAlone)
{
    S::RestoredTab tab = capturedTab();
    tab.socketName.clear();

    const S::RestoredTabPlan plan = S::planRestoredTab(tab, "/bin/host");

    CHECK(plan.action == S::RestoreAction::Reattach);
    CHECK(!plan.bridgeCommand.empty());
}

// The transport comes from the host, so a local tab and a remote one reach
// their daemons the way a fresh connect to each would.
TEST(testTheTransportFollowsTheHost)
{
    const S::RestoredTabPlan local = S::planRestoredTab(capturedTab(), "/bin/host");
    CHECK(local.hostLabel == S::localHostLabel);

    S::RestoredTab remote = capturedTab();
    remote.hostLabel = "someone@elsewhere";
    const S::RestoredTabPlan remotePlan = S::planRestoredTab(remote, "/bin/host");

    CHECK(remotePlan.hostLabel == "someone@elsewhere");
    // A remote tab dials its own host; a local one never dials that host.
    CHECK(remotePlan.transport.quicHost != local.transport.quicHost);
}

// A local tab saved under one app path and restored after the app moved keeps
// its saved bridge and takes the current binary's bootstrap. Both still run on
// this machine, so the restored tab's host is this machine's own daemon, whose
// clipboard writes and notifications are not asked about again on the client.
TEST(testALocalTabRestoredAfterTheAppMovedStaysOnThisMachine)
{
    S::RestoredTab tab = capturedTab();
    tab.bridgeCommand = S::localBridgeCommand("'/Applications/Old Place/Ubiquitty.app/Contents/Helpers/ubiquitty-host'");
    const S::RestoredTabPlan plan = S::planRestoredTab(tab, "'/Applications/Ubiquitty.app/Contents/Helpers/ubiquitty-host'");
    REQUIRE(plan.action == S::RestoreAction::Reattach);
    CHECK_MSG(S::isLocalTransportPlan(plan.bridgeCommand, plan.transport.quicHost, plan.transport.quicBootstrapCommand),
        "a local tab restored after the app moved was placed on another machine");
    S::RestoredTab remote = tab;
    remote.hostLabel = "someone@elsewhere";
    remote.bridgeCommand = S::remoteTransportPlan("someone@elsewhere").shellCommand;
    const S::RestoredTabPlan remotePlan = S::planRestoredTab(remote, "'/Applications/Ubiquitty.app/Contents/Helpers/ubiquitty-host'");
    CHECK(!S::isLocalTransportPlan(remotePlan.bridgeCommand, remotePlan.transport.quicHost, remotePlan.transport.quicBootstrapCommand));
}

// An unlabelled tab is this machine's. Stored layouts predate the label, and
// reading an empty one as a remote host would send a local reattach over ssh
// to a host named by the empty string.
TEST(testAnUnlabelledTabIsLocal)
{
    S::RestoredTab tab = capturedTab();
    tab.hostLabel.clear();

    CHECK(S::planRestoredTab(tab, "/bin/host").hostLabel == S::localHostLabel);
}

// ---- the capture: one live tab folded into what the store keeps ----
//
// The direction a quit depends on. A tab the capture drops is a window the next
// launch never opens, and a tab it stores with the wrong session either
// strands a shell the daemon is keeping or rejoins one the reader never had.

namespace {

S::TabCaptureFacts localShell()
{
    S::TabCaptureFacts facts;
    facts.isShareableLocalSession = true;
    return facts;
}

S::TabCaptureFacts thinClient()
{
    S::TabCaptureFacts facts;
    facts.isRemote = true;
    facts.remoteSessionID = 77;
    facts.remoteHostLabel = "someone@elsewhere";
    facts.remoteBridgeCommand = "ssh someone@elsewhere ubiquitty-host --attach";
    facts.remoteSocketName = "ubq-4242";
    return facts;
}

S::CaptureConditions atQuit()
{
    return S::CaptureConditions { .occasion = S::CaptureOccasion::Quit, .isLocalDaemonReady = true };
}

S::CaptureConditions savingAWorkspace()
{
    return S::CaptureConditions { .occasion = S::CaptureOccasion::SavedWorkspace, .isLocalDaemonReady = true };
}

const S::LocalDaemonTarget localDaemon { .bridgeCommand = "sh -c 'SOCK=\"\"; exec /bin/host --attach'", .socketName = "ubq-1111", .socketPath = "/tmp/ubq-1111" };

} // namespace

TEST(anUpdateReclaimsOnlyTheExactFormerlyLocalSessions)
{
    const auto facts = localShell();
    const auto action = S::planCapturedTab(facts, { .occasion = S::CaptureOccasion::UpdateRelaunch, .isLocalDaemonReady = true });
    REQUIRE(action == S::TabCaptureAction::ReHomes);
    auto forUpdate = S::storedTab(facts, action, localDaemon, 42);
    forUpdate.reclaimSessionIDsAfterUpdate = { 42, 44 };
    CHECK(S::planRestoredTab(forUpdate, "/bin/host").reclaimSessionIDsAfterUpdate == std::vector<std::uint32_t>({ 42, 44 }));
    forUpdate.hostLabel = "remote";
    CHECK(S::planRestoredTab(forUpdate, "/bin/host").reclaimSessionIDsAfterUpdate.empty());
}

// A tmux pane's view is not a session of its own, whatever else it looks like:
// storing one would open a second, unrelated shell where the pane was.
TEST(testARenderOnlyTabIsNeverStored)
{
    S::TabCaptureFacts facts = thinClient();
    facts.isRenderOnly = true;
    facts.paneLayout = "0000,80x24,0,0{40x24,0,0,1,39x24,41,0,2}";
    facts.isShareableLocalSession = true;

    CHECK(S::planCapturedTab(facts, atQuit()) == S::TabCaptureAction::Excluded);
    CHECK(S::planCapturedTab(facts, savingAWorkspace()) == S::TabCaptureAction::Excluded);
}

TEST(aHostedReferenceKeepsItsTabWithoutLaunchingLocalPanes)
{
    S::TabCaptureFacts facts;
    facts.isRenderOnly = true;
    facts.hasHostedReference = true;
    facts.paneLayout = "hosted layout";
    CHECK(S::planCapturedTab(facts, atQuit()) == S::TabCaptureAction::KeepsHostedSession);
    CHECK(S::planCapturedTab(facts, savingAWorkspace()) == S::TabCaptureAction::KeepsHostedSession);
    CHECK(S::storedTab(facts, S::TabCaptureAction::KeepsHostedSession, {}, 0).paneLayout.empty());
}

// A split the daemon cannot hold whole keeps its arrangement and nothing else:
// its panes restore as fresh shells, so a pane handed to the daemon would be a
// session nobody rejoins.
TEST(testASplitTabKeepsItsLayoutAndNoSession)
{
    S::TabCaptureFacts facts = localShell();
    facts.paneLayout = "0000,80x24,0,0{40x24,0,0,1,39x24,41,0,2}";
    facts.isShareableLocalSession = false;

    const S::TabCaptureAction action = S::planCapturedTab(facts, atQuit());
    CHECK(action == S::TabCaptureAction::FreshWithLayout);

    const S::RestoredTab stored = S::storedTab(facts, action, localDaemon, 0);
    CHECK(stored.paneLayout == facts.paneLayout);
    CHECK(stored.sessionID == 0);
    CHECK(stored.bridgeCommand.empty());
    CHECK(stored.hostLabel == S::localHostLabel);
    CHECK(S::planRestoredTab(stored, "/bin/host").action == S::RestoreAction::FreshLocal);
    CHECK(S::planRestoredTab(stored, "/bin/host").paneLayout == facts.paneLayout);
}

// At quit, a split whose every pane the daemon can hold is handed over whole:
// the stored tab rejoins the first leaf's session and carries the split, so
// the restore brings every pane back where it was. Anything short of a quit
// to a daemon that is up keeps the split as a layout of fresh panes.
TEST(testAQuitHandsAWholeSplitToTheDaemon)
{
    S::TabCaptureFacts facts = localShell();
    facts.paneLayout = "0000,80x24,0,0{40x24,0,0,1,39x24,41,0,2}";

    const S::TabCaptureAction action = S::planCapturedTab(facts, atQuit());
    CHECK(action == S::TabCaptureAction::ReHomes);
    const S::RestoredTab stored = S::storedTab(facts, action, localDaemon, 31);
    CHECK(stored.sessionID == 31);
    CHECK(stored.paneLayout == facts.paneLayout);
    CHECK(stored.bridgeCommand == localDaemon.bridgeCommand);
    const S::RestoredTabPlan plan = S::planRestoredTab(stored, "/bin/host");
    CHECK(plan.action == S::RestoreAction::Reattach);
    CHECK(plan.sessionID == 31);
    CHECK(plan.paneLayout == facts.paneLayout);

    // Refused part way: no session to rejoin, and the split restores fresh.
    const S::RestoredTabPlan refused = S::planRestoredTab(S::storedTab(facts, action, localDaemon, 0), "/bin/host");
    CHECK(refused.action == S::RestoreAction::FreshLocal);
    CHECK(refused.paneLayout == facts.paneLayout);

    S::CaptureConditions notReady = atQuit();
    notReady.isLocalDaemonReady = false;
    CHECK(S::planCapturedTab(facts, notReady) == S::TabCaptureAction::FreshWithLayout);
    CHECK(S::planCapturedTab(facts, savingAWorkspace()) == S::TabCaptureAction::FreshWithLayout);
}

// A thin client keeps the session it is attached to on either occasion, with
// every fact the restore needs to reach it again.
TEST(testAThinClientKeepsItsSessionHostBridgeAndSocket)
{
    for (const S::CaptureConditions conditions : { atQuit(), savingAWorkspace() }) {
        const S::TabCaptureAction action = S::planCapturedTab(thinClient(), conditions);
        CHECK(action == S::TabCaptureAction::KeepsRemoteSession);

        const S::RestoredTab stored = S::storedTab(thinClient(), action, localDaemon, 0);
        CHECK(stored.sessionID == 77);
        CHECK(stored.hostLabel == "someone@elsewhere");
        CHECK(stored.bridgeCommand == thinClient().remoteBridgeCommand);
        CHECK(stored.socketName == "ubq-4242");
        CHECK(S::planRestoredTab(stored, "/bin/host").action == S::RestoreAction::Reattach);
    }
}

// At quit a local shell the daemon can adopt is handed over, and the stored tab
// rejoins it through this machine's daemon on the socket it went to.
TEST(testAQuitHandsAShareableLocalShellToTheDaemon)
{
    const S::TabCaptureAction action = S::planCapturedTab(localShell(), atQuit());
    CHECK(action == S::TabCaptureAction::ReHomes);

    const S::RestoredTab stored = S::storedTab(localShell(), action, localDaemon, 31);
    CHECK(stored.sessionID == 31);
    CHECK(stored.hostLabel == S::localHostLabel);
    CHECK(stored.bridgeCommand == localDaemon.bridgeCommand);
    CHECK(stored.socketName == "ubq-1111");

    const S::RestoredTabPlan plan = S::planRestoredTab(stored, "/bin/host");
    CHECK(plan.action == S::RestoreAction::Reattach);
    CHECK(plan.sessionID == 31);
}

// Saving a workspace leaves the live windows alone: the same shell that a quit
// would hand over stays where it is and restores as a fresh one.
TEST(testSavingAWorkspaceHandsNothingOver)
{
    CHECK(S::planCapturedTab(localShell(), savingAWorkspace()) == S::TabCaptureAction::FreshLocal);

    S::CaptureConditions notReady = atQuit();
    notReady.isLocalDaemonReady = false;
    CHECK(S::planCapturedTab(localShell(), notReady) == S::TabCaptureAction::FreshLocal);

    S::TabCaptureFacts busy = localShell();
    busy.isShareableLocalSession = false;
    CHECK(S::planCapturedTab(busy, atQuit()) == S::TabCaptureAction::FreshLocal);
}

// A fresh tab is still a stored tab — the window, its place and its tags come
// back — and it carries nothing that would make the restore try to rejoin.
TEST(testAFreshLocalTabIsStoredAndCarriesNothingToRejoin)
{
    const S::TabCaptureAction action = S::planCapturedTab(localShell(), savingAWorkspace());
    const S::RestoredTab stored = S::storedTab(localShell(), action, localDaemon, 99);

    CHECK(stored.sessionID == 0);
    CHECK(stored.bridgeCommand.empty());
    CHECK(stored.socketName.empty());
    CHECK(stored.paneLayout.empty());
    CHECK(stored.hostLabel == S::localHostLabel);
    CHECK(S::planRestoredTab(stored, "/bin/host").action == S::RestoreAction::FreshLocal);
}

// A window already shared into this machine's daemon is a thin client of it:
// kept on either occasion, and rejoined through the local transport rather
// than dialled as a host of that name.
TEST(testASharedLocalTabRejoinsThroughThisMachinesDaemon)
{
    S::TabCaptureFacts facts;
    facts.isRemote = true;
    facts.remoteSessionID = 42;
    facts.remoteHostLabel = S::localHostLabel;
    facts.remoteBridgeCommand = S::localBridgeCommand("/bin/host");
    facts.remoteSocketName = "host-0123456789abcdef.sock";

    for (const S::CaptureConditions conditions : { atQuit(), savingAWorkspace() }) {
        const S::TabCaptureAction action = S::planCapturedTab(facts, conditions);
        CHECK(action == S::TabCaptureAction::KeepsRemoteSession);
        const S::RestoredTabPlan plan = S::planRestoredTab(S::storedTab(facts, action, localDaemon, 0), "/bin/host");
        CHECK(plan.action == S::RestoreAction::Reattach);
        CHECK(plan.sessionID == 42);
        CHECK(plan.bridgeCommand.find("SOCK=\"host-0123456789abcdef.sock\";") != std::string::npos);
        CHECK(plan.transport.quicHost == S::localTransportPlan("/bin/host").quicHost);
    }
}

// A daemon that refused the shell leaves a bridge and no session, which the
// restore reads as a fresh shell rather than a reattach to session zero.
TEST(testARefusedReHomeRestoresFresh)
{
    const S::RestoredTab stored = S::storedTab(localShell(), S::TabCaptureAction::ReHomes, localDaemon, 0);

    CHECK(stored.sessionID == 0);
    CHECK(S::planRestoredTab(stored, "/bin/host").action == S::RestoreAction::FreshLocal);
}

// ---- the capture at quit, walked over many shells ----
//
// A daemon that accepts and never answers costs each offer the whole handoff
// deadline, and the quit waits on every one of them.

namespace {

// Walks a quit capture the way the frontend does, with a daemon that answers
// every offer with `answer`, and counts the offers it makes.
struct WalkedQuit {
    std::vector<S::TabCaptureAction> actions;
    int offers = 0;
};

WalkedQuit walkQuit(const std::vector<S::TabCaptureFacts>& tabs, std::uint32_t answer)
{
    WalkedQuit walk;
    S::CaptureConditions conditions = atQuit();
    for (const S::TabCaptureFacts& facts : tabs) {
        const S::TabCaptureAction action = S::planCapturedTab(facts, conditions);
        if (action == S::TabCaptureAction::ReHomes) {
            ++walk.offers;
            conditions = S::conditionsAfterHandOff(conditions, answer);
        }
        walk.actions.push_back(action);
    }
    return walk;
}

S::QuitConditions persistentQuit()
{
    return S::QuitConditions { .isSessionPersistent = true, .isLocalDaemonReady = true };
}

} // namespace

// After a refusal the quit offers the daemon nothing more: one deadline, not
// one per window, and every later shell restores fresh with its window kept.
TEST(testARefusedHandOffEndsTheQuitsOffers)
{
    const WalkedQuit walk = walkQuit({ localShell(), localShell(), thinClient(), localShell() }, 0);

    CHECK(walk.offers == 1);
    CHECK(walk.actions.size() == 4);
    CHECK(walk.actions[0] == S::TabCaptureAction::ReHomes);
    CHECK(walk.actions[1] == S::TabCaptureAction::FreshLocal);
    CHECK(walk.actions[2] == S::TabCaptureAction::KeepsRemoteSession);
    CHECK(walk.actions[3] == S::TabCaptureAction::FreshLocal);
}

// A daemon that takes each shell is offered every one.
TEST(testAnAnsweringDaemonIsOfferedEveryShell)
{
    const WalkedQuit walk = walkQuit({ localShell(), localShell(), localShell() }, 12);

    CHECK(walk.offers == 3);
    for (const S::TabCaptureAction action : walk.actions)
        CHECK(action == S::TabCaptureAction::ReHomes);
}

// ---- the quit's confirmation: which tabs a quit ends ----
//
// The confirmation warns that a quit ends running processes. It has to agree
// with what the capture then does, or it warns about a shell the daemon is
// about to keep, or stays silent over one the quit kills.

// Without a persistent session nothing is captured, so every tab ends.
TEST(testWithoutPersistenceAQuitEndsEveryTab)
{
    const S::QuitConditions quit { .isSessionPersistent = false, .isLocalDaemonReady = true };

    CHECK(!S::isTabKeptRunningByQuit(localShell(), quit));
    CHECK(!S::isTabKeptRunningByQuit(thinClient(), quit));
}

// With one, a shell the daemon adopts and a thin client's session outlive the
// quit, and so does a view of panes another authority owns.
TEST(testAPersistentQuitKeepsWhatItHandsOverOrReattaches)
{
    CHECK(S::isTabKeptRunningByQuit(localShell(), persistentQuit()));
    CHECK(S::isTabKeptRunningByQuit(thinClient(), persistentQuit()));

    S::TabCaptureFacts view = localShell();
    view.isRenderOnly = true;
    CHECK(S::isTabKeptRunningByQuit(view, persistentQuit()));
}

// A split the daemon holds whole outlives the quit; one it cannot restores
// fresh, and so does a shell no daemon can take: the quit ends what runs in
// them, whatever the persistence setting says.
TEST(testAPersistentQuitStillEndsSplitsAndShellsNoDaemonTakes)
{
    S::TabCaptureFacts split = localShell();
    split.paneLayout = "0000,80x24,0,0{40x24,0,0,1,39x24,41,0,2}";
    CHECK(S::isTabKeptRunningByQuit(split, persistentQuit()));
    split.isShareableLocalSession = false;
    CHECK(!S::isTabKeptRunningByQuit(split, persistentQuit()));

    S::QuitConditions noDaemon = persistentQuit();
    noDaemon.isLocalDaemonReady = false;
    CHECK(!S::isTabKeptRunningByQuit(localShell(), noDaemon));
    CHECK(S::isTabKeptRunningByQuit(thinClient(), noDaemon));

    S::TabCaptureFacts unshareable = localShell();
    unshareable.isShareableLocalSession = false;
    CHECK(!S::isTabKeptRunningByQuit(unshareable, persistentQuit()));
}

// ---- whether a quit keeps the session at all ----
//
// The person decides with session persistence, except at the one quit they did
// not ask for: the updater's, to open the copy it just installed. Each corner
// of the two inputs is pinned, because the two failures differ: a relaunch
// that keeps nothing ends every local job for an update, and an ordinary quit
// that keeps everything restores an arrangement the reader chose not to keep.

namespace {

Ubiquitty::Preferences::Values persistence(std::optional<bool> isOn)
{
    Ubiquitty::Preferences::Values values;
    values.sessionPersistence = isOn;
    return values;
}

} // namespace

TEST(testAnUpdateRelaunchKeepsTheSessionWhateverPersistenceSays)
{
    for (const std::optional<bool> setting : { std::optional<bool>(), std::optional<bool>(false), std::optional<bool>(true) })
        CHECK(S::isSessionKeptByQuit(persistence(setting), S::QuitReason::UpdateRelaunch));
}

TEST(testAnOrdinaryQuitKeepsTheSessionOnlyWithPersistenceOn)
{
    CHECK(!S::isSessionKeptByQuit(persistence(std::nullopt), S::QuitReason::Ordinary));
    CHECK(!S::isSessionKeptByQuit(persistence(false), S::QuitReason::Ordinary));
    CHECK(S::isSessionKeptByQuit(persistence(true), S::QuitReason::Ordinary));
}

// The rule feeds the confirmation: an update relaunch with persistence off
// keeps a shell the daemon takes, so its jobs are nothing to warn about, while
// an ordinary quit under the same setting ends them.
TEST(testAnUpdateRelaunchWarnsAboutNothingTheDaemonKeeps)
{
    const Ubiquitty::Preferences::Values off = persistence(false);
    const S::QuitConditions relaunch { .isSessionPersistent = S::isSessionKeptByQuit(off, S::QuitReason::UpdateRelaunch), .isLocalDaemonReady = true };
    const S::QuitConditions ordinary { .isSessionPersistent = S::isSessionKeptByQuit(off, S::QuitReason::Ordinary), .isLocalDaemonReady = true };

    CHECK(S::isTabKeptRunningByQuit(localShell(), relaunch));
    CHECK(!S::isTabKeptRunningByQuit(localShell(), ordinary));

    S::QuitConditions noDaemon = relaunch;
    noDaemon.isLocalDaemonReady = false;
    CHECK_MSG(!S::isTabKeptRunningByQuit(localShell(), noDaemon), "an update relaunch with no daemon up kept a shell nothing holds");
}

UBIQUITTY_TEST_MAIN()
