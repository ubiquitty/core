// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Pending consent keeps the original session/question and one unapproved
// attachment. Its canonical greeting is decoded immediately, not buffered for
// replay. These Host fixtures remain separate from the pure Core model tests.

#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Host/ScreenPlaneAccess.h"
#include "NotifyRead.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"
#include "TrustedLocalPeer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fcntl.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* what) // bool-ok: the standalone Host assertion harness
{
    if (!ok) {
        std::fprintf(stderr, "consent_pending_toctou: FAIL — %s\n", what);
        ++failures;
    }
}

namespace Ubiquitty::Host {

struct ConsentPendingTOCTOUAccess {
    enum class HumanAdmission : std::uint8_t {
        Admitted,
        BeforeGreeting
    };
    static std::uint32_t makeSession(Daemon& daemon)
    {
        return ScreenPlaneAccess::makeSession(daemon, 24, 80).id;
    }

    static void connect(Daemon::Client& client, PAL::UnixFileDescriptor& peer)
    {
        int descriptors[2] { -1, -1 };
        const auto result = ::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors);
        check(result == 0, "the fixture owns both physical endpoints");
        if (result != 0)
            return;
        client.fileDescriptor.reset(descriptors[0]);
        peer.reset(descriptors[1]);
        check(::fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0, "the sender is nonblocking");
        check(::fcntl(descriptors[1], F_SETFL, O_NONBLOCK) == 0, "the reader is nonblocking");
    }

    static std::size_t addHumanClient(Daemon& daemon, std::uint32_t sessionID, PAL::UnixFileDescriptor& peer,
        HumanAdmission admission = HumanAdmission::Admitted)
    {
        Daemon::Client client;
        connect(client, peer);
        client.clientID = daemon.m_nextClientID++;
        client.sessionID = sessionID;
        seedTrustedLocalPeer(client);
        daemon.m_clients.push_back(std::move(client));
        const auto index = daemon.m_clients.size() - 1;
        if (admission == HumanAdmission::Admitted) {
            ScreenPlaneAccess::greetExisting(daemon, index, Protocol::Sync::ClientKind::Human, Protocol::Sync::Role::Owner);
            check(daemon.hasLiveStructureAttachment(daemon.m_clients[index]), "the human has a live current attachment");
        }
        return index;
    }

    static std::size_t addAgentClient(Daemon& daemon, const std::string& tokenID, PAL::UnixFileDescriptor& peer)
    {
        Daemon::Client client;
        connect(client, peer);
        client.clientID = daemon.m_nextClientID++;
        client.hasToken = true;
        client.tokenID = tokenID;
        client.tokenScope = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive,
            Protocol::Sync::Scope::Control, Protocol::Sync::Scope::NewSession };
        client.agentLabel = "test-agent";
        daemon.m_clients.push_back(std::move(client));
        return daemon.m_clients.size() - 1;
    }

    static void driveGreeting(Daemon& daemon, std::size_t index, std::uint32_t sessionID)
    {
        Protocol::Sync::Greet greet;
        greet.wireVersion = Protocol::Sync::wireVersion;
        greet.appVersion = Protocol::Sync::appVersion();
        greet.clientInstance = { .low = daemon.m_clients[index].clientID, .high = 0x70656e64696e67 };
        greet.clientKind = Protocol::Sync::ClientKind::Agent;
        greet.requestedRole = Protocol::Sync::Role::Agent;
        greet.request = { .target = Protocol::Sync::AdmissionTarget::Session, .sessionID = sessionID, .expectedDaemonInstanceID = daemon.m_daemonInstanceID, .agentLabel = "test-agent", .isAttachOnly = true };
        greet.facts.viewportRows = 24;
        greet.facts.viewportColumns = 80;
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, greet);
        check(Protocol::Sync::decodeGreet(body).has_value(), "the full greeting names the fixture's exact existing target");
        std::vector<std::uint8_t> stream;
        check(Protocol::Sync::appendFrame(stream, Protocol::Sync::FrameKind::Greet, 0, body), "the agent builds a canonical Greet");
        ScreenPlaneAccess::receive(daemon, index, stream);
    }

    static void driveScreen(Daemon& daemon, std::size_t index, std::span<const std::uint8_t> bytes)
    {
        ScreenPlaneAccess::receive(daemon, index, bytes);
    }

    static void call(Daemon& daemon, std::size_t index, std::string_view text)
    {
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Call { std::string(text) });
        std::vector<std::uint8_t> stream;
        check(Protocol::Sync::appendFrame(stream, Protocol::Sync::FrameKind::Call, 0, body), "the fixture builds a canonical Call");
        driveScreen(daemon, index, stream);
    }

    static void answer(Daemon& daemon, std::size_t human, std::uint32_t requestID, Protocol::Sync::ConsentChoice decision)
    {
        const auto text = Protocol::Control::consentResponseRequest(
            { requestID, Protocol::Sync::ConsentKind::Attach, decision }, "fixture.answer");
        check(!text.empty(), "a decision names its exact question kind");
        call(daemon, human, text);
        daemon.serviceSyncConnection(human, ScreenPlaneAccess::now());
    }

    static void service(Daemon& daemon, std::size_t index)
    {
        daemon.serviceSyncConnection(index, ScreenPlaneAccess::now());
        daemon.flushClient(index);
    }

    static std::optional<Protocol::Sync::ID128> attachment(const Daemon& daemon, std::size_t index)
    {
        const auto* connection = daemon.m_clients[index].syncConnection.get();
        const auto* record = connection ? connection->attachment() : nullptr;
        return record ? std::optional { record->attachmentID } : std::nullopt;
    }
    static bool retainsAttachment(const Daemon& daemon, Protocol::Sync::ID128 id) { return daemon.m_attachments->find(id); }
    static bool isAttached(const Daemon& daemon, std::size_t index)
    {
        const auto* connection = daemon.m_clients[index].syncConnection.get();
        return connection && connection->state() == Protocol::Sync::ConnectionState::Attached;
    }
    static bool isConsentPending(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].consentPending; }
    static bool isRefused(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].didOverflow; }
    static bool isListing(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].isListing; }
    static std::uint32_t question(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].consentRequestID; }
    static std::uint32_t target(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].consentSessionID; }
    static std::uint32_t selectedSession(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].sessionID; }
    static std::uint32_t clientID(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].clientID; }
    static std::size_t clientCount(const Daemon& daemon) { return daemon.m_clients.size(); }
    static bool mayRule(const Daemon& daemon, std::size_t index, std::uint32_t session) { return daemon.mayRuleOnConsentFor(daemon.m_clients[index], session); }
    static void disconnect(Daemon& daemon, std::size_t index) { daemon.dropClient(index); }
    static void revoke(Daemon& daemon) { daemon.reverifyTokenClients(); }
    static void failNextWrite(Daemon& daemon, std::size_t index) { daemon.m_clients[index].fileDescriptor.reset(); }
    static void expireQuestion(Daemon& daemon, std::size_t index) { daemon.m_clients[index].consentDeadlineMilliseconds = 1; }
    static void markExiting(Daemon& daemon, std::size_t index) { daemon.m_clients[index].sessionExitDeadlineMilliseconds = 1; }
    static void removeObserveScope(Daemon& daemon, std::size_t index) { daemon.m_clients[index].tokenScope = { Protocol::Sync::Scope::Drive }; }
};

} // namespace Ubiquitty::Host

using Ubiquitty::Host::ConsentPendingTOCTOUAccess;
using Ubiquitty::Host::Daemon;
using Ubiquitty::PAL::UnixFileDescriptor;
namespace Sync = Ubiquitty::Protocol::Sync;
using Access = ConsentPendingTOCTOUAccess;

struct ScreenReplies {
    std::vector<Sync::FrameKind> kinds;
    std::vector<std::string> results;
    std::vector<Sync::ByeReason> refusals;
    std::vector<Sync::ConsentQuestion> questions;
    std::vector<Sync::ConsentStatus> statuses;
    std::vector<Sync::Welcome> welcomes;
    std::optional<std::size_t> welcomeIndex;
    std::optional<std::size_t> pendingIndex;
    std::optional<std::size_t> approvalIndex;
};

static ScreenReplies takeScreenReplies(int peer, Daemon* daemon = nullptr, std::size_t client = 0)
{
    std::array<std::uint8_t, 65536> bytes;
    Sync::FrameDecoder decoder;
    for (std::size_t pass = 0; pass < 256; ++pass) {
        if (daemon)
            Access::service(*daemon, client);
        const auto count = ::recv(peer, bytes.data(), bytes.size(), MSG_DONTWAIT);
        if (count <= 0)
            break;
        decoder.append(std::span(bytes).first(static_cast<std::size_t>(count)));
    }
    ScreenReplies replies;
    while (const auto frame = decoder.next()) {
        replies.kinds.push_back(frame->kind);
        if (frame->kind == Sync::FrameKind::Welcome) {
            if (const auto welcome = Sync::decodeWelcome(frame->body)) {
                replies.welcomes.push_back(*welcome);
                replies.welcomeIndex = replies.kinds.size() - 1;
            }
        } else if (frame->kind == Sync::FrameKind::Result) {
            if (const auto result = Sync::decodeResult(frame->body))
                replies.results.push_back(result->text);
        } else if (frame->kind == Sync::FrameKind::Bye) {
            if (const auto bye = Sync::decodeBye(frame->body))
                replies.refusals.push_back(bye->reason);
        } else if (frame->kind == Sync::FrameKind::Notify) {
            if (const auto notify = UbiquittyTest::notifyOf(frame->body)) {
                if (const auto question = UbiquittyTest::consentQuestionOf(*notify))
                    replies.questions.push_back(*question);
                if (const auto status = UbiquittyTest::consentStatusOf(*notify)) {
                    replies.statuses.push_back(*status);
                    if (status->phase == Sync::ConsentStatusPhase::Pending)
                        replies.pendingIndex = replies.kinds.size() - 1;
                    if (status->phase == Sync::ConsentStatusPhase::Approved)
                        replies.approvalIndex = replies.kinds.size() - 1;
                }
            }
        }
    }
    check(decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0, "all emitted canonical frames are complete and readable");
    return replies;
}

struct PendingAttach {
    Daemon daemon;
    UnixFileDescriptor humanPeer;
    UnixFileDescriptor agentPeer;
    std::uint32_t session = Access::makeSession(daemon);
    std::size_t human = Access::addHumanClient(daemon, session, humanPeer);
    std::size_t agent = Access::addAgentClient(daemon, "held-attach", agentPeer);

    PendingAttach()
    {
        Access::driveGreeting(daemon, agent, session);
        check(Access::isConsentPending(daemon, agent), "the exact attach is held for a human");
    }

    ScreenReplies greet()
    {
        const auto replies = takeScreenReplies(agentPeer.get(), &daemon, agent);
        check(replies.welcomes.size() == 1 && replies.welcomes[0].admission == Sync::AttachmentAdmission::AwaitingConsent,
            "the pending Welcome is explicitly unapproved");
        if (replies.welcomes.size() == 1) {
            const auto& welcome = replies.welcomes.front();
            check(welcome.selection.target == Sync::AdmissionTarget::Session && welcome.selection.sessionID == session
                    && welcome.selection.daemonInstanceID == Ubiquitty::Host::ScreenPlaneAccess::daemonInstanceID(daemon),
                "the pending Welcome retains the independently selected session and daemon");
            check(welcome.consent == Sync::ConsentBinding { Access::question(daemon, agent), session }
                    && Access::attachment(daemon, agent) == std::optional { welcome.attachmentID },
                "the host's one unapproved attachment owns the exact pending question");
        }
        check(replies.statuses == std::vector { Sync::ConsentStatus { .question = { Access::question(daemon, agent), session }, .phase = Sync::ConsentStatusPhase::Pending } },
            "the pending status names the same exact question and session");
        check(replies.welcomeIndex && replies.pendingIndex && *replies.welcomeIndex < *replies.pendingIndex,
            "the unapproved Welcome precedes the exact own Pending status");
        check(!Access::isAttached(daemon, agent), "a pending Welcome grants no attached authority");
        check(replies.questions.empty() && replies.results.empty()
                && std::ranges::count(replies.kinds, Sync::FrameKind::Notify) == 1
                && std::ranges::all_of(replies.kinds, [](auto kind) {
                       return kind == Sync::FrameKind::Welcome || kind == Sync::FrameKind::Notify
                           || kind == Sync::FrameKind::Probe || kind == Sync::FrameKind::Echo;
                   }),
            "a pending attachment sees no question, Result, pane or picture");
        return replies;
    }
};

static void pendingCallsAreRefusedAndNeverReplayed(std::string_view method)
{
    PendingAttach fixture;
    const auto original = Access::question(fixture.daemon, fixture.agent);
    fixture.greet();
    const std::string parameters = method == "session.terminate" || method == "host.session.terminate"
        ? "{\"session\":4294967294}"
        : "{}";
    Access::call(fixture.daemon, fixture.agent,
        "{\"jsonrpc\":\"2.0\",\"id\":\"before-approval\",\"method\":\"" + std::string(method) + "\",\"params\":" + parameters + "}");
    const auto refused = takeScreenReplies(fixture.agentPeer.get(), &fixture.daemon, fixture.agent);
    check(Access::isRefused(fixture.daemon, fixture.agent), "a pending Call is refused, not deferred");
    check(refused.results.empty(), "no pending Call reaches a handler or produces a Result");
    check(!Access::isListing(fixture.daemon, fixture.agent), "the pending Call cannot start a listing");
    Access::answer(fixture.daemon, fixture.human, original, Sync::ConsentChoice::AllowOnce);
    const auto after = takeScreenReplies(fixture.agentPeer.get(), &fixture.daemon, fixture.agent);
    check(!Access::isAttached(fixture.daemon, fixture.agent), "a later answer cannot resurrect a refused attachment");
    check(after.results.empty(), "no rejected Call is replayed after a later answer");
}

static void approvalAdmitsOnlyFreshCallsAndTheirCurrentAuthority()
{
    PendingAttach fixture;
    fixture.greet();
    const auto identity = Access::attachment(fixture.daemon, fixture.agent);
    Access::answer(fixture.daemon, fixture.human, Access::question(fixture.daemon, fixture.agent), Sync::ConsentChoice::AllowOnce);
    const auto admitted = takeScreenReplies(fixture.agentPeer.get(), &fixture.daemon, fixture.agent);
    check(Access::isAttached(fixture.daemon, fixture.agent), "approval admits the original pending connection");
    check(Access::attachment(fixture.daemon, fixture.agent) == identity, "promotion keeps the exact attachment identity");
    const auto pane = std::ranges::find(admitted.kinds, Sync::FrameKind::PaneBind);
    check(admitted.approvalIndex && pane != admitted.kinds.end()
            && *admitted.approvalIndex < static_cast<std::size_t>(pane - admitted.kinds.begin()),
        "the approved status is delivered before the first pane bind");
    check(admitted.results.empty(), "approval itself replays no Call");
    Access::call(fixture.daemon, fixture.agent, R"({"jsonrpc":"2.0","id":"fresh","method":"host.methods","params":{}})");
    const auto fresh = takeScreenReplies(fixture.agentPeer.get(), &fixture.daemon, fixture.agent);
    check(fresh.results.size() == 1 && fresh.results[0].find("\"result\"") != std::string::npos, "a newly submitted admitted Call succeeds");
    Access::removeObserveScope(fixture.daemon, fixture.agent);
    Access::call(fixture.daemon, fixture.agent, R"({"jsonrpc":"2.0","id":"withdrawn","method":"host.methods","params":{}})");
    const auto withdrawn = takeScreenReplies(fixture.agentPeer.get(), &fixture.daemon, fixture.agent);
    check(withdrawn.results.size() == 1 && withdrawn.results[0].find("\"error\"") != std::string::npos,
        "a later Call still checks the current scope after approval");
}

enum class HoldEnd {
    Denied,
    Expired,
    Disconnected,
    Revoked
};

static void endingAPendingAttachErasesItsUnapprovedIdentity(HoldEnd end)
{
    PendingAttach fixture;
    fixture.greet();
    const auto identity = Access::attachment(fixture.daemon, fixture.agent);
    check(identity.has_value(), "the pending attachment has one concrete identity");
    switch (end) {
    case HoldEnd::Denied:
        Access::answer(fixture.daemon, fixture.human, Access::question(fixture.daemon, fixture.agent), Sync::ConsentChoice::Deny);
        break;
    case HoldEnd::Expired:
        Access::expireQuestion(fixture.daemon, fixture.agent);
        break;
    case HoldEnd::Disconnected:
        Access::disconnect(fixture.daemon, fixture.agent);
        break;
    case HoldEnd::Revoked:
        Access::revoke(fixture.daemon);
        break;
    }
    const auto replies = takeScreenReplies(fixture.agentPeer.get(), &fixture.daemon, fixture.agent);
    check(Access::isRefused(fixture.daemon, fixture.agent), "ending the pending attachment closes its client");
    check(!identity || !Access::retainsAttachment(fixture.daemon, *identity), "unapproved identity is erased, never retained as Lost resume authority");
    check(replies.results.empty(), "ending a pending attachment serves no Call");
    if (end == HoldEnd::Denied || end == HoldEnd::Expired) {
        const auto phase = end == HoldEnd::Denied ? Sync::ConsentStatusPhase::Denied : Sync::ConsentStatusPhase::Expired;
        check(replies.statuses.size() == 1 && replies.statuses[0].phase == phase, "denial or expiry delivers the exact own verdict");
        check(replies.refusals == std::vector { Sync::ByeReason::NoConsent }, "a welcomed denial closes with NoConsent");
    } else if (end == HoldEnd::Revoked)
        check(replies.refusals == std::vector { Sync::ByeReason::TokenRevoked }, "credential revocation keeps its authority refusal");
}

static void aFailedPendingRefusalDoesNotEraseItsNeighbor()
{
    PendingAttach fixture;
    fixture.greet();
    UnixFileDescriptor neighborPeer;
    const auto neighbor = Access::addHumanClient(fixture.daemon, fixture.session, neighborPeer);
    const auto count = Access::clientCount(fixture.daemon);
    const auto neighborID = Access::clientID(fixture.daemon, neighbor);
    Access::failNextWrite(fixture.daemon, fixture.agent);
    Access::call(fixture.daemon, fixture.agent, R"({"jsonrpc":"2.0","id":"refused","method":"host.describe","params":{}})");
    Access::service(fixture.daemon, fixture.agent);
    check(Access::isRefused(fixture.daemon, fixture.agent), "a failed refusal write still closes the offending pending client");
    check(Access::clientCount(fixture.daemon) == count, "a failed write defers client erasure until reap");
    check(Access::clientID(fixture.daemon, neighbor) == neighborID && !Access::isRefused(fixture.daemon, neighbor),
        "the neighboring attachment keeps its owner and identity");
}

enum class UnavailableHuman {
    Dropped,
    Exiting,
    BeforeGreeting
};

static void anUnavailableHumanDoesNotKeepAnAttachPending(UnavailableHuman state)
{
    Daemon daemon;
    const auto session = Access::makeSession(daemon);
    UnixFileDescriptor humanPeer;
    UnixFileDescriptor agentPeer;
    const auto human = Access::addHumanClient(daemon, session, humanPeer,
        state == UnavailableHuman::BeforeGreeting ? Access::HumanAdmission::BeforeGreeting : Access::HumanAdmission::Admitted);
    const auto agent = Access::addAgentClient(daemon, "no-live-human", agentPeer);
    switch (state) {
    case UnavailableHuman::Dropped:
        Access::disconnect(daemon, human);
        break;
    case UnavailableHuman::Exiting:
        Access::markExiting(daemon, human);
        break;
    case UnavailableHuman::BeforeGreeting:
        break;
    }
    check(!Access::mayRule(daemon, human, session), "an unavailable human cannot receive or answer the request");
    Access::driveGreeting(daemon, agent, session);
    Access::service(daemon, agent);
    check(!Access::isConsentPending(daemon, agent) && Access::isRefused(daemon, agent), "without an available human the attach is refused immediately");
    check(takeScreenReplies(agentPeer.get()).refusals == std::vector { Sync::ByeReason::NoConsent }, "the unavailable-human refusal is NoConsent");
}

static void aLateHumanAnswersOnlyTheOriginalHeldIdentity()
{
    PendingAttach fixture;
    const auto original = Access::question(fixture.daemon, fixture.agent);
    fixture.greet();
    UnixFileDescriptor arrivingPeer;
    const auto arriving = Access::addHumanClient(fixture.daemon, fixture.session, arrivingPeer);
    const auto delivered = takeScreenReplies(arrivingPeer.get(), &fixture.daemon, arriving);
    check(delivered.questions.size() == 1 && delivered.questions[0].requestID == original && delivered.questions[0].sessionID == fixture.session,
        "the newly admitted human receives the original question without an agent retry");
    Access::answer(fixture.daemon, arriving, original, Sync::ConsentChoice::AllowOnce);
    (void)takeScreenReplies(fixture.agentPeer.get(), &fixture.daemon, fixture.agent);
    check(!Access::isConsentPending(fixture.daemon, fixture.agent) && Access::isAttached(fixture.daemon, fixture.agent), "the late human approves the exact held attachment");
    check(Access::selectedSession(fixture.daemon, fixture.agent) == fixture.session, "the decision binds only the original held session");
}

static void aRepeatedGreetingCannotRetargetOrReviveTheHeldIdentity()
{
    PendingAttach fixture;
    fixture.greet();
    const auto original = Access::question(fixture.daemon, fixture.agent);
    const auto identity = Access::attachment(fixture.daemon, fixture.agent);
    const auto otherSession = Access::makeSession(fixture.daemon);
    Access::driveGreeting(fixture.daemon, fixture.agent, otherSession);
    const auto refused = takeScreenReplies(fixture.agentPeer.get(), &fixture.daemon, fixture.agent);
    check(Access::isRefused(fixture.daemon, fixture.agent), "a repeated canonical greeting closes its pending connection");
    check(refused.refusals == std::vector { Sync::ByeReason::Malformed }, "the repeated greeting is a protocol refusal, not another admission");
    check(!identity || !Access::retainsAttachment(fixture.daemon, *identity), "a refused pending identity is never resumable");
    check(Access::selectedSession(fixture.daemon, fixture.agent) == 0, "a repeated greeting binds no replacement session");
    Access::answer(fixture.daemon, fixture.human, original, Sync::ConsentChoice::AllowOnce);
    check(!Access::isAttached(fixture.daemon, fixture.agent), "an old answer cannot revive the refused attachment");
    check(Access::selectedSession(fixture.daemon, fixture.agent) == 0, "the attempted retarget remains unbound after a later answer");
}

int main()
{
    for (const std::string_view method : { "host.describe", "host.methods", "host.sessions.subscribe", "host.sessions.unsubscribe", "session.terminate", "host.session.terminate" })
        pendingCallsAreRefusedAndNeverReplayed(method);
    approvalAdmitsOnlyFreshCallsAndTheirCurrentAuthority();
    for (const auto end : { HoldEnd::Denied, HoldEnd::Expired, HoldEnd::Disconnected, HoldEnd::Revoked })
        endingAPendingAttachErasesItsUnapprovedIdentity(end);
    aFailedPendingRefusalDoesNotEraseItsNeighbor();
    for (const auto state : { UnavailableHuman::Dropped, UnavailableHuman::Exiting, UnavailableHuman::BeforeGreeting })
        anUnavailableHumanDoesNotKeepAnAttachPending(state);
    aLateHumanAnswersOnlyTheOriginalHeldIdentity();
    aRepeatedGreetingCannotRetargetOrReviveTheHeldIdentity();
    if (!failures)
        std::puts("consent_pending_toctou: PASSED — pending identity is immutable, Calls never replay, and only current exact consent grants admission");
    return failures ? 1 : 0;
}
