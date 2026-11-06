// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/revoke_consent_actions.cpp — the human's side of the consent
// system: taking a grant back.
//
// An agent reaches a session by asking, and a human answering yes may make
// that answer sticky. The consent.revoke Call withdraws that answer, and it is
// the only path by which a grant already given stops applying. It has four
// actions — kick this connection, forget the persisted grant, clear this
// session's allow, kick every agent — and each has to do exactly its own thing:
// a revoke that kicks the wrong client disconnects an innocent one, and a
// revoke that forgets the wrong grant leaves the named agent still allowed.
//
// The gates in front of it are the part worth pinning. The sender must be the
// human (derived from capability origin, never from a self-asserted flag) and
// must name the session it is itself attached to — otherwise one session's human could revoke grants on another's, which is
// the same cross-session reach the attach gate exists to prevent.
//
// Every existing coverer of this file drives a real daemon over a socket as a
// single principal, so none of these refusals ran. This uses the friend seam.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"
#include "ScreenPlaneAccess.h"
#include "TrustedLocalPeer.h"

#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Control/HostAdministration.h"
#include "Core/TerminalOperations.h"
#include "PAL/Stream.h"
#include "Platform/Host/CapabilityGrantFile.h"
#include "Platform/Host/ConsentStore.h"
#include "Platform/Host/TokenStore.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty {
namespace Host {

struct RevokeConsentAccess {
    // Every seeded client gets a real writable descriptor. A default Client
    // carries fd -1, and the daemon writes an EXIT to whoever it kicks — a
    // write to -1 fails EBADF, which routes into dropClient and marks the
    // client, so every assertion about "who was marked" would be measuring the
    // harness rather than the daemon. /dev/null accepts the write and lets the
    // daemon's own decision be the only thing under test.
    static PAL::UnixFileDescriptor writableDescriptor()
    {
        return PAL::UnixFileDescriptor(::open("/dev/null", O_WRONLY));
    }

    static std::size_t addHuman(Daemon& d, std::uint32_t sessionID)
    {
        Daemon::Client c {};
        c.fileDescriptor = writableDescriptor();
        c.clientID = d.m_nextClientID++;
        // The trusted local bridge, so originOf says Local and this client is
        // the human. Local is an identity the daemon takes at accept, not a
        // name, so the seed is the running binary and both halves are set.
        Ubiquitty::Host::seedTrustedLocalPeer(c);
        c.sessionID = sessionID;
        c.paneSubscriptions = { { .sessionID = sessionID } };
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Human, Protocol::Sync::Role::Viewer);
        assert(d.hasLiveStructureAttachment(d.m_clients[index]));
        return index;
    }

    static std::size_t addAgent(Daemon& d, std::uint32_t sessionID, const std::string& peerProcess,
        const std::string& tokenID)
    {
        Daemon::Client c {};
        c.fileDescriptor = writableDescriptor();
        c.clientID = d.m_nextClientID++;
        c.peerProcess = peerProcess;
        c.agentLabel = "an-agent"; // verified-local + self-labeled ⇒ origin Agent
        c.sessionID = sessionID;
        c.paneSubscriptions = { { .sessionID = sessionID } };
        if (!tokenID.empty()) {
            c.hasToken = true;
            c.tokenID = tokenID;
            c.tokenScope = { Protocol::Sync::Scope::Observe };
        }
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        // These fixtures start after outer authentication/attach; the current
        // Greet still creates the real serving attachment and its stable ID.
        // An agent opens a session, never a workspace, and is admitted without
        // a question only on a standing grant: the one a person's
        // allow-for-this-session answer gives.
        ScreenPlaneAccess::grantAgentSession(d, *ScreenPlaneAccess::findSession(d, sessionID), index);
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Agent, Protocol::Sync::Role::Agent);
        assert(d.hasLiveStructureAttachment(d.m_clients[index]));
        return index;
    }

    static void addSession(Daemon& d, std::uint32_t id)
    {
        assert(id >= d.m_nextSessionID);
        d.m_nextSessionID = id;
        const auto* session = &ScreenPlaneAccess::makeSession(d, 2, 4);
        assert(session->id == id);
    }

    // The grant store's directory is derived from the socket path, and an
    // empty socket path resolves to "." — which would drop `consent` and
    // `capability_grants` into whatever directory the test ran from. Point it
    // at a scratch directory so the persistence code runs for real without
    // writing into the tree.
    static void useScratchStore(Daemon& d, const std::string& directory)
    {
        d.m_socketPath = directory + "/daemon.sock";
    }

    static void grantAlways(Daemon& d, std::size_t agentIndex)
    {
        d.m_alwaysAllowedAgents.push_back(d.grantKeyFor(d.m_clients[agentIndex]));
    }

    static bool hasAlwaysGrant(const Daemon& d, std::size_t agentIndex)
    {
        const std::string key = d.grantKeyFor(d.m_clients[agentIndex]);
        for (const std::string& granted : d.m_alwaysAllowedAgents)
            if (granted == key)
                return true;
        return false;
    }

    // Record the pending entry the session policy records when a gated
    // capability resolves to Prompt, and read whether it is still standing.
    // Driving a real capability through a live Terminal would test the
    // chokepoint; what these two are for is the daemon's own bookkeeping
    // around it, which is where the entry is left behind.
    static void recordPending(Daemon& d, std::size_t agentIndex, Ubiquitty::Capability capability)
    {
        d.m_consentQueue.recordPending(capability, d.originOf(d.m_clients[agentIndex]),
            d.grantKeyFor(d.m_clients[agentIndex]));
    }

    static bool isPending(Daemon& d, std::size_t agentIndex, Ubiquitty::Capability capability)
    {
        return d.m_consentQueue.isPending(capability, d.originOf(d.m_clients[agentIndex]),
            d.grantKeyFor(d.m_clients[agentIndex]));
    }

    static void drop(Daemon& d, std::size_t index) { d.dropClient(index); }

    // The record the daemon mints when it asks a human about a capability, and
    // the answer a human sends back. Together they are the path a stale record
    // is answerable through.
    static std::uint32_t askAbout(Daemon& d, std::size_t agentIndex, Ubiquitty::Capability capability)
    {
        const std::uint32_t requestID = d.m_nextConsentRequestID++;
        d.m_pendingCapabilityGrants[requestID] = Daemon::PendingCapabilityGrant {
            .capability = capability,
            .origin = d.originOf(d.m_clients[agentIndex]),
            .identity = d.grantKeyFor(d.m_clients[agentIndex]),
            .sessionID = d.m_clients[agentIndex].sessionID,
            .deadlineMilliseconds = std::numeric_limits<std::uint64_t>::max(),
            .recipients = {},
        };
        // The seeded question has the same exact recipients an actually
        // delivered Notify records; a later unrelated human cannot answer it.
        auto& pending = d.m_pendingCapabilityGrants.at(requestID);
        for (const auto& candidate : d.m_clients)
            if (d.mayRuleOnConsentFor(candidate, pending.sessionID))
                pending.recipients.push_back({ candidate.clientID, pending.sessionID });
        return requestID;
    }

    // Drive the current typed Call ingress and its production service pass.
    // This does not bypass caller authority by calling the adapter directly.
    static void feedCall(Daemon& d, std::size_t index, std::string text)
    {
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Call { std::move(text) });
        std::vector<std::uint8_t> stream;
        const bool didEncode = Protocol::Sync::appendFrame(stream, Protocol::Sync::FrameKind::Call, 0, body);
        assert(didEncode);
        d.receiveScreenBytes(index, stream);
        d.serviceSyncConnection(index, ScreenPlaneAccess::now());
    }

    static void answer(Daemon& d, std::size_t humanIndex, std::uint32_t requestID,
        Protocol::Sync::ConsentChoice decision)
    {
        const auto call = Protocol::Control::consentResponseRequest(
            { requestID, Protocol::Sync::ConsentKind::Capability, decision }, "fixture.answer");
        assert(!call.empty());
        feedCall(d, humanIndex, call);
    }

    static bool isAsked(const Daemon& d, std::uint32_t requestID)
    {
        return d.m_pendingCapabilityGrants.contains(requestID);
    }

    static void grantSession(Daemon& d, std::uint32_t sessionID, std::size_t agentIndex)
    {
        for (const auto& session : d.m_sessions) {
            if (!session || session->id != sessionID)
                continue;
            (void)session->addSessionGrant(d.grantKeyFor(d.m_clients[agentIndex]));
        }
    }

    static void endSession(Daemon& d, std::uint32_t sessionID) { d.destroySession(sessionID); }

    // Whether the session records a grant for the agent's verified key.
    static bool sessionGrantHeld(Daemon& d, std::uint32_t sessionID, std::size_t agentIndex)
    {
        for (const auto& session : d.m_sessions)
            if (session && session->id == sessionID)
                return session->hasSessionGrantFor(d.grantKeyFor(d.m_clients[agentIndex]));
        return false;
    }

    // Whether the agent would be admitted to its session without a prompt.
    static bool standingGrant(const Daemon& d, std::uint32_t sessionID, std::size_t agentIndex)
    {
        const Daemon::Session* session = const_cast<Daemon&>(d).findSession(sessionID);
        return session && d.agentHasStandingGrant(d.m_clients[agentIndex], *session);
    }

    // How many session grants the session records.
    static std::size_t sessionGrantCount(Daemon& d, std::uint32_t sessionID)
    {
        for (const auto& session : d.m_sessions)
            if (session && session->id == sessionID)
                return session->agentSessionGrantIdentities.size();
        return 0;
    }

    static bool holdsGrant(Daemon& d, std::size_t agentIndex, Ubiquitty::Capability capability)
    {
        return d.m_grantStore.checkAndConsumeIfGranted(capability, d.originOf(d.m_clients[agentIndex]),
            d.grantKeyFor(d.m_clients[agentIndex]));
    }

    // An Always grant is recorded the way an answer records it, in the
    // capability grant file, which the store then mirrors.
    static bool grantCapability(Daemon& d, std::size_t agentIndex, Ubiquitty::Capability capability,
        Ubiquitty::CapabilityGrantScope scope)
    {
        const auto origin = d.originOf(d.m_clients[agentIndex]);
        const auto identity = d.grantKeyFor(d.m_clients[agentIndex]);
        if (scope == Ubiquitty::CapabilityGrantScope::Always)
            return d.editAlwaysAllowedCapabilityGrants(CapabilityGrantOperation::Grant, { capability, origin, identity });
        d.m_grantStore.grant(capability, origin, identity, scope, d.m_clients[agentIndex].sessionID);
        return true;
    }

    static bool hasCapability(Daemon& d, std::size_t agentIndex, Ubiquitty::Capability capability)
    {
        return d.m_grantStore.isGranted(capability, d.originOf(d.m_clients[agentIndex]),
            d.grantKeyFor(d.m_clients[agentIndex]));
    }

    static bool persistGrants(Daemon& d, std::size_t agentIndex) { return d.persistAlwaysAllowedAgent(agentIndex); }

    static std::string consentPath(const Daemon& d) { return d.consentStorePath(); }
    static std::string capabilityGrantsPath(const Daemon& d) { return d.capabilityGrantsPath(); }

    static bool storedAttachGrant(const Daemon& d, const std::string& identity)
    {
        const auto contents = readConsentList(d.consentStorePath());
        return contents.isWhole && std::ranges::find(contents.entries, identity) != contents.entries.end();
    }

    static bool storedCapability(const Daemon& d, std::size_t agentIndex, Ubiquitty::Capability capability)
    {
        Daemon reader;
        reader.m_socketPath = d.m_socketPath;
        reader.loadAlwaysAllowedCapabilityGrants();
        return reader.m_grantStore.isGranted(capability, d.originOf(d.m_clients[agentIndex]), d.grantKeyFor(d.m_clients[agentIndex]));
    }

    static Protocol::Sync::ID128 attachmentID(const Daemon& d, std::size_t index)
    {
        const auto* record = d.m_clients[index].syncConnection->attachment();
        assert(record);
        return record->attachmentID;
    }

    /// The retained request and the stable identity of its effects reader.
    struct RetainedResume {
        Protocol::Sync::ID128 clientInstance;
        Protocol::Sync::ResumeRequest request;
    };

    static RetainedResume retainedResume(const Daemon& d, std::size_t index)
    {
        const auto* record = d.m_clients[index].syncConnection->attachment();
        assert(record && !record->panes.empty());
        const auto& pane = *record->panes.front();
        return { .clientInstance = record->clientInstance,
            .request = { .attachmentID = record->attachmentID, .resumeSecret = record->resumeSecret, .panes = { Protocol::Sync::PaneResume { .paneID = pane.paneID, .effectsAcknowledged = pane.effectsAcknowledged } } } };
    }

    static std::size_t resumeOn(Daemon& d, std::uint32_t sessionID, const std::string& tokenID,
        const RetainedResume& resume)
    {
        Daemon::Client client;
        client.fileDescriptor = writableDescriptor();
        client.clientID = d.m_nextClientID++;
        client.hasToken = true;
        client.tokenID = tokenID;
        client.tokenScope = { Protocol::Sync::Scope::Observe };
        d.m_clients.push_back(std::move(client));
        const auto index = d.m_clients.size() - 1;
        Protocol::Sync::Greet greeting;
        greeting.wireVersion = Protocol::Sync::wireVersion;
        greeting.appVersion = Protocol::Sync::appVersion();
        greeting.clientInstance = resume.clientInstance;
        greeting.clientKind = Protocol::Sync::ClientKind::Agent;
        greeting.requestedRole = Protocol::Sync::Role::Agent;
        greeting.request.target = Protocol::Sync::AdmissionTarget::Session;
        greeting.request.sessionID = sessionID;
        greeting.request.expectedDaemonInstanceID = d.m_daemonInstanceID;
        greeting.request.agentLabel = "an-agent";
        greeting.request.isAttachOnly = true;
        greeting.resume = resume.request;
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, greeting);
        assert(Protocol::Sync::decodeGreet(body));
        std::vector<std::uint8_t> bytes;
        const bool didEncode = Protocol::Sync::appendFrame(bytes, Protocol::Sync::FrameKind::Greet, 0, body);
        assert(didEncode);
        d.receiveScreenBytes(index, bytes);
        d.serviceSyncConnection(index, ScreenPlaneAccess::now());
        return index;
    }

    static bool continuedAttachment(const Daemon& d, std::size_t index, const RetainedResume& resume)
    {
        const auto& client = d.m_clients[index];
        const auto* connection = client.syncConnection.get();
        const auto* record = connection ? connection->attachment() : nullptr;
        return connection && connection->state() == Protocol::Sync::ConnectionState::Attached && !connection->resumeRefusal()
            && record && record->attachmentID == resume.request.attachmentID
            && record->state == Protocol::Sync::AttachmentState::Live && record->servingConnection == client.clientID;
    }

    static bool refusedRevokedAttachment(const Daemon& d, std::size_t index, const RetainedResume& resume)
    {
        const auto* connection = d.m_clients[index].syncConnection.get();
        const auto* record = connection ? connection->attachment() : nullptr;
        return connection && connection->state() == Protocol::Sync::ConnectionState::Attached
            && connection->resumeRefusal() == Protocol::Sync::ResumeRefusal::UnknownAttachment
            && record && record->attachmentID != resume.request.attachmentID;
    }

    static bool showsSession(const Daemon& d, std::size_t index, std::uint32_t sessionID)
    {
        const auto* subscription = d.m_clients[index].subscriptionFor(sessionID);
        return subscription && subscription->screenHandle;
    }

    static void unbindPanes(Daemon& d, std::size_t index)
    {
        auto& client = d.m_clients[index];
        for (const auto& subscription : client.paneSubscriptions)
            if (subscription.screenHandle)
                client.syncConnection->unbindPane(subscription.screenHandle, 0);
        client.paneSubscriptions.clear();
    }

    static std::size_t addHeldAgent(Daemon& d, std::uint32_t sessionID)
    {
        Daemon::Client client;
        client.clientID = d.m_nextClientID++;
        client.fileDescriptor = writableDescriptor();
        client.peerProcess = "/usr/bin/agent-pending";
        client.agentLabel = "an-agent";
        client.hasToken = true;
        client.tokenID = "tok-pending";
        client.tokenScope = { Protocol::Sync::Scope::Observe };
        client.sessionID = sessionID;
        d.m_clients.push_back(std::move(client));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Agent, Protocol::Sync::Role::Agent);
        assert(d.m_clients[index].syncConnection->state() == Protocol::Sync::ConnectionState::AwaitingConsent);
        // The fixture selected the request before Greet. A fresh socket has
        // no admitted session until its pending question is taken.
        d.m_clients[index].sessionID = 0;
        return index;
    }

    static void approveHeldAgent(Daemon& d, std::size_t human, std::size_t agent)
    {
        const auto requestID = d.m_clients[agent].consentRequestID;
        feedCall(d, human, Protocol::Control::consentResponseRequest({ requestID, Protocol::Sync::ConsentKind::Attach, Protocol::Sync::ConsentChoice::AllowOnce }, "fixture.attach-answer"));
        assert(d.m_clients[agent].attachConsentApproval);
    }

    static bool isAdmittedAfterService(Daemon& d, std::size_t agent)
    {
        d.servicePendingAttachConsent(agent);
        return d.m_clients[agent].syncConnection->state() == Protocol::Sync::ConnectionState::Attached;
    }

    static void revokeSessionGrantFromManagement(Daemon& d, std::size_t human, std::uint32_t sessionID, const std::string& identity)
    {
        Protocol::Sync::ManagementRequest request;
        request.requestID = 1;
        request.operation = Protocol::Sync::ManagementOperation::RevokeGrant;
        request.daemonInstanceID = d.m_daemonInstanceID;
        request.identity = "session:" + std::to_string(sessionID) + ":" + identity;
        const auto call = Protocol::Control::hostAdministrationRequest(request, "fixture.management-revoke");
        assert(!call.empty());
        feedCall(d, human, call);
    }
    static std::string grantKey(const Daemon& d, std::size_t index) { return d.grantKeyFor(d.m_clients[index]); }
    // A refusal drains to its deadline; withdrawn authority may drop the
    // connection immediately. Both await the loop's separate reap pass.
    static bool kicked(const Daemon& d, std::size_t index)
    {
        return ScreenPlaneAccess::isLeaving(d, index) || ScreenPlaneAccess::isDropped(d, index);
    }

    static void feedRevoke(Daemon& d, std::size_t index, const Protocol::Control::ConsentRevocation& message)
    {
        const auto call = Protocol::Control::consentRevocationRequest(message, "fixture.revoke");
        assert(!call.empty());
        feedCall(d, index, call);
    }

    static std::optional<Codec::JSON::Value> revokeResult(Daemon& d, std::size_t index, const Protocol::Control::ConsentRevocation& message)
    {
        std::array<int, 2> descriptors { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0)
            return std::nullopt;
        PAL::UnixFileDescriptor reader(descriptors[0]);
        auto previous = std::move(d.m_clients[index].fileDescriptor);
        d.m_clients[index].fileDescriptor = PAL::UnixFileDescriptor(descriptors[1]);
        feedRevoke(d, index, message);
        d.drainSyncConnection(index, ScreenPlaneAccess::now());
        d.flushClient(index);
        Protocol::Sync::FrameDecoder decoder;
        std::array<std::uint8_t, 8192> bytes;
        for (;;) {
            const auto count = ::recv(reader.get(), bytes.data(), bytes.size(), MSG_DONTWAIT);
            if (count > 0)
                decoder.append(std::span(bytes).first(static_cast<std::size_t>(count)));
            else if (count < 0 && errno == EINTR)
                continue;
            else
                break;
        }
        d.m_clients[index].fileDescriptor = std::move(previous);
        std::optional<Codec::JSON::Value> answer;
        while (const auto frame = decoder.next()) {
            if (frame->kind != Protocol::Sync::FrameKind::Result)
                continue;
            const auto result = Protocol::Sync::decodeResult(frame->body);
            if (!result || result->transfer)
                continue;
            bool isValid = false;
            auto value = Codec::JSON::parse(result->text, isValid);
            if (isValid && value.stringOr("id") == "fixture.revoke")
                answer = std::move(value);
        }
        return decoder.error() == Protocol::Sync::EnvelopeError::None && !decoder.bufferedBytes() ? answer : std::nullopt;
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::RevokeConsentAccess;
namespace Control = Ubiquitty::Protocol::Control;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "revoke_consent_actions: FAIL — %s\n", what);
        ++failures;
    }
}

constexpr std::uint32_t theSession = 501;
constexpr std::uint32_t anotherSession = 502;

} // namespace

int main()
{
    // One scratch directory for the whole run: the daemon persists its grant
    // store beside its socket, and a test that leaves those files in the
    // working directory has written into the tree.
    char directoryTemplate[] = "/tmp/ubiquitty-revoke-XXXXXX";
    const char* madeDirectory = ::mkdtemp(directoryTemplate);
    if (!madeDirectory) {
        std::fprintf(stderr, "revoke_consent_actions: could not create a scratch directory\n");
        return 1;
    }
    const std::string scratchDirectory(madeDirectory);

    // (a) DisconnectNow kicks the named agent and nobody else. A revoke that
    // walked the client table by anything but the id would take out the
    // neighbour sitting next to it.
    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        const std::size_t human = RevokeConsentAccess::addHuman(d, theSession);
        const std::size_t target = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-a", "tok-a");
        const std::size_t bystander = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-b", "tok-b");

        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::DisconnectNow;
        revoke.sessionID = theSession;
        revoke.attachmentID = RevokeConsentAccess::attachmentID(d, target);
        RevokeConsentAccess::feedRevoke(d, human, revoke);

        check(RevokeConsentAccess::kicked(d, target), "DisconnectNow kicks the agent it names");
        check(!RevokeConsentAccess::kicked(d, bystander), "DisconnectNow leaves the other agent connected");
        check(!RevokeConsentAccess::kicked(d, human), "DisconnectNow does not kick the human who sent it");
    }

    // (b) A real stable attachment on ANOTHER session is not reachable:
    // knowing its ID cannot expand the human's exact subscribed session.
    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        RevokeConsentAccess::addSession(d, anotherSession);
        const std::size_t human = RevokeConsentAccess::addHuman(d, theSession);
        const std::size_t elsewhere = RevokeConsentAccess::addAgent(d, anotherSession, "/usr/bin/agent-c", "tok-c");

        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::DisconnectNow;
        revoke.sessionID = theSession; // the human's own session
        revoke.attachmentID = RevokeConsentAccess::attachmentID(d, elsewhere);
        RevokeConsentAccess::feedRevoke(d, human, revoke);
        check(!RevokeConsentAccess::kicked(d, elsewhere), "an agent on another session is out of reach");
    }

    // (c) The session field must be the human's own. Naming someone else's
    // session is refused outright rather than acted on.
    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        RevokeConsentAccess::addSession(d, anotherSession);
        const std::size_t human = RevokeConsentAccess::addHuman(d, theSession);
        const std::size_t agent = RevokeConsentAccess::addAgent(d, anotherSession, "/usr/bin/agent-d", "tok-d");

        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::DisconnectAll;
        revoke.sessionID = anotherSession; // not the sender's session
        RevokeConsentAccess::feedRevoke(d, human, revoke);
        check(!RevokeConsentAccess::kicked(d, agent), "a revoke naming another session is refused");

        // And session 0 is not a wildcard meaning "all of them".
        // Independent malformed input reaches the decoder; the producer's
        // own rejection of session zero is not the refusal under test.
        RevokeConsentAccess::feedCall(d, human,
            R"({"jsonrpc":"2.0","id":"fixture.zero","method":"consent.revoke","params":{"session":"session:0","action":"disconnectAgents"}})");
        check(!RevokeConsentAccess::kicked(d, agent), "session 0 is not a wildcard");
    }

    // (d) The sender must be the human. An agent that sends a revoke — however
    // it labels itself — is refused, or an agent could revoke the grants of
    // every other agent on the session it shares.
    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        const std::size_t sender = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-e", "tok-e");
        const std::size_t victim = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-f", "tok-f");

        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::DisconnectNow;
        revoke.sessionID = theSession;
        revoke.attachmentID = RevokeConsentAccess::attachmentID(d, victim);
        RevokeConsentAccess::feedRevoke(d, sender, revoke);
        check(!RevokeConsentAccess::kicked(d, victim), "an agent cannot revoke another agent");
    }

    // (e) StopAlways forgets the persisted grant, and the grant key is derived
    // server-side from the live client. The wire carries the observed verified
    // grant key, not a claimed label; the host still matches a live agent on
    // this session before acting on it.
    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        const std::size_t human = RevokeConsentAccess::addHuman(d, theSession);
        const std::size_t agent = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-h", "tok-h");
        RevokeConsentAccess::grantAlways(d, agent);
        check(RevokeConsentAccess::hasAlwaysGrant(d, agent), "the agent starts with a persisted grant");

        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::StopAlways;
        revoke.sessionID = theSession;
        revoke.identity = RevokeConsentAccess::grantKey(d, agent);
        // A capability this agent asked for and nobody has ruled on yet. The
        // revoke has to drop it, because the queue dedupes: a recorded triple
        // suppresses every later request for the same triple, so an entry left
        // behind is not a leak but a permanent denial. "Stop always allowing
        // this agent" would otherwise mean "and never let me allow it again",
        // with the human never asked and the chokepoint answering Prompt with
        // no prompt behind it. The only exit is a daemon restart.
        RevokeConsentAccess::recordPending(d, agent, Ubiquitty::Capability::SetColor);
        check(RevokeConsentAccess::isPending(d, agent, Ubiquitty::Capability::SetColor),
            "the agent has a question outstanding before the revoke");

        RevokeConsentAccess::feedRevoke(d, human, revoke);
        check(!RevokeConsentAccess::hasAlwaysGrant(d, agent), "StopAlways forgets the persisted grant");
        check(!RevokeConsentAccess::isPending(d, agent, Ubiquitty::Capability::SetColor),
            "StopAlways drops the questions nobody ruled on, so the agent can be asked about again");
    }

    // A capability write failure leaves the two persisted grants untouched.
    // Once capability removal commits, a failed attach edit never restores it.
    for (const bool shouldFailCapabilityWrite : { true, false }) {
        const std::string directory = scratchDirectory + (shouldFailCapabilityWrite ? "/capability-error" : "/consent-error");
        const bool didCreateDirectory = ::mkdir(directory.c_str(), 0700) == 0;
        check(didCreateDirectory, "the persistence failure has a private directory");
        if (!didCreateDirectory)
            return 1;
        {
            Daemon d;
            RevokeConsentAccess::useScratchStore(d, directory);
            RevokeConsentAccess::addSession(d, theSession);
            const auto human = RevokeConsentAccess::addHuman(d, theSession);
            const auto agent = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-write-error", "tok-write-error");
            const auto bystander = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-kept", "tok-kept");
            const auto capability = Ubiquitty::Capability::FileReadAccess;
            check(RevokeConsentAccess::grantCapability(d, agent, capability, Ubiquitty::CapabilityGrantScope::Always)
                    && RevokeConsentAccess::grantCapability(d, bystander, capability, Ubiquitty::CapabilityGrantScope::Always)
                    && RevokeConsentAccess::persistGrants(d, agent) && RevokeConsentAccess::persistGrants(d, bystander),
                "both principals begin with real persisted grants");
            const auto identity = RevokeConsentAccess::grantKey(d, agent);
            const auto bystanderIdentity = RevokeConsentAccess::grantKey(d, bystander);
            check(RevokeConsentAccess::storedAttachGrant(d, identity) && RevokeConsentAccess::storedCapability(d, agent, capability),
                "the target grants reload before the failure");
            RevokeConsentAccess::recordPending(d, agent, Ubiquitty::Capability::SetColor);
            RevokeConsentAccess::recordPending(d, bystander, Ubiquitty::Capability::SetColor);

            std::optional<Ubiquitty::Host::TokenStoreLock> heldLock;
            heldLock.emplace(shouldFailCapabilityWrite ? RevokeConsentAccess::capabilityGrantsPath(d) : RevokeConsentAccess::consentPath(d));
            check(heldLock->isLocked(), shouldFailCapabilityWrite ? "only the capability grant edit is locked" : "only the attach-consent edit is locked");
            Control::ConsentRevocation revoke {};
            revoke.action = Sync::ConsentRevocationKind::StopAlways;
            revoke.sessionID = theSession;
            revoke.identity = identity;
            const auto failed = RevokeConsentAccess::revokeResult(d, human, revoke);
            const auto* error = failed ? failed->find("error") : nullptr;
            check(error && error->isObject() && error->numberOr("code") == std::to_underlying(Control::ErrorCode::InternalError),
                "a failed store edit is reported as an RPC error");
            check(RevokeConsentAccess::hasAlwaysGrant(d, agent) && RevokeConsentAccess::storedAttachGrant(d, identity),
                "the failed transaction keeps the uncommitted attach grant");
            check(RevokeConsentAccess::hasCapability(d, agent, capability) == shouldFailCapabilityWrite
                    && RevokeConsentAccess::storedCapability(d, agent, capability) == shouldFailCapabilityWrite,
                "only a committed capability withdrawal changes live and reloaded authority");
            check(RevokeConsentAccess::isPending(d, agent, Ubiquitty::Capability::SetColor) == shouldFailCapabilityWrite,
                "committed capability withdrawal retires held questions");
            check(!RevokeConsentAccess::kicked(d, agent), "an incomplete revoke leaves the target available for retry");
            check(RevokeConsentAccess::hasAlwaysGrant(d, bystander) && RevokeConsentAccess::storedAttachGrant(d, bystanderIdentity)
                    && RevokeConsentAccess::hasCapability(d, bystander, capability) && RevokeConsentAccess::storedCapability(d, bystander, capability)
                    && RevokeConsentAccess::isPending(d, bystander, Ubiquitty::Capability::SetColor) && !RevokeConsentAccess::kicked(d, bystander),
                "the failed edit preserves another principal's grants and questions");

            heldLock.reset();
            const auto retried = RevokeConsentAccess::revokeResult(d, human, revoke);
            check(retried && retried->find("result") && !retried->find("error"), "the revoke succeeds after the store is writable");
            check(!RevokeConsentAccess::hasAlwaysGrant(d, agent) && !RevokeConsentAccess::storedAttachGrant(d, identity)
                    && !RevokeConsentAccess::hasCapability(d, agent, capability) && !RevokeConsentAccess::storedCapability(d, agent, capability)
                    && !RevokeConsentAccess::isPending(d, agent, Ubiquitty::Capability::SetColor) && RevokeConsentAccess::kicked(d, agent),
                "the retry withdraws live and persisted grants and disconnects the target");
            check(RevokeConsentAccess::storedAttachGrant(d, bystanderIdentity) && RevokeConsentAccess::storedCapability(d, bystander, capability)
                    && !RevokeConsentAccess::kicked(d, bystander),
                "the completed revoke preserves another principal");
        }
        ::unlink((directory + "/consent").c_str());
        ::unlink((directory + "/consent.lock").c_str());
        ::unlink((directory + "/capability_grants").c_str());
        ::unlink((directory + "/capability_grants.lock").c_str());
        check(::rmdir(directory.c_str()) == 0, "the persistence case leaves no scratch directory");
    }

    // (e1b) A revoke the human answers their way out of. The question the
    //       daemon asked is a record keyed by a sequential request id, and the
    //       response arm grants from that record — so a record surviving the
    //       revoke is not a leak but a live authorization. A human who had the
    //       dialog open, a queued frame, a second frontend, or anything that
    //       answers an id after the fact re-grants and re-persists exactly what
    //       the revoke had just taken away. The ids are sequential, so nobody
    //       needing to guess one is not a defence either.
    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        const std::size_t human = RevokeConsentAccess::addHuman(d, theSession);
        const std::size_t agent = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-stale", "tok-stale");
        RevokeConsentAccess::grantAlways(d, agent);
        const std::uint32_t controlQuestion = RevokeConsentAccess::askAbout(d, agent, Ubiquitty::Capability::SetHyperlink);
        RevokeConsentAccess::answer(d, human, controlQuestion, Sync::ConsentChoice::AllowOnce);
        check(RevokeConsentAccess::holdsGrant(d, agent, Ubiquitty::Capability::SetHyperlink),
            "the exact recipient can answer a live question before revocation");
        const std::uint32_t question = RevokeConsentAccess::askAbout(d, agent, Ubiquitty::Capability::SetColor);

        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::StopAlways;
        revoke.sessionID = theSession;
        revoke.identity = RevokeConsentAccess::grantKey(d, agent);
        RevokeConsentAccess::feedRevoke(d, human, revoke);
        check(!RevokeConsentAccess::hasAlwaysGrant(d, agent), "the revoke took the standing grant");

        RevokeConsentAccess::answer(d, human, question, Sync::ConsentChoice::AllowAlways);
        check(!RevokeConsentAccess::holdsGrant(d, agent, Ubiquitty::Capability::SetColor),
            "an answer to a question the revoke should have withdrawn does not undo the revoke");
    }

    // (e1c) The session-scoped revoke withdraws its own questions and no more.
    //       A principal can be asked about two sessions at once, and this action
    //       names one of them: withdrawing every question would revoke authority
    //       on a session the human said nothing about.
    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        RevokeConsentAccess::addSession(d, theSession + 1);
        const std::size_t human = RevokeConsentAccess::addHuman(d, theSession);
        const std::size_t here = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-two", "tok-two");
        const std::size_t elsewhere = RevokeConsentAccess::addAgent(d, theSession + 1, "/usr/bin/agent-two", "tok-two");
        RevokeConsentAccess::grantSession(d, theSession, here);
        const std::uint32_t asked = RevokeConsentAccess::askAbout(d, here, Ubiquitty::Capability::SetColor);
        const std::uint32_t askedElsewhere = RevokeConsentAccess::askAbout(d, elsewhere, Ubiquitty::Capability::SetHyperlink);

        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::SessionGrant;
        revoke.sessionID = theSession;
        revoke.identity = RevokeConsentAccess::grantKey(d, here);
        RevokeConsentAccess::feedRevoke(d, human, revoke);

        check(!RevokeConsentAccess::isAsked(d, asked),
            "the session revoke withdraws the question it was asked about");
        check(RevokeConsentAccess::isAsked(d, askedElsewhere),
            "and leaves the same principal's question about another session standing");
    }

    // (e1d) A session going away takes its questions. A record names the session
    //       whose human may rule on it, so one that outlives the session can
    //       never be ruled on and is reachable only by an id nobody should send.
    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        const std::size_t agent = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-gone", "tok-gone");
        const std::uint32_t asked = RevokeConsentAccess::askAbout(d, agent, Ubiquitty::Capability::SetColor);
        check(RevokeConsentAccess::isAsked(d, asked), "the question stands while the session does");

        RevokeConsentAccess::endSession(d, theSession);
        check(!RevokeConsentAccess::isAsked(d, asked), "and goes when the session does");
    }

    // (e2) A question outlives neither the connection that raised it nor, when
    // two processes of one agent share a grant key, the last of them. The first
    // half is the same permanent denial the revoke arm has: an agent whose link
    // drops mid-prompt is asking again the moment it reconnects, and a surviving
    // entry makes that ask a silent repeat. The second is why the drop is
    // conditional — two admitted clients using the same valid bearer share one
    // verified grant identity, and the first to leave must not take the other's
    // question. No successful tokenless attach is manufactured by this case.
    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        const std::size_t first = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-pair", "tok-pair");
        const std::size_t second = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-pair", "tok-pair");
        RevokeConsentAccess::recordPending(d, first, Ubiquitty::Capability::SetColor);
        check(RevokeConsentAccess::isPending(d, first, Ubiquitty::Capability::SetColor),
            "the pair has a question outstanding");

        RevokeConsentAccess::drop(d, first);
        check(RevokeConsentAccess::isPending(d, second, Ubiquitty::Capability::SetColor),
            "one process of a pair leaving holds the question the other can still be asked about");

        RevokeConsentAccess::drop(d, second);
        check(!RevokeConsentAccess::isPending(d, second, Ubiquitty::Capability::SetColor),
            "the last holder of an identity leaving drops the question with it");
    }
    {
        // The same revoke naming a verified identity nobody here carries:
        // no grant is forgotten, rather than the first one found.
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        const std::size_t human = RevokeConsentAccess::addHuman(d, theSession);
        const std::size_t agent = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-i", "tok-i");
        RevokeConsentAccess::grantAlways(d, agent);

        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::StopAlways;
        revoke.sessionID = theSession;
        revoke.identity = "token:unobserved";
        RevokeConsentAccess::feedRevoke(d, human, revoke);
        check(RevokeConsentAccess::hasAlwaysGrant(d, agent),
            "a StopAlways matching no observed agent forgets nothing");
    }

    // (f) DisconnectAll kicks every agent on the session and leaves the human
    // holding it — a revoke that took the human down with the agents would
    // close the window the person is using to revoke.
    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        RevokeConsentAccess::addSession(d, anotherSession);
        const std::size_t human = RevokeConsentAccess::addHuman(d, theSession);
        const std::size_t first = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-j", "tok-j");
        const std::size_t second = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-k", "tok-k");
        const std::size_t elsewhere = RevokeConsentAccess::addAgent(d, anotherSession, "/usr/bin/agent-l", "tok-l");

        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::DisconnectAll;
        revoke.sessionID = theSession;
        RevokeConsentAccess::feedRevoke(d, human, revoke);

        check(RevokeConsentAccess::kicked(d, first), "DisconnectAll kicks the first agent");
        check(RevokeConsentAccess::kicked(d, second), "DisconnectAll kicks the second agent");
        check(!RevokeConsentAccess::kicked(d, human), "DisconnectAll leaves the human attached");
        check(!RevokeConsentAccess::kicked(d, elsewhere), "DisconnectAll stops at the session boundary");
    }

    // (g) SessionGrant clears this session's allow without touching the
    // persisted one: the two grants are separate, and a human clearing the
    // session's should not silently revoke the standing answer as well.
    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        const std::size_t human = RevokeConsentAccess::addHuman(d, theSession);
        const std::size_t agent = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-m", "tok-m");
        RevokeConsentAccess::grantAlways(d, agent);
        RevokeConsentAccess::grantSession(d, theSession, agent);

        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::SessionGrant;
        revoke.sessionID = theSession;
        revoke.identity = RevokeConsentAccess::grantKey(d, agent);
        RevokeConsentAccess::feedRevoke(d, human, revoke);
        check(RevokeConsentAccess::kicked(d, agent), "clearing the active session grant kicks its holder");
        check(RevokeConsentAccess::hasAlwaysGrant(d, agent),
            "clearing the session grant leaves the persisted one alone");
    }

    // (g2) The session's grants are per principal. The agent that created the
    // session holds one; a person approving a second agent adds a grant beside
    // it rather than overwriting it, so the creator keeps its standing access
    // and can still act on its own session without a fresh prompt. Revoking
    // one grant leaves the other, and each is named on its own.
    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        const std::size_t human = RevokeConsentAccess::addHuman(d, theSession);
        const std::size_t creator = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-creator", "tok-creator");
        const std::size_t second = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-second", "tok-second");
        // The creator's grant, as createSession records it.
        RevokeConsentAccess::grantSession(d, theSession, creator);
        check(RevokeConsentAccess::standingGrant(d, theSession, creator), "the creator starts with a standing grant");

        // A person approves the second agent on the same session.
        RevokeConsentAccess::grantSession(d, theSession, second);
        check(RevokeConsentAccess::sessionGrantCount(d, theSession) == 2,
            "approving a second agent adds a grant rather than replacing the creator's");
        check(RevokeConsentAccess::standingGrant(d, theSession, creator),
            "the creator keeps its standing grant after another agent is approved");
        check(RevokeConsentAccess::standingGrant(d, theSession, second), "the approved agent is admitted too");

        // Revoking the second agent's grant leaves the creator's.
        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::SessionGrant;
        revoke.sessionID = theSession;
        revoke.identity = RevokeConsentAccess::grantKey(d, second);
        RevokeConsentAccess::feedRevoke(d, human, revoke);
        check(!RevokeConsentAccess::sessionGrantHeld(d, theSession, second), "revoking one grant drops that agent's");
        check(RevokeConsentAccess::standingGrant(d, theSession, creator),
            "and leaves the creator's grant and its standing access intact");
    }

    // Both a bound and an unbound retained pane can resume before its grant
    // is withdrawn, under the original reader's identity.
    for (const bool wasUnbound : { false, true }) {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        RevokeConsentAccess::addSession(d, anotherSession);
        RevokeConsentAccess::addAgent(d, anotherSession, "/usr/bin/agent-resume", "tok-resume");
        const auto lost = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-resume", "tok-resume");
        const auto resume = RevokeConsentAccess::retainedResume(d, lost);
        if (wasUnbound)
            RevokeConsentAccess::unbindPanes(d, lost);
        Ubiquitty::Host::ScreenPlaneAccess::reap(d, lost);
        const auto replacement = RevokeConsentAccess::resumeOn(d, anotherSession, "tok-resume", resume);
        check(RevokeConsentAccess::continuedAttachment(d, replacement, resume), "an unrevoked retained attachment resumes as the same live record");
        check(RevokeConsentAccess::showsSession(d, replacement, theSession), "an unrevoked retained pane binds despite another greeting target");
    }

    // A lost attachment still holds its panes. Withdrawing their grant ends
    // that authority even when the next greeting selects an unrelated session.
    for (const auto action : { Sync::ConsentRevocationKind::SessionGrant, Sync::ConsentRevocationKind::DisconnectAll }) {
        for (const bool wasUnbound : { false, true }) {
            Daemon d;
            RevokeConsentAccess::useScratchStore(d, scratchDirectory);
            RevokeConsentAccess::addSession(d, theSession);
            RevokeConsentAccess::addSession(d, anotherSession);
            const auto human = RevokeConsentAccess::addHuman(d, theSession);
            const auto independent = RevokeConsentAccess::addAgent(d, anotherSession, "/usr/bin/agent-resume", "tok-resume");
            const auto bystander = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-other", "tok-other");
            const auto lost = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-resume", "tok-resume");
            const auto resume = RevokeConsentAccess::retainedResume(d, lost);
            const auto identity = RevokeConsentAccess::grantKey(d, lost);
            if (wasUnbound)
                RevokeConsentAccess::unbindPanes(d, lost);
            Ubiquitty::Host::ScreenPlaneAccess::reap(d, lost);
            check(Ubiquitty::Host::ScreenPlaneAccess::attachments(d).find(resume.request.attachmentID) != nullptr, "transport loss retains resumable pane authority");
            Control::ConsentRevocation revoke {};
            revoke.action = action;
            revoke.sessionID = theSession;
            if (action == Sync::ConsentRevocationKind::SessionGrant)
                revoke.identity = identity;
            RevokeConsentAccess::feedRevoke(d, human, revoke);
            check(!Ubiquitty::Host::ScreenPlaneAccess::attachments(d).find(resume.request.attachmentID), "revocation also retires lost attachments");
            check(!RevokeConsentAccess::kicked(d, independent), "a separate attachment to the other session survives");
            if (action == Sync::ConsentRevocationKind::SessionGrant)
                check(!RevokeConsentAccess::kicked(d, bystander), "session grant revocation preserves a different principal");
            const auto replacement = RevokeConsentAccess::resumeOn(d, anotherSession, "tok-resume", resume);
            check(RevokeConsentAccess::refusedRevokedAttachment(d, replacement, resume), "the revoked resume is unknown and receives a fresh attachment");
            check(!RevokeConsentAccess::showsSession(d, replacement, theSession), "an unrelated greeting cannot revive the revoked pane");
            check(RevokeConsentAccess::showsSession(d, replacement, anotherSession), "a refused resume may still attach to its authorized target");
        }
    }

    // An active resume may select one session while holding another's pane.
    // Both human and management revocation follow the held authority.
    for (const bool usesManagement : { false, true }) {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        RevokeConsentAccess::addSession(d, anotherSession);
        const auto human = RevokeConsentAccess::addHuman(d, theSession);
        const auto independent = RevokeConsentAccess::addAgent(d, anotherSession, "/usr/bin/agent-resume", "tok-resume");
        const auto lost = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-resume", "tok-resume");
        const auto resume = RevokeConsentAccess::retainedResume(d, lost);
        const auto identity = RevokeConsentAccess::grantKey(d, lost);
        Ubiquitty::Host::ScreenPlaneAccess::reap(d, lost);
        const auto replacement = RevokeConsentAccess::resumeOn(d, anotherSession, "tok-resume", resume);
        check(RevokeConsentAccess::continuedAttachment(d, replacement, resume), "premise: the live resume continues the exact retained record");
        check(RevokeConsentAccess::showsSession(d, replacement, theSession), "premise: a live resumed attachment holds the earlier pane");
        if (usesManagement)
            RevokeConsentAccess::revokeSessionGrantFromManagement(d, human, theSession, identity);
        else {
            Control::ConsentRevocation revoke {};
            revoke.action = Sync::ConsentRevocationKind::SessionGrant;
            revoke.sessionID = theSession;
            revoke.identity = identity;
            RevokeConsentAccess::feedRevoke(d, human, revoke);
        }
        check(RevokeConsentAccess::kicked(d, replacement), "revocation disconnects a resumed holder despite its different greeting target");
        check(!Ubiquitty::Host::ScreenPlaneAccess::attachments(d).find(resume.request.attachmentID), "revocation retires the active resumed record");
        check(!RevokeConsentAccess::kicked(d, independent), "scoped revocation preserves the independent other-session attachment");
    }

    {
        Daemon d;
        RevokeConsentAccess::useScratchStore(d, scratchDirectory);
        RevokeConsentAccess::addSession(d, theSession);
        const auto human = RevokeConsentAccess::addHuman(d, theSession);
        const auto pending = RevokeConsentAccess::addHeldAgent(d, theSession);
        RevokeConsentAccess::approveHeldAgent(d, human, pending);
        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::DisconnectAll;
        revoke.sessionID = theSession;
        RevokeConsentAccess::feedRevoke(d, human, revoke);
        check(RevokeConsentAccess::kicked(d, pending), "disconnect all includes an approved attach still awaiting delivery");
        check(!RevokeConsentAccess::isAdmittedAfterService(d, pending), "an approval held before disconnect all cannot attach afterwards");
    }

    // (h) A truncated body: the decode refuses it and the daemon acts on
    // nothing, from a sender holding every grant so the refusal is the
    // decoder's rather than the gate's.
    {
        Control::ConsentRevocation revoke {};
        revoke.action = Sync::ConsentRevocationKind::DisconnectAll;
        revoke.sessionID = theSession;
        const auto call = Control::consentRevocationRequest(revoke, "fixture.truncated");
        assert(!call.empty());
        for (std::size_t cut = 0; cut < call.size(); ++cut) {
            Daemon d;
            RevokeConsentAccess::useScratchStore(d, scratchDirectory);
            RevokeConsentAccess::addSession(d, theSession);
            const std::size_t human = RevokeConsentAccess::addHuman(d, theSession);
            const std::size_t agent = RevokeConsentAccess::addAgent(d, theSession, "/usr/bin/agent-n", "tok-n");
            RevokeConsentAccess::feedCall(d, human, call.substr(0, cut));
            check(!RevokeConsentAccess::kicked(d, agent), "a truncated revoke kicks nobody");
        }
    }

    // Leave nothing behind: the store files the daemon wrote, then the
    // directory itself.
    ::unlink((scratchDirectory + "/consent").c_str());
    ::unlink((scratchDirectory + "/capability_grants").c_str());
    ::unlink((scratchDirectory + "/consent.lock").c_str());
    ::unlink((scratchDirectory + "/capability_grants.lock").c_str());
    ::rmdir(scratchDirectory.c_str());

    if (failures == 0) {
        std::printf("revoke_consent_actions: four revoke actions, each doing only its own work; "
                    "the sender must be the session's own presence-aware human; "
                    "stable selectors must still match a live agent or exact session grant\n");
        return 0;
    }
    std::fprintf(stderr, "revoke_consent_actions: %d failure(s)\n", failures);
    return 1;
}
