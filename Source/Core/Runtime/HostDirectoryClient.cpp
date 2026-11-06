// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/HostDirectoryClient.h"

#include "Core/Transport/SubprocessChannel.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <utility>

namespace Ubiquitty::Runtime {

namespace {

class DirectoryChannelFactory final : public Session::ChannelFactory {
public:
    explicit DirectoryChannelFactory(std::string command)
        : m_command(std::move(command))
    {
    }

    void createChannel(Session::TransportMode, Done done) override
    {
        done(std::make_unique<Transport::SubprocessChannel>(m_command,
                 Transport::Origin { Transport::OriginKind::Daemon, "" }),
            false);
    }

private:
    std::string m_command;
};

} // namespace

struct HostDirectoryClient::State final : Session::SessionDirectoryObserver {
    /// The errand one queued or running request is.
    enum class Operation : std::uint8_t {
        None,
        List,
        Manage,
        Terminate,
        /// One change asked of the host's structure, as a call.
        Structure
    };

    /// One request waiting for the errand ahead of it.
    struct QueuedOperation {
        Operation operation { Operation::None };
        std::uint32_t sessionID { 0 };
        Protocol::Sync::ManagementRequest request;
        /// The call a structure change makes, and its request's number.
        std::string call;
        std::uint64_t structureRequestID { 0 };
    };

    /// What the watch's connection reports, kept apart from the errands'
    /// so that a watch ending is never read as an errand failing.
    struct WatchObserver final : Session::SessionDirectoryObserver {
        State* state { nullptr };

        void onDirectoryFailed(const Session::DirectoryFailureReport& report) override
        {
            state->snapshot.isWatching = false;
            state->leaveTreeBehind();
            state->snapshot.watchError = Session::listingFailureNotice(report, Session::NoticePlacement::Standalone);
            state->publish(Event::WatchEnded);
        }
        void onWatchInterrupted(const Session::DirectoryFailureReport& report) override
        {
            state->leaveTreeBehind();
            state->snapshot.watchError = Session::listingFailureNotice(report, Session::NoticePlacement::Standalone);
            state->publish(Event::WatchInterrupted);
        }
        void onSessionTree(const Session::SessionTree& tree) override
        {
            state->holdTree(tree, TreeFreshness::Live);
            state->snapshot.watchError.clear();
            state->publish(Event::SessionTree);
        }
    };

    State(std::string command, PostedWorkLoop loop)
        : ownedDispatcher(std::make_unique<PostedDispatcher>(loop))
        , ownedFactory(std::make_unique<DirectoryChannelFactory>(std::move(command)))
        , dispatcher(ownedDispatcher.get())
        , directory(ownedDispatcher.get(), ownedFactory.get(), this)
        , watcher(ownedDispatcher.get(), ownedFactory.get(), &watchObserver)
    {
        watchObserver.state = this;
    }

    State(Session::Dispatcher& dispatcher, Session::ChannelFactory& factory)
        : dispatcher(&dispatcher)
        , directory(&dispatcher, &factory, this)
        , watcher(&dispatcher, &factory, &watchObserver)
    {
        watchObserver.state = this;
    }

    /// Keeps how a structure change ended and says so.
    ///
    /// - Parameter result: How it ended.
    void settleStructure(StructureResult result)
    {
        snapshot.structureResults.push_back(result);
        if (snapshot.structureResults.size() > maxKeptStructureResults)
            snapshot.structureResults.erase(snapshot.structureResults.begin());
        snapshot.structureResult = std::move(result);
        publish(Event::StructureSettled);
    }

    /// Settles a structure change once the caller that asked has its
    /// number: never inside the call that asked.
    ///
    /// The result is boxed: its sentence makes it larger than a posted unit
    /// holds.
    ///
    /// - Parameter result: How it ended.
    void settleStructureLater(StructureResult result)
    {
        dispatcher->post([alive = std::weak_ptr<State*>(self), result = std::make_unique<StructureResult>(std::move(result))] {
            if (const std::shared_ptr<State*> state = alive.lock())
                (*state)->settleStructure(std::move(*result));
        });
    }

    void onCallAnswered(const std::string& response) override
    {
        (void)settle();
        const std::uint64_t requestID = std::exchange(structureRequestInFlight, 0);
        std::optional<StructureResult> result = structure.noteAnswer(response);
        // An answer under another id is no answer to this request.
        if (!result)
            result = structure.fail(requestID, "The host answered a different call.");
        if (result) {
            // Nothing follows the host for this client: ask what it holds
            // now, so the tree shows what was served.
            if (result->outcome == StructureOutcome::Served && !watcher.isWatchLive() && !hasQueuedListing()) {
                snapshot.phase = ListingPhase::Asking;
                QueuedOperation listing;
                listing.operation = Operation::List;
                queue.push_back(std::move(listing));
            }
            settleStructure(std::move(*result));
        }
        startNext();
    }

    /// This machine's wall clock, in milliseconds since the Unix epoch.
    [[nodiscard]] std::uint64_t wallNowUnixMilliseconds() const
    {
        if (wallClock)
            return wallClock();
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    }

    /// Keeps a tree a listing or a watch reported as the newest.
    ///
    /// - Parameters:
    ///   - tree: The tree, copied.
    ///   - freshness: Whether a connected watch follows it.
    void holdTree(const Session::SessionTree& tree, TreeFreshness freshness)
    {
        snapshot.tree = std::make_shared<const Session::SessionTree>(tree);
        snapshot.treeFreshness = freshness;
        snapshot.treeHeldAtUnixSeconds = static_cast<std::int64_t>(wallNowUnixMilliseconds() / 1000);
        ++snapshot.treeRevision;
        snapshot.hostClockLeadMilliseconds = static_cast<std::int64_t>(tree.hostNowUnixMilliseconds) - static_cast<std::int64_t>(wallNowUnixMilliseconds());
    }

    void onSessionTree(const Session::SessionTree& tree) override
    {
        // A listing's tree is as current as a watch's while a watch follows
        // the host, and is nobody's to follow otherwise.
        holdTree(tree, watcher.isWatchLive() ? TreeFreshness::Live : TreeFreshness::Listed);
        didListingReportTree = true;
    }

    /// Marks a tree a watch was following as followed no longer.
    void leaveTreeBehind()
    {
        if (snapshot.treeFreshness == TreeFreshness::Live)
            snapshot.treeFreshness = TreeFreshness::Stale;
    }

    void publish(Event event)
    {
        snapshot.event = event;
        ++snapshot.revision;
        const auto notify = callback;
        if (notify) {
            notify();
        }
    }

    /// Takes the operation that just settled, so the next can start.
    ///
    /// - Returns: The settled operation.
    Operation settle()
    {
        const Operation settled = inFlight;
        inFlight = Operation::None;
        return settled;
    }

    /// Whether a listing is waiting in the queue.
    [[nodiscard]] bool hasQueuedListing() const
    {
        return std::ranges::any_of(queue, [](const QueuedOperation& queued) { return queued.operation == Operation::List; });
    }

    /// Starts one operation now.
    ///
    /// - Parameter queued: The operation.
    /// - Returns: Whether the errand started.
    bool start(QueuedOperation queued)
    {
        inFlight = queued.operation;
        bool didStart = false;
        switch (queued.operation) {
        case Operation::None:
            break;
        case Operation::List:
            didListingReportTree = false;
            didStart = directory.refresh();
            break;
        case Operation::Manage:
            didStart = directory.manage(std::move(queued.request));
            break;
        case Operation::Terminate:
            didStart = directory.terminate(queued.sessionID);
            break;
        case Operation::Structure:
            structureRequestInFlight = queued.structureRequestID;
            didStart = directory.call(std::move(queued.call));
            if (!didStart) {
                structureRequestInFlight = 0;
                if (std::optional<StructureResult> result = structure.withdraw(queued.structureRequestID, "The request could not be sent to this host. Try again."))
                    settleStructureLater(std::move(*result));
            }
            break;
        }
        // A start that settled synchronously already cleared this; one that
        // never started has nothing in flight.
        if (!didStart)
            inFlight = Operation::None;
        return didStart;
    }

    /// Runs the operation or queues it behind the errand that is out.
    ///
    /// - Parameter queued: The operation.
    /// - Returns: Whether it started or was queued.
    bool submit(QueuedOperation queued)
    {
        if (directory.isBusy() || !queue.empty()) {
            queue.push_back(std::move(queued));
            return true;
        }
        return start(std::move(queued));
    }

    /// Starts queued operations, oldest first, until one is out.
    void startNext()
    {
        while (!directory.isBusy() && !queue.empty()) {
            QueuedOperation next = std::move(queue.front());
            queue.pop_front();
            (void)start(std::move(next));
        }
    }

    void onSessionList(const std::vector<Session::SessionRow>&) override
    {
        (void)settle();
        // A host that answered and reported no tree holds none this build
        // can draw; the previous answer's would describe a host that is gone.
        // A watch that is running holds the tree itself and keeps it.
        if (!didListingReportTree && snapshot.tree && !snapshot.isWatching) {
            snapshot.tree.reset();
            snapshot.treeFreshness = TreeFreshness::None;
            snapshot.treeHeldAtUnixSeconds = 0;
            ++snapshot.treeRevision;
        }
        snapshot.daemonSocketName = directory.daemonSocketName();
        snapshot.daemonInstanceID = directory.daemonInstanceID();
        snapshot.error.clear();
        snapshot.failureDetail.clear();
        snapshot.failure = Session::ListingFailure::None;
        snapshot.remotePlatform.clear();
        snapshot.olderSide = Protocol::Sync::OlderSide::Unknown;
        snapshot.hostBuild = directory.hostBuild();
        snapshot.answeredAtUnixSeconds = static_cast<std::int64_t>(wallNowUnixMilliseconds() / 1000);
        // A listing still queued is a newer ask the person is waiting on.
        snapshot.phase = hasQueuedListing() ? ListingPhase::Asking : ListingPhase::Listed;
        publish(Event::SessionList);
        startNext();
    }

    void onManagementReply(const Protocol::Sync::ManagementReply& reply) override
    {
        (void)settle();
        snapshot.management = reply;
        snapshot.daemonInstanceID = directory.daemonInstanceID();
        snapshot.daemonSocketName = directory.daemonSocketName();
        snapshot.error = reply.status == Protocol::Sync::ManagementStatus::Success ? "" : reply.detail;
        publish(Event::ManagementReply);
        startNext();
    }

    void onDirectoryFailed(const Session::DirectoryFailureReport& report) override
    {
        const Operation settled = settle();
        // A change whose call never came back ends as unanswered, and says
        // nothing about the listing or the last errand's error.
        if (settled == Operation::Structure) {
            if (std::optional<StructureResult> result = structure.fail(std::exchange(structureRequestInFlight, 0), Session::listingFailureNotice(report, Session::NoticePlacement::Standalone)))
                settleStructure(std::move(*result));
            startNext();
            return;
        }
        snapshot.error = Session::listingFailureNotice(report, Session::NoticePlacement::Standalone);
        snapshot.failureDetail = Session::placedNotice(report.detail, Session::NoticePlacement::Standalone);
        if (report.failure == Session::ListingFailure::Refused || report.failure == Session::ListingFailure::InstanceUnreadable) {
            snapshot.hostBuild = report.hostBuild;
        }
        // Only a failed listing says anything about the listing: a refused
        // management request leaves the host's sessions exactly as listed.
        if (settled == Operation::List) {
            snapshot.failure = report.failure;
            snapshot.deniedReason = report.deniedReason;
            snapshot.remotePlatform = report.remotePlatform;
            snapshot.olderSide = report.olderSide;
            snapshot.phase = hasQueuedListing() ? ListingPhase::Asking : ListingPhase::Failed;
        }
        publish(Event::Failed);
        startNext();
    }

    void onTerminateSettled(std::uint32_t sessionID, Session::TerminateOutcome outcome) override
    {
        (void)settle();
        snapshot.lastTerminateSessionID = sessionID;
        snapshot.terminateOutcome = outcome;
        snapshot.error.clear();
        publish(Event::TerminateSettled);
        startNext();
    }

    std::unique_ptr<PostedDispatcher> ownedDispatcher;
    std::unique_ptr<DirectoryChannelFactory> ownedFactory;
    /// The loop every callback and posted settlement runs on.
    Session::Dispatcher* dispatcher { nullptr };
    /// What a posted settlement holds weakly, so one that outlives this
    /// state does nothing.
    std::shared_ptr<State*> self { std::make_shared<State*>(this) };
    /// The structure changes this client has out.
    StructureCalls structure;
    /// The request whose call the errand in flight carries; zero otherwise.
    std::uint64_t structureRequestInFlight { 0 };
    Snapshot snapshot;
    ChangeCallback callback;
    /// The operation the directory is running, None between errands.
    Operation inFlight { Operation::None };
    /// Operations waiting for the one in flight, oldest first. Holds at most
    /// one listing: a second ask joins the queued one.
    std::deque<QueuedOperation> queue;
    std::function<std::uint64_t()> wallClock;
    /// Whether the listing in flight reported a tree before its rows.
    bool didListingReportTree { false }; // bool-ok: names the one condition it reports
    WatchObserver watchObserver;
    // Revokes posted weak work and joins the carrier before its providers die.
    Session::SessionDirectoryCore directory;
    Session::SessionDirectoryCore watcher;
};

HostDirectoryClient::HostDirectoryClient(std::string command, PostedWorkLoop loop)
    : m_state(std::make_unique<State>(std::move(command), loop))
{
}

HostDirectoryClient::HostDirectoryClient(Session::Dispatcher& dispatcher, Session::ChannelFactory& factory)
    : m_state(std::make_unique<State>(dispatcher, factory))
{
}

HostDirectoryClient::~HostDirectoryClient() = default;
HostDirectoryClient::HostDirectoryClient(HostDirectoryClient&&) noexcept = default;
HostDirectoryClient& HostDirectoryClient::operator=(HostDirectoryClient&&) noexcept = default;

void HostDirectoryClient::setChangeCallback(ChangeCallback callback)
{
    m_state->callback = std::move(callback);
}

void HostDirectoryClient::setAuthToken(std::string token)
{
    m_state->watcher.setAuthToken(token);
    m_state->directory.setAuthToken(std::move(token));
}

void HostDirectoryClient::setWallClock(std::function<std::uint64_t()> clock)
{
    // One clock for both connections and for the lead measured against the
    // trees they assemble: an errand that read another would state a host
    // clock a moment off the one its lead is taken from.
    m_state->directory.setWallClock(clock);
    m_state->watcher.setWallClock(clock);
    m_state->wallClock = std::move(clock);
}

bool HostDirectoryClient::watch()
{
    State& state = *m_state;
    if (state.watcher.isWatching())
        return true;
    // Set before the errand starts, as a listing's phase is: a carrier that
    // fails at once reports inside the start.
    state.snapshot.isWatching = true;
    state.snapshot.watchError.clear();
    // The watcher runs nothing but watches, so one starts; a carrier that
    // failed inside the start has already been reported, and is dialled
    // again.
    (void)state.watcher.watch();
    return state.snapshot.isWatching;
}

void HostDirectoryClient::stopWatching()
{
    m_state->watcher.stopWatching();
    m_state->snapshot.isWatching = false;
    m_state->snapshot.watchError.clear();
    // What it last reported stays, as of now and followed by nothing.
    if (m_state->snapshot.treeFreshness == TreeFreshness::Live)
        m_state->snapshot.treeFreshness = TreeFreshness::Listed;
}

void HostDirectoryClient::setExpectedDaemon(Session::HostInstanceInfo instance)
{
    m_state->directory.setExpectedDaemon(instance);
    m_state->watcher.setExpectedDaemon(std::move(instance));
}

std::shared_ptr<const Session::SessionTree> HostDirectoryClient::tree() const
{
    return m_state->snapshot.tree;
}

std::uint64_t HostDirectoryClient::hostNowUnixMilliseconds() const
{
    const State& state = *m_state;
    if (!state.snapshot.tree)
        return 0;
    const std::int64_t stated = static_cast<std::int64_t>(state.snapshot.tree->hostNowUnixMilliseconds);
    const std::int64_t running = static_cast<std::int64_t>(state.wallNowUnixMilliseconds()) + state.snapshot.hostClockLeadMilliseconds;
    return static_cast<std::uint64_t>(std::max(running, stated));
}

bool HostDirectoryClient::refresh()
{
    State& state = *m_state;
    // Set before the errand starts: a carrier that fails synchronously reports
    // inside the start, and its Failed must not be overwritten afterwards.
    state.snapshot.phase = ListingPhase::Asking;
    if (state.hasQueuedListing())
        return true;
    return state.submit({ State::Operation::List, 0, {}, {}, 0 });
}

bool HostDirectoryClient::terminate(std::uint32_t sessionID)
{
    return m_state->submit({ State::Operation::Terminate, sessionID, {}, {}, 0 });
}

bool HostDirectoryClient::manage(Protocol::Sync::ManagementRequest request)
{
    if (!Session::SessionDirectoryCore::isManagementRequestPinned(request))
        return false;
    return m_state->submit({ State::Operation::Manage, 0, std::move(request), {}, 0 });
}

bool HostDirectoryClient::isBusy() const
{
    return m_state->directory.isBusy() || !m_state->queue.empty();
}

/// The tree a state holds when it is one a listing answered or a connected
/// watch follows.
///
/// - Parameter held: The state's snapshot.
/// - Returns: The tree; null when none is held or it is stale.
static const Session::SessionTree* treeHeldFresh(const HostDirectoryClient::Snapshot& held)
{
    using TreeFreshness = HostDirectoryClient::TreeFreshness;
    return held.tree && (held.treeFreshness == TreeFreshness::Listed || held.treeFreshness == TreeFreshness::Live) ? held.tree.get() : nullptr;
}

std::uint64_t HostDirectoryClient::requestStructureChange(StructureRequest request)
{
    State& state = *m_state;
    StructureCalls::Admission admission = state.structure.admit(request, treeHeldFresh(state.snapshot));
    if (admission.refusal) {
        state.settleStructureLater(std::move(*admission.refusal));
        return admission.requestID;
    }
    State::QueuedOperation queued;
    queued.operation = State::Operation::Structure;
    queued.call = std::move(admission.call);
    queued.structureRequestID = admission.requestID;
    (void)state.submit(std::move(queued));
    return admission.requestID;
}

std::uint64_t HostDirectoryClient::setLabel(Session::Identity::ObjectID target, const std::string& label)
{
    StructureRequest request;
    request.operation = StructureOperation::SetLabel;
    request.target = target;
    request.text = label;
    return requestStructureChange(std::move(request));
}

std::uint64_t HostDirectoryClient::setTags(Session::Identity::ObjectID target, const StructureTags& tags)
{
    StructureRequest request;
    request.operation = StructureOperation::SetTags;
    request.target = target;
    request.tags = tags.names;
    return requestStructureChange(std::move(request));
}

std::uint64_t HostDirectoryClient::renameWorkspace(Session::Identity::ObjectID workspace, const std::string& name)
{
    StructureRequest request;
    request.operation = StructureOperation::RenameWorkspace;
    request.target = workspace;
    request.text = name;
    return requestStructureChange(std::move(request));
}

std::uint64_t HostDirectoryClient::renameTab(Session::Identity::ObjectID tab, const std::string& name)
{
    StructureRequest request;
    request.operation = StructureOperation::RenameTab;
    request.target = tab;
    request.text = name;
    return requestStructureChange(std::move(request));
}

std::uint64_t HostDirectoryClient::moveTab(Session::Identity::ObjectID tab, Session::Identity::ObjectID window, std::uint32_t index)
{
    StructureRequest request;
    request.operation = StructureOperation::MoveTab;
    request.target = tab;
    request.window = window;
    request.index = index;
    return requestStructureChange(std::move(request));
}

DestructiveConfirmation HostDirectoryClient::confirmationFor(DestructiveRequest request, Session::Identity::ObjectID target, const std::string& hostName)
{
    State& state = *m_state;
    return state.structure.confirmations().ask(treeHeldFresh(state.snapshot), request, target, hostName);
}

std::uint64_t HostDirectoryClient::closeWindow(Session::Identity::ObjectID window, std::uint64_t confirmationToken)
{
    StructureRequest request;
    request.operation = StructureOperation::CloseWindow;
    request.target = window;
    request.confirmationToken = confirmationToken;
    return requestStructureChange(std::move(request));
}

std::uint64_t HostDirectoryClient::closeWindow(Session::Identity::ObjectID window)
{
    return closeWindow(window, 0);
}

TerminateAdmission HostDirectoryClient::terminate(DestructiveRequest request, Session::Identity::ObjectID target, std::uint64_t confirmationToken)
{
    State& state = *m_state;
    TerminateAdmission admission;
    if (request == DestructiveRequest::CloseWindow) {
        admission.whyNot = "A window is closed, not terminated.";
        return admission;
    }
    DestructiveConfirmations::Redemption redemption = state.structure.confirmations().redeem(confirmationToken, request, target, treeHeldFresh(state.snapshot));
    if (!redemption.refusal.empty()) {
        admission.whyNot = std::move(redemption.refusal);
        return admission;
    }
    for (const std::uint32_t sessionID : redemption.sessionIDs) {
        if (state.submit({ State::Operation::Terminate, sessionID, {}, {}, 0 }))
            ++admission.sessionCount;
    }
    if (admission.sessionCount == 0)
        admission.whyNot = "The request could not be sent to this host. Try again.";
    return admission;
}

void HostDirectoryClient::noteAttachQuestion(AttachQuestion question)
{
    m_state->snapshot.attachQuestion = std::move(question);
}

std::uint64_t HostDirectoryClient::wallNowUnixSeconds() const
{
    return m_state->wallNowUnixMilliseconds() / 1000;
}

HostDirectoryClient::Snapshot HostDirectoryClient::snapshot() const
{
    return m_state->snapshot;
}

#if defined(UBIQUITTY_ENABLE_TEST_HOOKS) || UBIQUITTY_UI_TESTS
void HostDirectoryClient::noteListedSessionTreeForTest(const Session::SessionTree& tree)
{
    m_state->onSessionTree(tree);
    m_state->onSessionList({});
}

void HostDirectoryClient::noteWatchedSessionTreeForTest(const Session::SessionTree& tree)
{
    m_state->watchObserver.onSessionTree(tree);
}

void HostDirectoryClient::noteWatchInterruptedForTest(const Session::DirectoryFailureReport& report)
{
    m_state->watchObserver.onWatchInterrupted(report);
}
#endif

} // namespace Ubiquitty::Runtime
