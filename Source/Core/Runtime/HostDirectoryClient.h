// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include "Core/App/SessionOutline.h"
#include "Core/Runtime/AttachQuestion.h"
#include "Core/Runtime/HostDirectory.h"
#include "Core/Runtime/HostOutline.h"
#include "Core/Runtime/PostedDispatcher.h"
#include "Core/Runtime/SessionListWords.h"
#include "Core/Runtime/StructureCalls.h"
#include "Core/Session/SessionDirectoryCore.h"
#include "Core/Session/SessionTree.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Ubiquitty::Runtime {

/// Owns control-only host discovery without allocating a terminal or surface.
/// Public methods and callbacks run on the dispatcher's event loop.
///
/// A listing answers with the host's tree beside its flat rows: every
/// workspace, window, tab and pane with what each shows. ``outline`` turns the
/// newest tree into the rows a frontend draws. ``watch`` keeps a second
/// connection subscribed, so the tree follows the host while a person is
/// looking at it; it is separate from the errands so that a watch never
/// makes a terminate or a management request wait.
///
/// A watch holds its connection for as long as it is asked to. When the
/// carrier drops it dials again on the reconnect schedule and the snapshot
/// says so: ``Snapshot::treeFreshness`` turns ``TreeFreshness::Stale``, so
/// no frontend draws a tree nothing is following as one that is live, and
/// ``Snapshot::watchError`` holds why until the watch is back.
class UBIQUITTY_NONCOPYABLE HostDirectoryClient final {
public:
    using ChangeCallback = std::function<void()>;
    /// Identifies the most recently completed directory operation.
    enum class Event : std::uint8_t {
        None,
        SessionList,
        ManagementReply,
        Failed,
        TerminateSettled,
        /// A watch reported the host's tree again.
        SessionTree,
        /// A watch ended on its own: the host refused it.
        WatchEnded,
        /// A watch lost its host and is dialling again.
        WatchInterrupted,
        /// A change asked of the host's structure ended: served, refused or
        /// unanswered. ``Snapshot::structureResult`` says which and why.
        StructureSettled
    };

    /// How current ``Snapshot::tree`` is.
    ///
    /// The values are explicit because the Swift mirrors pin them.
    enum class TreeFreshness : std::uint8_t {
        /// There is no tree.
        None = 0,
        /// The tree is what a listing answered, as of
        /// ``Snapshot::treeHeldAtUnixSeconds``; nothing follows it.
        Listed = 1,
        /// A watch that is connected keeps the tree current.
        Live = 2,
        /// A watch kept the tree current and has lost its host: the tree is
        /// what the host held when it was last heard, and may no longer be
        /// so.
        Stale = 3,
    };

    /// Where the host's listing stands.
    ///
    /// The values are explicit because the Swift mirrors pin them.
    enum class ListingPhase : std::uint8_t {
        /// Nothing has asked the host yet.
        NotAsked = 0,
        /// An ask is out or queued; ``Snapshot::tree`` is the previous
        /// answer, if any.
        Asking = 1,
        /// The host answered; ``Snapshot::tree`` is its answer, possibly
        /// holding nothing.
        Listed = 2,
        /// The last ask failed for ``Snapshot::failure``;
        /// ``Snapshot::tree`` is the last answer that succeeded.
        Failed = 3,
    };

    /// Captures directory results independently of subsequent operations.
    struct Snapshot {
        Protocol::Sync::ManagementReply management;
        /// Pins the daemon incarnation that supplied this successful listing.
        std::string daemonSocketName;
        std::uint64_t daemonInstanceID = 0;
        /// The sentence for the last failure, for a status line; empty after a
        /// success.
        std::string error;
        std::uint64_t revision { 0 };
        Event event { Event::None };
        std::uint32_t lastTerminateSessionID { 0 };
        Session::TerminateOutcome terminateOutcome { Session::TerminateOutcome::Failed };
        /// Where the listing stands.
        ListingPhase phase { ListingPhase::NotAsked };
        /// Why the last listing failed, ``Session::ListingFailure::None`` when
        /// it did not.
        Session::ListingFailure failure { Session::ListingFailure::None };
        /// The host's reason, read only when ``failure`` is
        /// ``Session::ListingFailure::Refused``.
        Protocol::Sync::ByeReason deniedReason { Protocol::Sync::ByeReason::TokenMissing };
        /// The platform the host reported with a failure: a host with nothing
        /// installed, or one that refused; empty otherwise.
        std::string remotePlatform;
        /// Which end of a version refusal is older, read only when ``failure``
        /// is ``Session::ListingFailure::Refused``.
        Protocol::Sync::OlderSide olderSide { Protocol::Sync::OlderSide::Unknown };
        /// The carrier's or the host's own words on the last failure, standing
        /// alone as a sentence for a person to read under the failure's own.
        std::string failureDetail;
        /// When the host last answered a listing, in seconds since the Unix
        /// epoch; 0 before any answer. Wall-clock, because a person reads it.
        std::int64_t answeredAtUnixSeconds { 0 };
        /// What the host last said about its build: its range and app
        /// version, from a listing or a refusal.
        Protocol::Sync::PeerBuild hostBuild;
        /// Everything the host holds, as the newest listing or watch report
        /// assembled it; null before the first and after a listing the host
        /// answered without one while no watch was running. Kept across a
        /// failed errand as the rows are.
        std::shared_ptr<const Session::SessionTree> tree;
        /// Moves whenever ``tree`` is replaced.
        std::uint64_t treeRevision { 0 };
        /// How far the host's wall clock was ahead of this machine's when
        /// ``tree`` was assembled, in milliseconds; negative when behind.
        std::int64_t hostClockLeadMilliseconds { 0 };
        /// How current ``tree`` is: followed, as of a listing, or left behind
        /// by a watch that lost its host.
        TreeFreshness treeFreshness { TreeFreshness::None };
        /// When ``tree`` was last replaced, in seconds since the Unix epoch
        /// by this machine's clock; 0 with no tree.
        std::int64_t treeHeldAtUnixSeconds { 0 };
        /// Whether a watch was asked for and has not ended: following the
        /// host, or dialling it again after a drop.
        bool isWatching { false }; // bool-ok: names the one state it reports
        /// The sentence for why the watch is not following: why its carrier
        /// dropped, while it dials again, or why it ended on its own. Empty
        /// while it follows the host and after one was stopped.
        std::string watchError;
        /// How the change that settled last ended, with the number its
        /// request was given; `requestID` zero before any has.
        StructureResult structureResult;
        /// The changes that settled most recently, oldest first, at most
        /// ``maxKeptStructureResults``: a frontend that asked for several
        /// finds each by its number.
        std::vector<StructureResult> structureResults;
        /// What the last ``decideAttach`` asks the person, worded, with the
        /// workspaces it is about under the outline's own titles; asks
        /// nothing when that decision picked a connect itself.
        AttachQuestion attachQuestion;
    };

    /// Creates a client over a trusted host transport command.
    ///
    /// - Parameters:
    ///   - shellCommand: The shell-escaped command connecting to the host.
    ///   - loop: The owning event loop every callback and settlement runs on.
    HostDirectoryClient(std::string shellCommand, PostedWorkLoop loop);
    /// Uses a carrier provider and dispatcher that outlive the client.
    HostDirectoryClient(Session::Dispatcher&, Session::ChannelFactory&);
    /// Stops the carrier and revokes pending observer work.
    ~HostDirectoryClient();
    /// Transfers ownership while retaining stable callback targets.
    HostDirectoryClient(HostDirectoryClient&&) noexcept;
    /// Replaces the owned client, cancelling its pending operation.
    HostDirectoryClient& operator=(HostDirectoryClient&&) noexcept;
    HostDirectoryClient(const HostDirectoryClient&) = delete;
    HostDirectoryClient& operator=(const HostDirectoryClient&) = delete;

    /// Owns the change callback and invokes it on the client's dispatcher.
    ///
    /// Captures remain alive until callback replacement or client destruction.
    void setChangeCallback(ChangeCallback);
    /// Sets the authentication token used by subsequent operations.
    void setAuthToken(std::string);
    /// Replaces the wall clock ages are anchored to this machine by.
    ///
    /// - Parameter clock: Returns milliseconds since the Unix epoch.
    void setWallClock(std::function<std::uint64_t()> clock);
    /// Follows the host: its tree is kept current in ``Snapshot::tree`` and
    /// each change arrives as ``Event::SessionTree``, until ``stopWatching``
    /// or until the host refuses the watch, which arrives as
    /// ``Event::WatchEnded``. A carrier that drops arrives as
    /// ``Event::WatchInterrupted`` and is dialled again.
    ///
    /// - Returns: Whether a watch is running: this one or one already out.
    [[nodiscard]] bool watch();
    /// Stops following the host. Does nothing when no watch is running.
    void stopWatching();

    /// Sets the discovered daemon whose listing this endpoint must answer.
    ///
    /// Called before either a listing or a watch starts.
    /// - Parameter instance: The picked daemon; empty for the host's default.
    void setExpectedDaemon(Session::HostInstanceInfo instance);
    /// The rows a frontend draws for the newest tree.
    ///
    /// The title compositions and the tag rules are the live preferences',
    /// and the status colours the request's theme's. Ages are measured
    /// against the host's clock, kept running by this machine's since the
    /// tree was assembled.
    ///
    /// Defined here so that a build which lists hosts and draws no outline,
    /// the agent's, links neither the title settings nor the themes.
    ///
    /// - Parameter request: The reader's own half: who is looking, which
    ///   rows are closed and what is searched for.
    /// - Returns: The rows; none before any tree.
    [[nodiscard]] App::SessionOutline outline(const HostOutlineRequest& request) const
    {
        const std::shared_ptr<const Session::SessionTree> held = tree();
        if (!held)
            return {};
        return buildHostOutline(*held, Preferences::values(), readerStatusColors(Preferences::values(), request.themeIndex), request, hostNowUnixMilliseconds());
    }

    /// The process and lifetime details under one outline level.
    ///
    /// - Parameters:
    ///   - kind: The selected row's identity's kind.
    ///   - number: Its number.
    /// - Returns: Core-worded details, none without a tree or a live target.
    [[nodiscard]] std::vector<HostSessionDetails> sessionDetails(std::uint8_t kind, std::uint64_t number) const
    {
        const auto target = HostOutlineRequest::rowIdentity(kind, number);
        const std::shared_ptr<const Session::SessionTree> held = tree();
        return held && target ? hostSessionDetails(*held, *target) : std::vector<HostSessionDetails> {};
    }

    /// Decides the connect to this host, on the tree this client holds.
    ///
    /// The tree counts as the host's answer only while it is held fresh:
    /// as a listing answered it, or followed by a watch that is connected.
    /// One a watch left behind, or none, is a host that has not answered, so
    /// nothing is read as empty or as ended from it. When the rule asks the
    /// person, the question is worded here and kept in
    /// ``Snapshot::attachQuestion``; any other answer clears it.
    ///
    /// Defined here for the reason ``outline`` is.
    ///
    /// - Parameters:
    ///   - hostKey: The embedder's name for the host, whose remembered
    ///     pairing is recalled.
    ///   - predefinedWorkspaceName: The name the saved host carries, empty
    ///     when it names none.
    ///   - chosenSessionID: The listed session the person selected, or 0.
    ///   - request: The reader's half of the outline whose titles name the
    ///     workspaces a question is about.
    /// - Returns: The connect to make.
    [[nodiscard]] Session::AttachDecision decideAttach(const std::string& hostKey, const std::string& predefinedWorkspaceName, std::uint32_t chosenSessionID, const HostOutlineRequest& request)
    {
        const Snapshot held = snapshot();
        const bool isAnswer = held.tree && (held.treeFreshness == TreeFreshness::Listed || held.treeFreshness == TreeFreshness::Live); // bool-ok: names the one state it reports
        Session::AttachDecision decision = HostDirectory::decide(hostKey, predefinedWorkspaceName, isAnswer ? held.tree.get() : nullptr, chosenSessionID);
        // A question's candidates are named whatever the reader has closed
        // or is searching for.
        HostOutlineRequest whole = request;
        whole.collapsed.clear();
        whole.collapsedRecordedRows.clear();
        whole.query.clear();
        noteAttachQuestion(decision.plan == Session::AttachPlan::ChooseFromList ? attachQuestionOf(decision, outline(whole)) : AttachQuestion {});
        return decision;
    }

    /// What this client reports about its host, as the facts a host's
    /// section is worded from.
    ///
    /// - Parameters:
    ///   - kind: Whether the host is this machine or another.
    ///   - transportRefusal: Why the host could not be dialled at all, as
    ///     the frontend's own door found before any errand.
    /// - Returns: The facts.
    [[nodiscard]] HostSectionFacts sectionFacts(HostKind kind, HostTransportRefusal transportRefusal) const
    {
        static_assert(std::to_underlying(ListingPhase::NotAsked) == std::to_underlying(HostListingProgress::NotAsked) && std::to_underlying(ListingPhase::Asking) == std::to_underlying(HostListingProgress::Asking)
            && std::to_underlying(ListingPhase::Listed) == std::to_underlying(HostListingProgress::Listed) && std::to_underlying(ListingPhase::Failed) == std::to_underlying(HostListingProgress::Failed));
        const Snapshot held = snapshot();
        HostSectionFacts facts;
        facts.isThisMachine = kind == HostKind::ThisMachine;
        facts.transportRefusal = transportRefusal;
        facts.progress = static_cast<HostListingProgress>(std::to_underlying(held.phase));
        facts.failure = held.failure;
        facts.deniedReason = held.deniedReason;
        facts.olderSide = held.olderSide;
        facts.remotePlatform = held.remotePlatform;
        facts.workspaceCount = held.tree ? static_cast<std::uint32_t>(held.tree->workspaces.size()) : 0;
        facts.isStale = held.treeFreshness == TreeFreshness::Stale;
        facts.hasAnswered = held.answeredAtUnixSeconds != 0;
        const std::int64_t now = static_cast<std::int64_t>(wallNowUnixSeconds());
        facts.secondsSinceAnswered = facts.hasAnswered && now > held.answeredAtUnixSeconds ? static_cast<std::uint64_t>(now - held.answeredAtUnixSeconds) : 0;
        return facts;
    }

    /// The state of this host's section and every wording of it
    /// (`hostSectionWords`).
    ///
    /// - Parameters:
    ///   - hostName: What the host is called.
    ///   - kind: Whether the host is this machine or another.
    ///   - transportRefusal: Why the host could not be dialled at all.
    /// - Returns: The state and its words.
    [[nodiscard]] HostSectionWords sectionWords(const std::string& hostName, HostKind kind, HostTransportRefusal transportRefusal) const
    {
        return hostSectionWords(sectionFacts(kind, transportRefusal), hostName);
    }

    /// This machine's wall clock as this client reads it, in seconds since
    /// the Unix epoch: the one ``Snapshot::answeredAtUnixSeconds`` is
    /// stamped by.
    [[nodiscard]] std::uint64_t wallNowUnixSeconds() const;

    /// What a Connect window holds for this host, as the facts its default
    /// button and its row markers are decided from.
    ///
    /// - Parameters:
    ///   - hostName: What the host is called.
    ///   - decision: What ``decideAttach`` answered.
    ///   - request: The reader's request, whose outline names the rows.
    ///   - selectedKind: The selected row's identity's kind.
    ///   - selectedNumber: Its number; zero with no row selected.
    ///   - recordedWorkspaceID: The workspace of a selected recorded row,
    ///     zero when the selection has an identity instead.
    ///   - recordedLevel: The recorded row's listed level.
    ///   - recordedWindow: Its window index in the record.
    ///   - recordedTab: Its tab index, zero for a window's row.
    /// - Returns: The facts, with `isConnectPending` false: the window sets
    ///   it.
    [[nodiscard]] ConnectActionFacts connectActionFacts(const std::string& hostName, const Session::AttachDecision& decision, const HostOutlineRequest& request, std::uint8_t selectedKind, std::uint64_t selectedNumber,
        std::uint32_t recordedWorkspaceID = 0, std::uint8_t recordedLevel = 0, std::uint32_t recordedWindow = 0, std::uint32_t recordedTab = 0) const
    {
        ConnectActionFacts facts;
        facts.hostName = hostName;
        facts.isDecided = true;
        facts.plan = decision.plan;
        facts.decidedSessionID = decision.sessionID;
        facts.decidedWorkspaceID = decision.workspaceID;
        facts.decidedWorkspaceName = decision.workspaceName;
        facts.questionText = snapshot().attachQuestion.text;
        // A row is found whatever the reader has closed or searches for.
        HostOutlineRequest whole = request;
        whole.collapsed.clear();
        whole.collapsedRecordedRows.clear();
        whole.query.clear();
        whole.density = App::OutlineDensity::Regular;
        const std::optional<Session::Identity::ObjectID> selected = HostOutlineRequest::rowIdentity(selectedKind, selectedNumber);
        for (const App::OutlineRow& row : outline(whole).rows) {
            if (row.level == App::ListedLevel::Workspace && decision.workspaceID != 0 && row.id.number == decision.workspaceID) {
                facts.landingRowTitle = row.title;
                facts.isLandingShownHere = row.open.opening == Session::AttachOpening::ByRevealing;
            }
            const bool isRecordedSelection = recordedWorkspaceID != 0 && row.recordedKey
                && row.recordedKey->workspaceID == recordedWorkspaceID && std::to_underlying(row.recordedKey->level) == recordedLevel
                && row.recordedKey->window == recordedWindow && row.recordedKey->tab == recordedTab; // bool-ok: names the one match it reports
            if ((selected && row.id == *selected) || isRecordedSelection) {
                facts.isRowSelected = true;
                facts.selectedOpen = row.open;
                facts.selectedAttachedPeople = row.attendance.people;
            }
        }
        return facts;
    }

    /// The most settled changes ``Snapshot::structureResults`` keeps.
    static constexpr std::size_t maxKeptStructureResults = 8;

    /// Sets or clears the label of a workspace, a window, a tab, a pane or a
    /// session of the host.
    ///
    /// Every change below is asked the same way. It is checked against the
    /// tree this client holds: a tree that is not held fresh, a target the
    /// tree does not hold, a change to the same target still waiting for the
    /// host, or more waiting than a reader may have out, refuses it here.
    /// Otherwise it is one call to the host, queued in order behind an
    /// errand that is out. Either way it ends as ``Event::StructureSettled``
    /// with ``Snapshot::structureResult`` holding the number returned here:
    /// served, or the host's refusal in its own words, or this client's. The
    /// event never arrives before the method has returned. A served change
    /// reaches the tree by the watch that follows the host, or by a listing
    /// this client asks for when none does.
    ///
    /// - Parameters:
    ///   - target: The row's identity, or the identity of what the row
    ///     folded into itself when that is what is meant.
    ///   - label: The label; empty clears it.
    /// - Returns: The request's number.
    std::uint64_t setLabel(Session::Identity::ObjectID target, const std::string& label);

    /// Replaces the tags of a workspace, a window, a tab, a pane or a
    /// session; none clears them. Asked and answered as ``setLabel``.
    ///
    /// - Parameters:
    ///   - target: What is tagged.
    ///   - tags: Its tags, in the order they are drawn.
    /// - Returns: The request's number.
    std::uint64_t setTags(Session::Identity::ObjectID target, const StructureTags& tags);

    /// Gives a workspace its name, or takes it away with an empty one. Asked
    /// and answered as ``setLabel``.
    ///
    /// - Parameters:
    ///   - workspace: The workspace.
    ///   - name: The name.
    /// - Returns: The request's number.
    std::uint64_t renameWorkspace(Session::Identity::ObjectID workspace, const std::string& name);

    /// Gives a tab its name, or takes it away with an empty one. Asked and
    /// answered as ``setLabel``.
    ///
    /// - Parameters:
    ///   - tab: The tab.
    ///   - name: The name.
    /// - Returns: The request's number.
    std::uint64_t renameTab(Session::Identity::ObjectID tab, const std::string& name);

    /// Puts a tab at a place among the tabs of a window of its workspace.
    /// Asked and answered as ``setLabel``.
    ///
    /// - Parameters:
    ///   - tab: The tab.
    ///   - window: The window it is to stand in.
    ///   - index: The place it takes there, from zero, counted once it has
    ///     left where it stood.
    /// - Returns: The request's number.
    std::uint64_t moveTab(Session::Identity::ObjectID tab, Session::Identity::ObjectID window, std::uint32_t index);

    /// Words the question a request that ends sessions is asked with, from
    /// the tree this client holds, and gives it the token the request is
    /// sent with.
    ///
    /// A frontend shows the title, the body and the two buttons as they are
    /// and makes the safe button the default. Asking again about the same
    /// thing gives a new token; the older one stays good until it is spent
    /// or eight newer questions have been asked.
    ///
    /// - Parameters:
    ///   - request: What would be asked of the host.
    ///   - target: A window to close; a session, or the pane it draws into,
    ///     or a workspace, to terminate.
    ///   - hostName: What the host is called in the question.
    /// - Returns: The question; its token is zero and `whyNot` says why when
    ///   there is nothing to ask.
    [[nodiscard]] DestructiveConfirmation confirmationFor(DestructiveRequest request, Session::Identity::ObjectID target, const std::string& hostName);

    /// Closes a window and ends the sessions in its panes. Asked and
    /// answered as ``setLabel``, and refused here unless the token is the
    /// one ``confirmationFor`` returned for closing this window and the
    /// window still holds the sessions the question named.
    ///
    /// - Parameters:
    ///   - window: The window.
    ///   - confirmationToken: The token of the question that was answered.
    /// - Returns: The request's number.
    std::uint64_t closeWindow(Session::Identity::ObjectID window, std::uint64_t confirmationToken);

    /// ``closeWindow`` with no token, which is always refused: kept while a
    /// caller moves to the form that takes one.
    ///
    /// - Parameter window: The window.
    /// - Returns: The request's number.
    std::uint64_t closeWindow(Session::Identity::ObjectID window);

    /// Terminates a session, or every session of a workspace, through the
    /// host's authorization and consent gates.
    ///
    /// Sent only with the token ``confirmationFor`` returned for terminating
    /// this target, and only while it would end the sessions the question
    /// named. Each session is one terminate, queued in order behind an
    /// errand that is out, and each ends as its own
    /// ``Event::TerminateSettled``.
    ///
    /// - Parameters:
    ///   - request: `TerminateSession` or `TerminateWorkspace`.
    ///   - target: The session or its pane, or the workspace.
    ///   - confirmationToken: The token of the question that was answered.
    /// - Returns: How many sessions the host was asked to end, or why none
    ///   was.
    [[nodiscard]] TerminateAdmission terminate(DestructiveRequest request, Session::Identity::ObjectID target, std::uint64_t confirmationToken);

    /// The newest tree, as ``Snapshot::tree`` holds it.
    [[nodiscard]] std::shared_ptr<const Session::SessionTree> tree() const;
    /// The host's wall clock now: what it stated when the newest tree was
    /// assembled, kept running by this machine's since and never behind what
    /// the tree states. Zero before any tree.
    [[nodiscard]] std::uint64_t hostNowUnixMilliseconds() const;
    /// ``outline`` for a caller across a language seam: the same rows, read
    /// one field of one row a call.
    ///
    /// - Parameter request: As ``outline``.
    /// - Returns: The rows.
    [[nodiscard]] HostOutlineRows outlineRows(const HostOutlineRequest& request) const { return HostOutlineRows(outline(request)); }
    /// A request with a row and every row under it opened, on the tree this
    /// client holds: what "expand all under this row" leaves a reader with.
    ///
    /// - Parameters:
    ///   - request: The reader's request.
    ///   - kind: The row's identity's kind.
    ///   - number: The identity's number.
    /// - Returns: The request, changed when the tree holds the row.
    [[nodiscard]] HostOutlineRequest requestOpeningAllUnder(HostOutlineRequest request, std::uint8_t kind, std::uint64_t number) const
    {
        if (const std::shared_ptr<const Session::SessionTree> held = tree())
            (void)request.openAllUnder(*held, kind, number);
        return request;
    }
    /// ``requestOpeningAllUnder`` for a row that has no identity.
    ///
    /// - Parameters:
    ///   - request: The reader's request.
    ///   - workspaceID: The restorable workspace.
    ///   - level: 1 for a window's row, 2 for a tab's.
    ///   - window: The window's index in the record.
    ///   - tab: The tab's index in that window; zero on a window's row.
    /// - Returns: The request, changed when the tree holds the row.
    [[nodiscard]] HostOutlineRequest requestOpeningAllUnderRecorded(HostOutlineRequest request, std::uint32_t workspaceID, std::uint8_t level, std::size_t window, std::size_t tab) const
    {
        if (const std::shared_ptr<const Session::SessionTree> held = tree())
            (void)request.openAllUnderRecorded(*held, workspaceID, level, window, tab);
        return request;
    }
    /// Requests a fresh host listing without attaching to a session.
    ///
    /// Coalesces: while another errand is out, the listing is queued once
    /// behind it, and every further ask before it starts joins that one. Two
    /// windows asking the same host are answered by one listing rather than
    /// one of them being refused.
    ///
    /// - Returns: True; the listing has started or is queued.
    [[nodiscard]] bool refresh();
    /// Requests a correlated administration operation against a pinned host.
    ///
    /// Queued in order behind an errand that is out.
    ///
    /// - Returns: Whether the request started or was queued; false for a
    ///   mutation that names no daemon.
    [[nodiscard]] bool manage(Protocol::Sync::ManagementRequest);
    /// Requests termination through the host's authorization and consent gates.
    ///
    /// Sends without a question having been asked: kept while a caller moves
    /// to the form that takes a confirmation's token, and removed after.
    ///
    /// Queued in order behind an errand that is out.
    ///
    /// - Parameter sessionID: The host session to terminate.
    /// - Returns: True; the request has started or is queued.
    [[nodiscard]] bool terminate(std::uint32_t sessionID);
    /// Reports whether an operation is out or queued.
    [[nodiscard]] bool isBusy() const;
    /// Returns the last successful listing, retained across failed errands.
    [[nodiscard]] Snapshot snapshot() const;

    /// Live sibling daemon endpoints reported by this host's current listing.
    [[nodiscard]] Session::HostInstanceList hostInstances() const
    {
        const auto state = snapshot();
        return { state.tree ? state.tree->hostInstances : std::vector<Session::HostInstanceInfo> {} };
    }

#if defined(UBIQUITTY_ENABLE_TEST_HOOKS) || UBIQUITTY_UI_TESTS
    /// Drives a listing's answer without dialling a host.
    ///
    /// The errand's observer is called as a listing calls it, its tree
    /// before its rows, so a case here exercises what runs. Behind the hook
    /// because a frontend holding a tree no host stated would offer
    /// workspaces to attach to that no host holds.
    ///
    /// - Parameter tree: The tree the listing answers with.
    void noteListedSessionTreeForTest(const Session::SessionTree& tree);
    /// Drives a connected watch's report without dialling a host.
    ///
    /// - Parameter tree: The tree the watch reports.
    void noteWatchedSessionTreeForTest(const Session::SessionTree& tree);
    /// Drives a watch losing its host without a carrier to drop.
    ///
    /// - Parameter report: Why the carrier dropped.
    void noteWatchInterruptedForTest(const Session::DirectoryFailureReport& report);
#endif

private:
    /// Asks the host for one change, or refuses it here.
    ///
    /// - Parameter request: The change.
    /// - Returns: The request's number.
    std::uint64_t requestStructureChange(StructureRequest request);

    /// Keeps what the last ``decideAttach`` asks in the snapshot.
    ///
    /// - Parameter question: The question, or one that asks nothing.
    void noteAttachQuestion(AttachQuestion question);

    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace Ubiquitty::Runtime
