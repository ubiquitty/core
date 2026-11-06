// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// SessionDirectoryCore.cpp — see SessionDirectoryCore.h.

#include "Core/Session/SessionDirectoryCore.h"

#include "Core/Protocol/Control/HostAdministration.h"
#include "Core/Protocol/Control/SessionTermination.h"
#include "Core/Protocol/Sync/AttachVerdictText.h"
#include "Core/Session/HostListing.h"
#include "Core/Session/ProofOfLife.h"
#include "Core/Session/ReconnectBackoff.h"
#include "Core/Transport/CarrierCloseReason.h"

#include <chrono>
#include <string_view>
#include <utility>

namespace Ubiquitty::Session {

namespace {

// How long a held terminate waits for the human. Past it the host has denied on
// its own timer, and a client still waiting is waiting for a frame that is not
// coming.
constexpr double terminateConsentSeconds = 35.0;

// How long a listing waits for a tree it holds part of. A host sends every
// structure and every pane's records in the round trips after it answers the
// subscription; past this the listing is delivered with what arrived, and a
// pane whose records did not is listed by its structure alone.
constexpr double treeWaitSeconds = 2.0;

// ssh's own wording when BatchMode stopped it from asking for a passphrase or
// a password.
constexpr std::string_view signInRefusal = "Permission denied";

// The reason OpenSSH's server gives when it disconnects a client that used up
// its MaxAuthTries; ssh prints it after "Received disconnect from …:".
constexpr std::string_view attemptsRefusal = "Too many authentication failures";

/// A failure report with no host verdict in it.
///
/// - Parameters:
///   - failure: The kind.
///   - detail: The carrier's or the host's words.
/// - Returns: The report.
DirectoryFailureReport failureReport(ListingFailure failure, std::string_view detail)
{
    DirectoryFailureReport report;
    report.failure = failure;
    report.detail = std::string(detail);
    return report;
}

} // namespace

ListingFailure classifyCarrierClose(std::string_view closeReason)
{
    // Attempts first: a server counting failures may have said "Permission
    // denied" to an earlier method in the same run.
    if (closeReason.find(attemptsRefusal) != std::string_view::npos)
        return ListingFailure::AuthenticationAttemptsExhausted;
    if (closeReason.find(signInRefusal) != std::string_view::npos)
        return ListingFailure::SignInRequired;
    return ListingFailure::Unreachable;
}

std::string placedNotice(std::string notice, NoticePlacement placement)
{
    if (notice.empty() || placement == NoticePlacement::Embedded)
        return notice;
    if (notice.front() >= 'a' && notice.front() <= 'z')
        notice.front() = static_cast<char>(notice.front() - 'a' + 'A');
    if (const char last = notice.back(); last != '.' && last != '?' && last != '!')
        notice.push_back('.');
    return notice;
}

std::string listingFailureNotice(const DirectoryFailureReport& report, NoticePlacement placement)
{
    // Each clause opens with a word that takes a capital when it stands
    // alone: never with a command's name, which is written one way.
    std::string notice;
    switch (report.failure) {
    case ListingFailure::None:
        return {};
    case ListingFailure::Unreachable:
        notice = "the host could not be reached"; // copy-ok: a clause; placedNotice punctuates it
        break;
    case ListingFailure::SignInRequired:
        notice = "signing in over SSH needs a password or an unlocked key"; // copy-ok: a clause; placedNotice punctuates it
        break;
    case ListingFailure::AuthenticationAttemptsExhausted:
        notice = "the server closed the connection after too many authentication attempts"; // copy-ok: a clause; placedNotice punctuates it
        break;
    case ListingFailure::EndedBeforeAnswer:
        notice = "the host closed the SSH connection before Ubiquitty’s host answered"; // copy-ok: a clause; placedNotice punctuates it
        break;
    case ListingFailure::InstanceGone:
        notice = "the Ubiquitty host this list was asked of is no longer running"; // copy-ok: a clause; placedNotice punctuates it
        break;
    case ListingFailure::InstanceUnreadable:
        return placedNotice(report.detail, placement);
    case ListingFailure::NotInstalled:
        notice = report.remotePlatform.empty() ? std::string("this host has no ubiquitty-host installed")
                                               : "this " + report.remotePlatform + " host has no ubiquitty-host installed";
        break;
    case ListingFailure::Refused:
        // The host's words when it gave any — an AUTH_DENIED's are the
        // notice for its reason, a management refusal's are its own.
        return placedNotice(report.detail.empty() ? (report.deniedReason == Protocol::Sync::ByeReason::Normal ? "the host refused the request" : std::string(Protocol::Sync::byeNotice(report.deniedReason))) : report.detail, placement);
    case ListingFailure::Malformed:
        notice = "the host sent a reply Ubiquitty doesn’t understand"; // copy-ok: a clause; placedNotice punctuates it
        break;
    }
    if (!report.detail.empty())
        notice += ": " + report.detail;
    return placedNotice(std::move(notice), placement);
}

SessionDirectoryCore::SessionDirectoryCore(Dispatcher* dispatcher, ChannelFactory* factory, SessionDirectoryObserver* observer)
    : m_dispatcher(dispatcher)
    , m_factory(factory)
    , m_observer(observer)
    // No Terminal: a directory client mirrors no grid. The screen the host
    // sends a bound session still decodes, and lands nowhere.
    , m_core(std::make_unique<Protocol::Sync::SyncClientCore>(nullptr, static_cast<Protocol::Sync::SyncClientObserver*>(this)))
{
}

SessionDirectoryCore::~SessionDirectoryCore()
{
    // Severed first: a closure the dispatcher already holds cannot be recalled,
    // and reading this object as gone is the only thing that stops it.
    revokeWeakPtrs();
    teardownChannel(); // joins the channel's I/O thread before our fields tear down
}

// ---- public API -------------------------------------------------------

bool SessionDirectoryCore::refresh() { return startErrand(Errand::List, 0); }

bool SessionDirectoryCore::watch() { return startErrand(Errand::Watch, 0); }

void SessionDirectoryCore::stopWatching()
{
    if (m_errand != Errand::Watch)
        return;
    requestFinish(Finish::WatchStopped);
    completeFinish();
}

std::uint64_t SessionDirectoryCore::wallNowUnixMilliseconds() const
{
    if (m_wallClock)
        return m_wallClock();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

void SessionDirectoryCore::armTreeDeadline()
{
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    m_dispatcher->postDelayed(treeWaitSeconds, [weakThis = weakPtr(), generation] {
        if (auto* directory = weakThis.get())
            directory->handleTreeDeadline(generation);
    });
}

void SessionDirectoryCore::handleTreeDeadline(std::uint64_t generation)
{
    // Only a listing arms this, and an errand that ended moved the stamp.
    if (generation != m_generation.load(std::memory_order_acquire))
        return;
    m_listed.isTreeDue = true;
    serviceListing();
    completeFinish();
}

void SessionDirectoryCore::serviceListing()
{
    if ((m_errand != Errand::List && m_errand != Errand::Watch) || m_finish != Finish::None)
        return;
    const bool isWatch = m_errand == Errand::Watch; // bool-ok: a local reading of the errand
    if (!m_listed.didSubscribe && m_core->isScreenAttached()) {
        m_listed.didSubscribe = true;
        // Both forms ask through the typed subscription. A one-shot errand
        // closes after projecting a complete tree; a watch keeps following.
        if (isWatch) {
            armWatchProbe();
        } else {
            const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
            m_dispatcher->postDelayed(directoryAnswerSeconds, [weakThis = weakPtr(), generation] {
                if (auto* directory = weakThis.get())
                    directory->handleListAnswerDeadline(generation);
            });
        }
        if (!m_core->call(HostListing::subscribeRequest())) {
            requestFinish(Finish::Failed, TerminateOutcome::Failed, failureReport(ListingFailure::Unreachable, "the listing request could not be sent"));
            return;
        }
    }
    for (const std::string& response : m_core->takeCallResults()) {
        if (m_listing->noteAnswer(response, wallNowUnixMilliseconds()) == HostListing::Answer::Listing)
            armTreeDeadline();
    }
    if (m_listing->isRefused()) {
        const auto failure = m_listing->failure();
        const bool isMalformed = failure == HostListing::Failure::Malformed || failure == HostListing::Failure::Unsupported;
        requestFinish(Finish::Failed, TerminateOutcome::Failed, failureReport(isMalformed ? ListingFailure::Malformed : ListingFailure::Refused, m_listing->failureDetail()));
        return;
    }
    if (const Protocol::Sync::ClientConnection* screen = m_core->screen()) {
        switch (m_listing->refresh(*screen, m_core->listingRevision(), wallNowUnixMilliseconds())) {
        case HostListing::Change::None:
            break;
        case HostListing::Change::Assembled:
            m_listed.hasUnreportedTree = true;
            break;
        case HostListing::Change::Malformed:
            requestFinish(Finish::Failed, TerminateOutcome::Failed, failureReport(ListingFailure::Malformed, "the host described a workspace this version of Ubiquitty can’t read"));
            return;
        }
    }
    const bool isTreeSettled = m_listing->isRefused() || (m_listing->isListing() && m_listing->isWhole()) || m_listed.isTreeDue; // bool-ok: a local verdict
    if (!isWatch) {
        if (m_listing->isListing() && m_listing->isWhole()) {
            requestFinish(Finish::ListDelivered);
        } else if (m_listed.isTreeDue) {
            requestFinish(Finish::Failed, TerminateOutcome::Failed, failureReport(ListingFailure::Unreachable, "the host did not finish the listing"));
        }
        return;
    }
    if (m_listed.hasUnreportedTree && isTreeSettled) {
        // Whole once, a watch reports every change from then on: a pane that
        // appears is listed by its structure until its records follow.
        m_listed.isTreeDue = true;
        m_listed.hasUnreportedTree = false;
        // Following again: the next drop starts its dials from the first.
        m_watch = { .isLive = true };
        m_observer->onSessionTree(m_listing->tree());
    }
}

bool SessionDirectoryCore::terminate(std::uint32_t sessionID) { return startErrand(Errand::Terminate, sessionID); }

bool SessionDirectoryCore::call(std::string request)
{
    if (isBusy() || request.empty() || request.size() > Protocol::Sync::maxFrameBodyBytes)
        return false;
    m_callRequest = std::move(request);
    if (!startErrand(Errand::Call, 0))
        return false;
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    m_dispatcher->postDelayed(directoryAnswerSeconds, [weakThis = weakPtr(), generation] {
        if (auto* directory = weakThis.get())
            directory->handleCallDeadline(generation);
    });
    return true;
}

void SessionDirectoryCore::handleCallDeadline(std::uint64_t generation)
{
    if (generation != m_generation.load(std::memory_order_acquire) || m_errand != Errand::Call)
        return;
    requestFinish(Finish::Failed, TerminateOutcome::Failed, failureReport(ListingFailure::Unreachable, "the host did not answer the call"));
    completeFinish();
}

bool SessionDirectoryCore::manage(Protocol::Sync::ManagementRequest request)
{
    if (isBusy() || (!request.daemonInstanceID && request.operation != Protocol::Sync::ManagementOperation::List)) {
        return false;
    }
    request.requestID = ++m_nextRequestID;
    if (!request.requestID) {
        request.requestID = ++m_nextRequestID;
    }
    const bool isTermination = request.operation == Protocol::Sync::ManagementOperation::TerminateSession;
    if ((isTermination && !request.sessionID) || (!isTermination && !Protocol::Control::isValidHostAdministrationIntent(request))) {
        return false;
    }
    m_managementRequest = std::move(request);
    m_managementReply = {};
    if (!startErrand(Errand::Management, 0)) {
        return false;
    }
    const auto generation = m_generation.load(std::memory_order_acquire);
    const auto deadline = m_managementRequest.operation == Protocol::Sync::ManagementOperation::TerminateSession ? terminateConsentSeconds : directoryAnswerSeconds;
    m_dispatcher->postDelayed(deadline, [weakThis = weakPtr(), generation] {
        if (auto* directory = weakThis.get(); directory && directory->m_generation.load(std::memory_order_acquire) == generation && directory->m_errand == Errand::Management) {
            directory->requestFinish(Finish::Failed, TerminateOutcome::Failed,
                failureReport(ListingFailure::Unreachable, "the host didn’t answer the request in time"));
            directory->completeFinish();
        }
    });
    return true;
}

// ---- errand lifecycle -------------------------------------------------

bool SessionDirectoryCore::startErrand(Errand errand, std::uint32_t sessionID)
{
    if (m_errand != Errand::None)
        return false;
    m_errand = errand;
    m_targetSessionID = sessionID;
    m_finish = Finish::None;
    m_finishFailure = {};
    m_didSeeAck = false;
    m_terminationRequestID.clear();
    m_managementCallID.clear();
    m_didSendCall = false;
    m_listed = {};
    m_listing = std::make_unique<HostListing>();
    m_callResponse = {};
    m_bootstrapResponse = {};
    m_core->resetForReconnect();
    m_watch = {};
    dialChannel();
    return true;
}

void SessionDirectoryCore::dialChannel()
{
    const std::uint64_t generation = ++m_generation; // this dial's stamp
    auto ready = [weakThis = weakPtr(), generation](std::unique_ptr<Ubiquitty::Transport::Channel> channel, bool) {
        auto* directory = weakThis.get();
        if (!directory)
            return; // the owner is gone; nothing wants this channel
        if (generation != directory->m_generation.load(std::memory_order_acquire))
            return; // a finished errand's late channel
        directory->handleChannelReady(std::move(channel));
    };
    // A terminate binds its existing session for the consent exemption; only
    // listings may use a door that forbids every session-bound HELLO.
    if (m_errand == Errand::List || m_errand == Errand::Watch)
        m_factory->createControlChannel(std::move(ready));
    else
        m_factory->createChannel(m_factory->controlTransport(), std::move(ready));
    // Every dial has to end in a host that welcomed it, or it is a dial that
    // failed: a carrier can open onto a path that carries nothing and never
    // says so, and an errand that waited for it to would wait for good.
    m_dispatcher->postDelayed(directoryHandshakeSeconds, [weakThis = weakPtr(), generation] {
        auto* directory = weakThis.get();
        if (!directory || generation != directory->m_generation.load(std::memory_order_acquire))
            return;
        const Protocol::Sync::ClientConnection* screen = directory->m_core->screen();
        if (screen && screen->welcome())
            return;
        directory->requestFailure(failureReport(ListingFailure::Unreachable, "the host did not answer"));
        directory->completeFinish();
    });
}

void SessionDirectoryCore::handleListAnswerDeadline(std::uint64_t generation)
{
    if (generation != m_generation.load(std::memory_order_acquire) || m_errand != Errand::List || m_listing->isListing())
        return;
    requestFailure(failureReport(ListingFailure::Unreachable, "the host did not answer the listing"));
    completeFinish();
}

void SessionDirectoryCore::interruptWatch(const DirectoryFailureReport& report)
{
    teardownChannel();
    // The next connection is a new one: a new handshake, a new subscription
    // and a tree assembled from what that connection is sent.
    m_core->resetForReconnect();
    m_listed = {};
    m_listing = std::make_unique<HostListing>();
    m_watch.isLive = false;
    ++m_watch.attempts;
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    m_dispatcher->postDelayed(reconnectDelayForAttempt(m_watch.attempts), [weakThis = weakPtr(), generation] {
        auto* directory = weakThis.get();
        // A watch stopped while it waited moved the stamp, and so did
        // whatever errand was started since.
        if (directory && generation == directory->m_generation.load(std::memory_order_acquire))
            directory->dialChannel();
    });
    // Told once a drop, however many dials it takes to get back.
    if (!m_watch.didReportInterruption) {
        m_watch.didReportInterruption = true;
        m_observer->onWatchInterrupted(report);
    }
}

void SessionDirectoryCore::armWatchProbe()
{
    m_didHearSinceProbeArmed = false;
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    m_dispatcher->postDelayed(watchProbeIntervalSeconds, [weakThis = weakPtr(), generation] {
        if (auto* directory = weakThis.get())
            directory->handleWatchProbe(generation);
    });
}

void SessionDirectoryCore::handleWatchProbe(std::uint64_t generation)
{
    // A connection since dropped or stopped is not the one being asked.
    if (generation != m_generation.load(std::memory_order_acquire))
        return;
    // Anything heard is contact: the quiet starts again.
    if (m_didHearSinceProbeArmed || !m_core->checkScreenLiveness()) {
        armWatchProbe();
        return;
    }
    serviceScreen();
    m_dispatcher->postDelayed(proofOfLifeDeadlineSeconds, [weakThis = weakPtr(), generation] {
        if (auto* directory = weakThis.get())
            directory->handleWatchProof(generation);
    });
}

void SessionDirectoryCore::handleWatchProof(std::uint64_t generation)
{
    if (generation != m_generation.load(std::memory_order_acquire))
        return;
    if (!m_core->isScreenLivenessUnproven()) {
        armWatchProbe();
        return;
    }
    // The path is dead though no transport event says so.
    requestFailure(failureReport(ListingFailure::Unreachable, "the host stopped answering"));
    completeFinish();
}

void SessionDirectoryCore::armScreenTick()
{
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    // One timer out a connection: servicing it again before the timer fires
    // arms no second one.
    if (m_screenTickGeneration == generation)
        return;
    const std::optional<std::uint64_t> delay = m_core->screenTickDelayMicroseconds();
    if (!delay)
        return;
    m_screenTickGeneration = generation;
    m_dispatcher->postDelayed(static_cast<double>(*delay) / 1'000'000.0, [weakThis = weakPtr(), generation] {
        if (auto* directory = weakThis.get())
            directory->handleScreenTick(generation);
    });
}

void SessionDirectoryCore::handleScreenTick(std::uint64_t generation)
{
    if (generation != m_generation.load(std::memory_order_acquire))
        return;
    // A timer of an errand that ended dies here: were it to service the
    // errand since begun, every errand would leave a timer running beside
    // the one that errand armed.
    m_screenTickGeneration = 0;
    serviceScreen();
    completeFinish();
}

void SessionDirectoryCore::handleChannelReady(std::unique_ptr<Ubiquitty::Transport::Channel> channel)
{
    if (!channel) {
        requestFailure(failureReport(ListingFailure::Unreachable, "no connection could be opened"));
        completeFinish();
        return;
    }
    m_channel = std::move(channel);
    if (!m_channel->start(static_cast<Ubiquitty::Transport::ChannelObserver*>(this))) {
        // An immediate local setup error fires no observer callback, so the
        // failure path is synthesized here.
        m_channel.reset();
        requestFailure(failureReport(ListingFailure::Unreachable, "the connection could not be started"));
        completeFinish();
    }
}

void SessionDirectoryCore::requestFinish(Finish finish, TerminateOutcome outcome, DirectoryFailureReport failure)
{
    if (m_errand == Errand::None || m_finish != Finish::None)
        return; // the first answer is the errand's answer
    m_finish = finish;
    m_finishOutcome = outcome;
    m_finishFailure = std::move(failure);
}

void SessionDirectoryCore::requestFailure(DirectoryFailureReport failure)
{
    if (isValidHostInstance(m_expectedDaemon)
        && (failure.failure == ListingFailure::Malformed
            || (failure.failure == ListingFailure::Refused && failure.deniedReason == Protocol::Sync::ByeReason::Malformed))) {
        failure.failure = ListingFailure::InstanceUnreadable;
        failure.hostBuild = m_core->hostBuild();
        const auto& version = failure.hostBuild.appVersion.empty() ? m_expectedDaemon.appVersion : failure.hostBuild.appVersion;
        failure.detail = "This app cannot read the running Ubiquitty host" // copy-ok: the optional version and sentence ending follow
            + (version.empty() ? std::string {} : " (" + version + ")")
            + ". Its sessions keep running. Use a matching build to open them.";
    }
    if (m_errand == Errand::Terminate) {
        requestFinish(Finish::TerminateSettled, TerminateOutcome::Failed);
        return;
    }
    // A watch whose host could not be reached is not over: it dials again, as
    // it does for a host that went away before it answered, which a daemon
    // being replaced does for a moment. Anything the host or ssh said in
    // answer is over.
    if (m_errand == Errand::Watch && (failure.failure == ListingFailure::Unreachable || failure.failure == ListingFailure::EndedBeforeAnswer)) {
        requestFinish(Finish::WatchInterrupted, TerminateOutcome::Failed, std::move(failure));
        return;
    }
    requestFinish(Finish::Failed, TerminateOutcome::Failed, std::move(failure));
}

void SessionDirectoryCore::completeFinish()
{
    if (m_finish == Finish::None)
        return;
    const Finish finish = m_finish;
    const TerminateOutcome outcome = m_finishOutcome;
    const std::uint32_t sessionID = m_targetSessionID;
    DirectoryFailureReport failure = std::move(m_finishFailure);
    if (finish == Finish::WatchInterrupted) {
        // The errand goes on: only its carrier is gone.
        m_finish = Finish::None;
        m_finishFailure = {};
        interruptWatch(failure);
        return;
    }
    if (finish == Finish::ListDelivered || finish == Finish::ManagementDelivered || finish == Finish::CallDelivered || finish == Finish::WatchStopped) {
        // A clean detach, so the host drops this control client on the frame it
        // reads rather than on an end-of-file it has to time out. The Bye goes
        // first: without it the host would hold the connection's attachment
        // for a resume nobody is going to make.
        std::vector<std::uint8_t> frame;
        m_core->leaveScreen();
        m_core->takeScreenOutput(frame);
        (void)writeFrame(frame);
    }
    teardownChannel();
    m_finish = Finish::None;
    m_finishFailure = {};
    m_errand = Errand::None;
    switch (finish) {
    case Finish::None:
        break;
    case Finish::ListDelivered:
        // The tree first, so a reader told of the rows already holds it.
        if (m_listing && m_listing->isListing())
            m_observer->onSessionTree(m_listing->tree());
        m_observer->onSessionList(m_listing && m_listing->isListing() ? sessionRows(m_listing->tree()) : std::vector<SessionRow> {});
        break;
    case Finish::WatchStopped:
    case Finish::WatchInterrupted:
        break;
    case Finish::ManagementDelivered:
        m_observer->onManagementReply(m_managementReply);
        break;
    case Finish::Failed:
        m_observer->onDirectoryFailed(failure);
        break;
    case Finish::TerminateSettled:
        m_observer->onTerminateSettled(sessionID, outcome);
        break;
    case Finish::CallDelivered:
        m_observer->onCallAnswered(m_callResponse);
        break;
    }
}

void SessionDirectoryCore::teardownChannel()
{
    ++m_generation; // invalidate every in-flight callback and timer of the errand being ended
    if (m_channel) {
        m_channel->stop(); // joins the I/O thread; no further observer callbacks
        m_channel.reset();
    }
    m_didSeeAck = false;
}

// ---- ChannelObserver (the channel's I/O thread) -----------------------

void SessionDirectoryCore::onConnect()
{
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    m_dispatcher->post([weakThis = weakPtr(), generation] {
        if (auto* directory = weakThis.get())
            directory->handleConnect(generation);
    });
}

void SessionDirectoryCore::onData(std::span<const std::uint8_t> data)
{
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    std::vector<std::uint8_t> bytes(data.begin(), data.end());
    m_dispatcher->post([weakThis = weakPtr(), generation, bytes = std::move(bytes)] {
        if (auto* directory = weakThis.get())
            directory->handleData(generation, bytes);
    });
}

void SessionDirectoryCore::onClose(const std::string& reason)
{
    const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
    m_dispatcher->post([weakThis = weakPtr(), generation, reason = std::string(reason)] {
        if (auto* directory = weakThis.get())
            directory->handleClose(generation, reason);
    });
}

// ---- dispatcher-thread handlers ---------------------------------------

void SessionDirectoryCore::handleConnect(std::uint64_t generation)
{
    if (generation != m_generation.load(std::memory_order_acquire))
        return;
    sendHello();
    completeFinish();
}

void SessionDirectoryCore::handleData(std::uint64_t generation, const std::vector<std::uint8_t>& bytes)
{
    if (generation != m_generation.load(std::memory_order_acquire))
        return;
    const auto startup = m_bootstrapResponse.consume(bytes);
    switch (startup.status) {
    case BootstrapResponse::Status::Waiting:
        return;
    case BootstrapResponse::Status::Deploy: {
        DirectoryFailureReport report = failureReport(ListingFailure::NotInstalled, {});
        report.remotePlatform = m_bootstrapResponse.platform();
        requestFailure(std::move(report));
        completeFinish();
        return;
    }
    case BootstrapResponse::Status::Invalid:
        requestFailure(failureReport(ListingFailure::Malformed, "the host printed unexpected output before ubiquitty-host started"));
        completeFinish();
        return;
    case BootstrapResponse::Status::InstanceGone:
        // Over, for a watch too: a dial pinned to a stopped daemon reaches it
        // no sooner for waiting.
        requestFailure(failureReport(ListingFailure::InstanceGone, {}));
        completeFinish();
        return;
    case BootstrapResponse::Status::Ready:
        break;
    }
    m_didHearSinceProbeArmed = true;
    const bool isReadable = m_core->feed(std::as_bytes(startup.protocolBytes)); // bool-ok: the decoder's verdict on the bytes just fed
    serviceScreen();
    if (!isReadable) {
        // A Bye on the Sync connection carries the host's reason; any
        // other stop is bytes the decoder cannot read.
        const Protocol::Sync::ClientConnection* screen = m_core->screen();
        if (screen && screen->byeReason()) {
            auto failure = failureReport(ListingFailure::Refused, screen->notice());
            failure.deniedReason = *screen->byeReason();
            requestFailure(std::move(failure));
        } else
            requestFailure(failureReport(ListingFailure::Malformed, {}));
    }
    completeFinish();
}

void SessionDirectoryCore::serviceScreen()
{
    if (m_errand == Errand::None)
        return;
    if (const Protocol::Sync::ClientConnection* screen = m_core->screen(); screen && screen->welcome())
        m_grantedRole = screen->welcome()->grantedRole;
    const bool isManagementTermination = m_errand == Errand::Management && m_managementRequest.operation == Protocol::Sync::ManagementOperation::TerminateSession;
    if ((m_errand == Errand::Terminate || isManagementTermination) && m_finish == Finish::None) {
        const auto target = isManagementTermination ? m_managementRequest.sessionID : m_targetSessionID;
        if (!m_didSendCall && m_didSeeAck && m_core->isScreenAttached()) {
            m_didSendCall = true;
            const auto generation = m_generation.load(std::memory_order_acquire);
            m_terminationRequestID = "ubiquitty.terminate." + std::to_string(generation);
            const auto authority = isManagementTermination ? Protocol::Control::TerminationAuthority::Management : Protocol::Control::TerminationAuthority::Control;
            if (!m_core->call(Protocol::Control::terminationRequest(target, m_terminationRequestID, authority))) {
                if (isManagementTermination)
                    requestFailure(failureReport(ListingFailure::Unreachable, "the request to end the session could not be sent"));
                else
                    requestFinish(Finish::TerminateSettled, TerminateOutcome::Failed);
            } else if (!isManagementTermination)
                m_dispatcher->postDelayed(terminateConsentSeconds, [weakThis = weakPtr(), generation] {
                    if (auto* directory = weakThis.get())
                        directory->handleTerminateDeadline(generation);
                });
        }
        for (const auto& response : m_core->takeCallResults()) {
            bool isJSON = false;
            const auto answer = Codec::JSON::parse(response, isJSON);
            const auto outcome = isJSON ? Protocol::Control::terminationOutcome(answer, m_terminationRequestID, target) : std::nullopt;
            if (!outcome)
                continue;
            using Protocol::Control::TerminationOutcome;
            if (isManagementTermination) {
                m_managementReply.requestID = m_managementRequest.requestID;
                m_managementReply.daemonInstanceID = m_daemonInstanceID;
                m_managementReply.status = *outcome == TerminationOutcome::Terminated                   ? Protocol::Sync::ManagementStatus::Success
                    : *outcome == TerminationOutcome::Refused || *outcome == TerminationOutcome::Denied ? Protocol::Sync::ManagementStatus::Refused
                                                                                                        : Protocol::Sync::ManagementStatus::Failed;
                if (m_managementReply.status != Protocol::Sync::ManagementStatus::Success)
                    m_managementReply.detail = "The host didn’t confirm that the session ended.";
                requestFinish(Finish::ManagementDelivered);
                continue;
            }
            switch (*outcome) {
            case TerminationOutcome::Terminated:
            case TerminationOutcome::Forgotten:
                requestFinish(Finish::TerminateSettled, TerminateOutcome::Terminated);
                break;
            case TerminationOutcome::Denied:
                requestFinish(Finish::TerminateSettled, TerminateOutcome::Denied);
                break;
            case TerminationOutcome::TimedOut:
                requestFinish(Finish::TerminateSettled, TerminateOutcome::TimedOut);
                break;
            case TerminationOutcome::Refused:
                requestFinish(Finish::TerminateSettled, TerminateOutcome::Refused);
                break;
            case TerminationOutcome::Failed:
                requestFinish(Finish::TerminateSettled, TerminateOutcome::Failed);
                break;
            }
        }
    }
    if (m_errand == Errand::Management && !isManagementTermination && m_finish == Finish::None) {
        if (!m_didSendCall && m_didSeeAck && m_core->isScreenAttached()) {
            m_didSendCall = true;
            m_managementCallID = "ubiquitty.management." + std::to_string(m_generation.load(std::memory_order_acquire));
            if (!m_core->call(Protocol::Control::hostAdministrationRequest(m_managementRequest, m_managementCallID)))
                requestFailure(failureReport(ListingFailure::Malformed, "the request could not be sent"));
        }
        for (const auto& response : m_core->takeCallResults()) {
            const auto reply = Protocol::Control::readHostAdministrationResult(response, m_managementCallID, m_managementRequest);
            if (!reply)
                continue;
            m_managementReply = *reply;
            requestFinish(Finish::ManagementDelivered);
        }
    }
    if (m_errand == Errand::Call && m_finish == Finish::None) {
        if (!m_didSendCall && m_core->isScreenAttached()) {
            m_didSendCall = true;
            if (!m_core->call(m_callRequest))
                requestFinish(Finish::Failed, TerminateOutcome::Failed, failureReport(ListingFailure::Malformed, "the request is too large to send"));
        }
        // One call an errand, so the first answer is its answer.
        if (std::vector<std::string> results = m_core->takeCallResults(); !results.empty() && m_finish == Finish::None) {
            m_callResponse = std::move(results.front());
            requestFinish(Finish::CallDelivered);
        }
    }
    serviceListing();
    // What the connection owes the host for the bytes just read: its
    // acknowledgements and the echoes of the host's probes. An answer the
    // connection holds back a moment, and the probe it owes a quiet host
    // later, leave on the timer armed below.
    m_core->tickScreen();
    std::vector<std::uint8_t> frame;
    m_core->takeScreenOutput(frame);
    if (!frame.empty())
        (void)writeFrame(frame);
    armScreenTick();
}

void SessionDirectoryCore::handleClose(std::uint64_t generation, const std::string& reason)
{
    if (generation != m_generation.load(std::memory_order_acquire))
        return;
    if (!m_channel)
        return; // already torn down
    // Only the correlated result can establish a termination's success.
    if (m_errand == Errand::Terminate)
        requestFinish(Finish::TerminateSettled, TerminateOutcome::Failed);
    else if (m_bootstrapResponse.isIncomplete())
        requestFailure(failureReport(ListingFailure::Malformed, "the connection ended before ubiquitty-host started"));
    else {
        // The detail is what ssh said, never the channel's own words for its
        // pipe: "the connection closed" names nothing a person can act on.
        const std::string said = Transport::closeDiagnostic(reason);
        ListingFailure failure = classifyCarrierClose(reason);
        // ssh writes why it could not connect or sign in; a carrier that ended
        // with nothing from either ssh or the host ended on the remote side.
        if (failure == ListingFailure::Unreachable && said.empty() && reason == Transport::peerClosedReason && !m_didSeeAck
            && !m_bootstrapResponse.hasReceivedAnything())
            failure = ListingFailure::EndedBeforeAnswer;
        requestFailure(failureReport(failure, said));
    }
    teardownChannel();
    completeFinish();
}

void SessionDirectoryCore::handleTerminateDeadline(std::uint64_t generation)
{
    if (generation != m_generation.load(std::memory_order_acquire) || m_errand != Errand::Terminate)
        return;
    requestFinish(Finish::TerminateSettled, TerminateOutcome::TimedOut);
    completeFinish();
}

void SessionDirectoryCore::sendHello()
{
    Protocol::Sync::HandshakeRequest hello;
    // A control errand has no grid of its own. The daemon makes a sole client
    // authoritative over the session's size and reflows the buffer to the grid
    // its HELLO claims, and a detached session — the usual thing a person ends
    // from the picker — has no other client to hold the real size. Claiming a
    // size here would shrink and reflow the very session the errand is about to
    // end, and leave it that way whenever the end does not happen. The daemon's
    // resize is guarded on both being greater than zero.
    hello.columns = 0;
    hello.rows = 0;
    // A list binds no session; a terminate binds the one it names, which is the
    // host's attached-requester exemption through its consent gate.
    if (m_errand == Errand::Terminate)
        hello.sessionID = m_targetSessionID;
    else
        hello.target = Protocol::Sync::HandshakeTarget::Control;
    // A terminate is about a session that already exists, so a vanished id must
    // refuse rather than take the daemon's reattach-or-create path — which would
    // mint the session this errand was asked to end, then report it ended.
    if (m_errand == Errand::Terminate)
        hello.sessionFlags |= Protocol::Sync::helloSessionAttachOnly;
    // Administration takes the Manage scope rather than Control, so the host
    // is told which this connection is for before it checks a token.
    if (m_errand == Errand::Management)
        hello.intents.add(Protocol::Sync::HelloIntent::RequestsManagement);
    hello.authToken = m_authToken;
    std::vector<std::uint8_t> frame;
    m_core->buildHello(hello, frame);
    if (!writeFrame(frame))
        requestFailure(failureReport(ListingFailure::Unreachable, {}));
}

bool SessionDirectoryCore::writeFrame(const std::vector<std::uint8_t>& built)
{
    if (built.empty() || !m_channel)
        return false;
    // Only whole client frames leave for the errands.
    std::vector<std::uint8_t> kept;
    if (!m_core->keepFramesHostReads(built, kept))
        return false;
    if (!kept.empty())
        m_channel->send(kept);
    return true;
}

// ---- SyncClientObserver (dispatcher thread, inside feed()) ------------

void SessionDirectoryCore::onHelloAck(std::uint32_t, const std::string& socketName)
{
    if (isValidHostInstance(m_expectedDaemon) && socketName != m_expectedDaemon.socketName) {
        requestFailure(failureReport(ListingFailure::InstanceUnreadable,
            "A different host answered the listing. The selected host has not been reported stopped. Try again to list its sessions."));
        return;
    }
    m_daemonSocketName = socketName;
    m_daemonInstanceID = m_core->daemonInstanceID();
    m_didSeeAck = true;
    if (m_errand == Errand::Management) {
        if (!m_daemonInstanceID || (m_managementRequest.daemonInstanceID && m_managementRequest.daemonInstanceID != m_daemonInstanceID)) {
            requestFinish(Finish::Failed, TerminateOutcome::Failed,
                failureReport(ListingFailure::Refused, "The host restarted. Refresh the list before making changes."));
            return;
        }
        m_managementRequest.daemonInstanceID = m_daemonInstanceID;
        // Every typed management operation waits for the validated Welcome.
    }
}

void SessionDirectoryCore::onRefused(Protocol::Sync::ByeReason reason)
{
    if (m_errand == Errand::Terminate) {
        requestFinish(Finish::TerminateSettled, TerminateOutcome::Refused);
        return;
    }
    DirectoryFailureReport report = failureReport(ListingFailure::Refused, Protocol::Sync::byeNotice(reason));
    report.deniedReason = reason;
    report.hostBuild = m_core->hostBuild();
    report.olderSide = Protocol::Sync::olderSideOfRefusal(reason, report.hostBuild);
    report.remotePlatform = m_core->hostPlatform();
    if ((reason == Protocol::Sync::ByeReason::VersionMismatch || reason == Protocol::Sync::ByeReason::Malformed)
        && isValidHostInstance(m_expectedDaemon)) {
        report.failure = ListingFailure::Malformed;
        requestFailure(std::move(report));
        return;
    }
    requestFinish(Finish::Failed, TerminateOutcome::Failed, std::move(report));
}

void SessionDirectoryCore::onExit()
{
    // serviceScreen reads the correlated result preceding the pane's exit.
    if (m_errand != Errand::Terminate)
        requestFailure(failureReport(ListingFailure::Unreachable, {}));
}

} // namespace Ubiquitty::Session
