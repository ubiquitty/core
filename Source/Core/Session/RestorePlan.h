// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// One captured tab folded into what opening it requires: whether it reattaches
// a daemon session or opens a fresh shell, the bridge command pinned to the
// socket that session actually lives on, and the transport derived from its
// host. And the other direction: one live tab folded into what the session
// store keeps for it, which is the half a quit depends on — a tab the capture
// skips is a window the next launch never opens.
//
// Derived rather than stored, for the transport: a layout carries the host it
// was captured against, and resolving the transport from that host is what
// makes a restored window reach whatever a freshly connected one reaches. A
// stored transport pins the layout to whichever carrier happened to be live
// when it was written, and a QUIC daemon that has since moved port would be
// dialled at the old one forever.

#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Session/RemoteBootstrap.h"
#include "Core/Title/BannerText.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Session {

/// Why the application is quitting, which decides what the quit keeps.
enum class QuitReason : std::uint8_t {
    /// The person quit, or the system asked the application to.
    Ordinary,
    /// The updater installed a newer copy and quits the application to open
    /// it.
    UpdateRelaunch,
};

/// Whether a quit keeps the session, and whether the launch after it restores
/// what it kept.
///
/// Keeping the session hands every local shell the daemon can adopt to it and
/// saves the arrangement. Session persistence keeps it at every quit. A quit to
/// relaunch an update keeps it whatever that setting says: the person asked for
/// a newer copy of the application, not for their terminals to end, and the
/// launch the updater opens is the same minute of their work.
///
/// A launch asks the same question of the reason its saved arrangement was
/// kept for, so an arrangement only an update relaunch kept is restored with
/// session persistence off. The frontend consumes that reason at the launch
/// that reads it, so it restores one launch and no other.
///
/// - Parameters:
///   - values: The preferences in effect.
///   - reason: Why the application quits, or, at launch, why the saved
///     arrangement was kept.
/// - Returns: Whether the session is kept, or the saved arrangement restored.
[[nodiscard]] inline bool isSessionKeptByQuit(const Preferences::Values& values, QuitReason reason)
{
    return reason == QuitReason::UpdateRelaunch || Preferences::Effective::sessionPersistence(values);
}

/// The facts an update must prove before Sparkle may replace the running app.
/// A successful socket preflight is not an adoption, and a started successor
/// is not proof that it took every session the app placed in its predecessor.
struct UpdateRelaunchFacts {
    std::size_t localPaneCount = 0;
    std::size_t adoptedLocalPaneCount = 0;
    std::size_t attachedLocalPaneCount = 0;
    std::size_t movedLocalPaneCount = 0;
    std::size_t unresolvedAdoptionCount = 0;
    bool isNewHostVerified = false;
    bool isNewHostInstalled = false;
    bool isNewHostAnswering = false;
    std::size_t olderHostCount = 0;
    std::size_t pendingSessionCount = 0;
};

/// Whether the app may relinquish its old binary to Sparkle.
[[nodiscard]] inline bool mayInstallUpdateAndRelaunch(const UpdateRelaunchFacts& facts)
{
    return facts.adoptedLocalPaneCount == facts.localPaneCount
        && facts.attachedLocalPaneCount == facts.localPaneCount
        && facts.movedLocalPaneCount == facts.localPaneCount && !facts.unresolvedAdoptionCount
        && facts.isNewHostVerified && facts.isNewHostInstalled && facts.isNewHostAnswering
        && !facts.olderHostCount && !facts.pendingSessionCount;
}

/// Selects the installation path allowed by the terminal ownership evidence.
enum class UpdateRelaunchAction {
    InstallNow,
    AskBeforeRestart,
    InstallOnQuit,
};

/// Names the person's answer to the update's process-loss warning.
enum class UpdateRiskChoice {
    Unasked,
    NotNow,
    RestartAnyway,
};

/// Chooses the installation path from the transfer proof and the person's answer.
///
/// A confirmed restart still cannot discard an unresolved local adoption.
///
/// - Parameters:
///   - facts: The ownership proof collected before installation.
///   - choice: The person's answer, or `Unasked` before any warning.
/// - Returns: The path Sparkle may take without losing an unresolved owner.
[[nodiscard]] inline UpdateRelaunchAction updateRelaunchAction(const UpdateRelaunchFacts& facts, UpdateRiskChoice choice)
{
    if (mayInstallUpdateAndRelaunch(facts))
        return UpdateRelaunchAction::InstallNow;
    if (choice == UpdateRiskChoice::Unasked)
        return UpdateRelaunchAction::AskBeforeRestart;
    return choice == UpdateRiskChoice::RestartAnyway && !facts.unresolvedAdoptionCount
        ? UpdateRelaunchAction::InstallNow
        : UpdateRelaunchAction::InstallOnQuit;
}

/// The warning names the terminals whose process ownership the update could
/// not prove. It does not promise that an explicit restart will keep them.
[[nodiscard]] inline std::string updateRelaunchRiskDetail(std::size_t terminalCount, const std::string& foregroundProgram)
{
    if (!terminalCount)
        return "Ubiquitty couldn’t prepare the new host. Restarting now could close open processes. Ubiquitty will update the next time you quit it if you choose Not Now.";
    const std::string count = std::to_string(terminalCount);
    const std::string program = Title::sanitizedBannerText(std::string_view(foregroundProgram).substr(0, 96), Title::LevelMarks::Remove);
    return terminalCount == 1
        ? "Ubiquitty couldn’t move 1 terminal" + (program.empty() ? std::string() : " running " + program)
            + " to the new host. Restarting now would close its process. Ubiquitty will update the next time you quit it if you choose Not Now."
        : "Ubiquitty couldn’t move " + count + " terminals to the new host."
            + (program.empty() ? std::string() : " One is running " + program + ".")
            + " Restarting now would close their processes. Ubiquitty will update the next time you quit it if you choose Not Now.";
}

[[nodiscard]] inline std::string updateRelaunchRiskDetail(std::size_t terminalCount)
{
    return updateRelaunchRiskDetail(terminalCount, {});
}

[[nodiscard]] inline std::string_view updateRelaunchRiskTitle() { return "The Update Couldn’t Keep Your Terminals"; }
[[nodiscard]] inline std::string_view updateRelaunchProceedTitle() { return "Restart Anyway"; }
[[nodiscard]] inline std::string_view updateRelaunchPostponeTitle() { return "Not Now"; }

/// Session-restore classification: a restored tab reattaches iff it carries both
/// a daemon session (sessionID != 0) and a bridge command to reach it; else a
/// fresh local shell. Launch-side decision only.
enum class RestoreAction {
    Reattach,
    FreshLocal
};

[[nodiscard]] inline RestoreAction classifyRestoreTab(bool hasLiveSession, bool hasBridgeCommand) { return (hasLiveSession && hasBridgeCommand) ? RestoreAction::Reattach : RestoreAction::FreshLocal; }

/// One tab as the session store holds it.
struct RestoredTab {
    /// The daemon session this tab held, or zero for none.
    std::uint32_t sessionID = 0;
    /// The command that reaches the daemon, as captured.
    std::string bridgeCommand;
    /// The socket the session lives on now, which the bridge is pinned to.
    std::string socketName;
    /// The host the tab was captured against; `local` for this machine.
    std::string hostLabel;
    /// The split topology to rebuild, empty for a lone pane.
    std::string paneLayout;
    /// Exact daemon sessions adopted for the update. A split may also contain
    /// sessions that were hosted before it and must remain hosted.
    std::vector<std::uint32_t> reclaimSessionIDsAfterUpdate;
};

/// What opening one restored tab requires.
struct RestoredTabPlan {
    /// Whether this tab rejoins a daemon session or opens a fresh shell.
    RestoreAction action = RestoreAction::FreshLocal;
    /// The socket-pinned command that reaches the session; empty when fresh.
    std::string bridgeCommand;
    /// The title the window carries until the host's own stream names it.
    std::string hostLabel;
    /// The session to re-home to; zero when fresh.
    std::uint32_t sessionID = 0;
    /// How to reach the host; carries nothing when fresh.
    RemoteTransportPlan transport;
    /// The split topology to rebuild.
    std::string paneLayout;
    std::vector<std::uint32_t> reclaimSessionIDsAfterUpdate;
};

/// The label a tab carries when it was captured on this machine.
inline constexpr const char* localHostLabel = "local";

/// Folds a captured tab into the launch it describes.
///
/// A tab reattaches only when it carries both a session and a way to reach it;
/// either alone is a fresh shell, because a session with no bridge cannot be
/// reached and a bridge with no session has nothing to rejoin.
///
/// The bridge is pinned to `socketName` so a daemon that an update has since
/// superseded is still found — it keeps serving its own sessions until they
/// end — and falls back to the captured command verbatim when there is no
/// socket to pin to.
///
/// - Parameters:
///   - tab: The captured tab.
///   - quotedLocalBinary: The local `ubiquitty-host` path, already shell-quoted.
/// - Returns: The plan, whose `action` says which of its fields carry.
[[nodiscard]] inline RestoredTabPlan planRestoredTab(const RestoredTab& tab, const std::string& quotedLocalBinary)
{
    RestoredTabPlan plan;
    plan.action = classifyRestoreTab(tab.sessionID != 0, !tab.bridgeCommand.empty());
    plan.hostLabel = tab.hostLabel.empty() ? localHostLabel : tab.hostLabel;
    plan.paneLayout = tab.paneLayout;
    if (plan.action != RestoreAction::Reattach)
        return plan;

    plan.sessionID = tab.sessionID;
    if (plan.hostLabel == localHostLabel)
        plan.reclaimSessionIDsAfterUpdate = tab.reclaimSessionIDsAfterUpdate;
    plan.bridgeCommand = substituteReattachSocket(tab.bridgeCommand, tab.socketName);
    plan.transport = plan.hostLabel == localHostLabel
        ? localTransportPlan(quotedLocalBinary)
        : remoteTransportPlan(plan.hostLabel);
    return plan;
}

/// When a layout is captured, which decides whether a local shell may be
/// handed to the daemon.
enum class CaptureOccasion {
    /// The application is quitting: a local shell the daemon can adopt is
    /// handed over, so it outlives the process and the restore rejoins it.
    Quit,
    /// A relaunch may proceed only after its captured local panes are adopted
    /// and the new host has taken them from this app's old host.
    UpdateRelaunch,
    /// A workspace is saved while its windows stay open: nothing is handed
    /// over, because the live windows must not change under the reader.
    SavedWorkspace,
};

/// What capturing one tab stores.
enum class TabCaptureAction {
    /// A view of panes another authority owns — a tmux pane — rather than a
    /// session of its own, so no tab is stored.
    Excluded,
    /// A rendered window whose owner supplies a typed live reference.
    KeepsHostedSession,
    /// A split tab the daemon does not hold whole: its pane layout, rebuilt
    /// with fresh local panes.
    FreshWithLayout,
    /// A thin client: the daemon session it is attached to and the way back.
    KeepsRemoteSession,
    /// Every pane's local shell handed to the daemon, which the restore
    /// rejoins in the split the tab had.
    ReHomes,
    /// A fresh local shell at restore.
    FreshLocal,
};

/// One live tab, as the capture reads it from whichever shell drew it.
struct TabCaptureFacts {
    /// Whether the tab only shows panes another authority owns.
    bool isRenderOnly = false;
    /// Whether a rendered window supplies a reference, including an unavailable one.
    bool hasHostedReference = false;
    /// Whether the tab is a thin client of a daemon session, or of as many as
    /// it has panes.
    bool isRemote = false;
    /// Whether the daemon can hold every pane of the tab: each runs a plain
    /// local shell it could adopt or, in a split, one it already holds.
    bool isShareableLocalSession = false;
    /// The split topology, empty for a lone pane.
    std::string paneLayout;
    /// A thin client's daemon session, zero otherwise.
    std::uint32_t remoteSessionID = 0;
    /// The host a thin client is attached to.
    std::string remoteHostLabel;
    /// The command that reaches a thin client's daemon.
    std::string remoteBridgeCommand;
    /// The socket a thin client's session lives on.
    std::string remoteSocketName;
};

/// What the capture as a whole knows, which no single tab does.
struct CaptureConditions {
    /// Why the layout is being captured.
    CaptureOccasion occasion = CaptureOccasion::SavedWorkspace;
    /// Whether this machine's daemon is up to adopt a shell.
    bool isLocalDaemonReady = false;
};

/// Decides what capturing one tab stores.
///
/// Local shells are handed over only at quit, only to a daemon that is up, and
/// only when the daemon can hold every pane of the tab. A split outranks
/// everything but exclusion, and is handed over whole or not at all: whole,
/// the restore rejoins each pane in the split it had; otherwise its panes
/// restore as fresh local shells in its layout, since a split handed over in
/// part would strand sessions the restore never rejoins. A thin client keeps
/// its session on either
/// occasion, because keeping it disturbs nothing live. Every other tab
/// restores as a fresh shell, keeping its window, frame, position and tags.
///
/// - Parameters:
///   - facts: The tab.
///   - conditions: The capture it belongs to.
/// - Returns: What to store for it.
[[nodiscard]] inline TabCaptureAction planCapturedTab(const TabCaptureFacts& facts, const CaptureConditions& conditions)
{
    if (facts.isRenderOnly && facts.hasHostedReference) {
        return TabCaptureAction::KeepsHostedSession;
    }
    if (facts.isRenderOnly)
        return TabCaptureAction::Excluded;
    const bool isHandedOver = conditions.occasion != CaptureOccasion::SavedWorkspace && conditions.isLocalDaemonReady && facts.isShareableLocalSession;
    if (!facts.paneLayout.empty())
        return isHandedOver ? TabCaptureAction::ReHomes : TabCaptureAction::FreshWithLayout;
    if (facts.isRemote)
        return TabCaptureAction::KeepsRemoteSession;
    if (isHandedOver)
        return TabCaptureAction::ReHomes;
    return TabCaptureAction::FreshLocal;
}

/// The conditions the rest of a capture runs under once one shell has been
/// offered to the daemon.
///
/// A daemon that took no shell is offered no other. One that accepts a
/// connection and never answers costs each offer the whole handoff deadline,
/// and the capture at quit runs on the thread the application exits from, so
/// offering every shell in turn would hold the quit for that deadline once per
/// window. The tabs after a refusal restore as fresh shells, keeping their
/// windows.
///
/// - Parameters:
///   - conditions: The capture's conditions before the offer.
///   - reHomedSessionID: The daemon's id for the shell offered, zero when it
///     did not take it.
/// - Returns: The conditions for the tabs still to be captured.
[[nodiscard]] inline CaptureConditions conditionsAfterHandOff(CaptureConditions conditions, std::uint32_t reHomedSessionID)
{
    if (reHomedSessionID == 0)
        conditions.isLocalDaemonReady = false;
    return conditions;
}

/// What a quit about to happen knows, which its confirmation asks before any
/// capture runs.
struct QuitConditions {
    /// Whether the quit saves the session for the next launch to restore.
    bool isSessionPersistent = false;
    /// Whether this machine's daemon is up to adopt a shell.
    bool isLocalDaemonReady = false;
};

/// Whether a quit leaves a tab's running processes alive, so the quit's
/// confirmation has nothing to warn about for it.
///
/// Only a persistent session keeps anything: without one the quit captures
/// nothing and every tab's processes end with the application. With one, the
/// answer is the capture's own (``planCapturedTab`` at quit): a shell handed to
/// the daemon outlives the application, and so does every pane of a split
/// handed over whole; a thin client's session lives in its daemon, and a
/// render-only view's panes belong to another authority. A split the daemon
/// cannot hold whole restores with fresh panes and a shell no daemon adopts
/// restores fresh, so the processes running in either end with the quit.
///
/// - Parameters:
///   - facts: The tab.
///   - conditions: The quit.
/// - Returns: Whether the tab's processes outlive the quit.
[[nodiscard]] inline bool isTabKeptRunningByQuit(const TabCaptureFacts& facts, const QuitConditions& conditions)
{
    if (!conditions.isSessionPersistent)
        return false;
    const CaptureConditions capture { .occasion = CaptureOccasion::Quit, .isLocalDaemonReady = conditions.isLocalDaemonReady };
    switch (planCapturedTab(facts, capture)) {
    case TabCaptureAction::Excluded:
    case TabCaptureAction::KeepsRemoteSession:
    case TabCaptureAction::KeepsHostedSession:
    case TabCaptureAction::ReHomes:
        return true;
    case TabCaptureAction::FreshWithLayout:
    case TabCaptureAction::FreshLocal:
        return false;
    }
    return false;
}

/// The daemon a capture at quit hands local shells to.
struct LocalDaemonTarget {
    /// The command that reaches this machine's daemon.
    std::string bridgeCommand;
    /// The versioned socket the shells were handed to, which the restore pins.
    std::string socketName;
    /// The daemon's control socket, which a shell is handed through; empty
    /// when no daemon is up to take one.
    std::string socketPath;
};

/// Folds one tab's capture into what the session store keeps: the inverse of
/// ``planRestoredTab``.
///
/// A re-home that failed stores its bridge with no session, which
/// ``planRestoredTab`` reads as a fresh shell — the window still comes back.
///
/// - Parameters:
///   - facts: The tab.
///   - action: What ``planCapturedTab`` decided for it.
///   - daemon: Where a re-homed shell went.
///   - reHomedSessionID: The daemon's id for the shell, zero when the tab was
///     not re-homed or the daemon refused it.
/// - Returns: The stored tab; empty for an excluded one, which is not stored.
[[nodiscard]] inline RestoredTab storedTab(const TabCaptureFacts& facts, TabCaptureAction action, const LocalDaemonTarget& daemon, std::uint32_t reHomedSessionID)
{
    RestoredTab tab;
    switch (action) {
    case TabCaptureAction::Excluded:
    case TabCaptureAction::KeepsHostedSession:
        return tab;
    case TabCaptureAction::FreshWithLayout:
        tab.hostLabel = localHostLabel;
        tab.paneLayout = facts.paneLayout;
        return tab;
    case TabCaptureAction::KeepsRemoteSession:
        tab.sessionID = facts.remoteSessionID;
        tab.hostLabel = facts.remoteHostLabel;
        tab.bridgeCommand = facts.remoteBridgeCommand;
        tab.socketName = facts.remoteSocketName;
        return tab;
    case TabCaptureAction::ReHomes:
        tab.sessionID = reHomedSessionID;
        tab.hostLabel = localHostLabel;
        tab.bridgeCommand = daemon.bridgeCommand;
        tab.socketName = daemon.socketName;
        tab.paneLayout = facts.paneLayout;
        return tab;
    case TabCaptureAction::FreshLocal:
        tab.hostLabel = localHostLabel;
        return tab;
    }
    return tab;
}

} // namespace Ubiquitty::Session
