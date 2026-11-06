// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/consent_remote_effects.cpp — a remote program's clipboard writes
// and notifications reach the phone that runs it, once the phone said yes.
//
// The host has no pasteboard and no notification centre, so an OSC 52 write
// or an OSC 9 banner the capability gate admits is the client's to perform —
// and one client's: the session's primary human (Docs/sync.md §6.11), never
// an agent or a viewer. This drives the whole of it through the daemon's own
// code: servicePTY parses the program's output, the policy asks the phone,
// a consent.respond Call takes the phone's "Always", and the original and later writes
// enter the typed effect log with the phone's immutable recipient identity.
// Endpoint output is decoded as complete canonical frames, including typed
// effect recipient identities and image placement records.
//
// The grant lifetime is part of the subject. A remote owner's clipboard write
// is asked about by naming the owner's device, and its "Always" is kept; any
// other host-escaping capability is still forced to a single use, and the
// prompt says so through the origin the request carries.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"
#include "Platform/Host/TokenStore.h"

#include "Assertions.h"
#include "Core/Protocol/Sync/SyncLink.h"
#include "NotifyRead.h"
#include "Platform/StreamDouble.h"
#include "ScopedNothrowAllocationRefusal.h"
#include "ScreenPlaneAccess.h"
#include "TrustedLocalPeer.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Local/Clipboard.h"
#include "Core/Protocol/Sync/EffectDelivery.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Security/CapabilityConsentText.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Sound.h"
#include "PAL/Capability.h"
#include "PAL/Socket.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace Ubiquitty {
namespace Host {

struct ConsentRemoteEffectsAccess {
    // A session wired the way createSession wires one, the effect clients
    // included.
    static Daemon::Session& addSession(Daemon& d, std::uint32_t id, TestDoubles::StreamDouble** streamOut)
    {
        auto session = std::make_unique<Daemon::Session>(d.m_capabilityDecisions);
        session->id = id;
        session->term = std::make_unique<Terminal>();
        resize(*session->term, 24, 80);
        session->capabilityPolicy.setGrantStore(&d.m_grantStore);
        session->capabilityPolicy.setConsentQueue(&d.m_consentQueue);
        session->term->capabilityPolicy = &session->capabilityPolicy;
        Daemon::installTerminalClients(*session);
        auto stream = std::make_unique<TestDoubles::StreamDouble>();
        *streamOut = stream.get();
        session->stream = std::move(stream);
        session->screenSource = std::make_unique<Protocol::Sync::PaneSource>(*session->term, Protocol::Sync::ID128 { .low = id, .high = 9 });
        session->screenSource->publish(Protocol::Sync::FrameClass::Settled, 1);
        Daemon::Session& ref = *session;
        d.m_sessions.push_back(std::move(session));
        return ref;
    }

    // Models only the verified interactive-bootstrap return boundary, not
    // token minting or TLS. No admission/subscription is seeded; the one Greet
    // below must still attach this exact existing session and caller instance.
    static std::size_t addPhone(Daemon& d, std::uint32_t sessionID, PAL::UnixFileDescriptor& peer,
        Protocol::Sync::ID128 instance, bool servesClipboard = false, // bool-ok: Mirrors Greet's single clipboard-serving fixture choice.
        std::unique_ptr<Protocol::Sync::ClientConnection>* readerOut = nullptr,
        std::optional<Protocol::Sync::ResumeRequest> resume = std::nullopt)
    {
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.hasToken = true;
        c.hasBootstrapToken = true;
        c.isInteractiveUser = true;
        c.tokenID = "owner-phone";
        c.tokenScope = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive };
        attach(c, peer);
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        greet(d, index, sessionID, instance, Protocol::Sync::ClientKind::Human, servesClipboard, std::nullopt, readerOut, {}, resume);
        if (readerOut && *readerOut) {
            // The reader reads what its greeting brought, Welcome and PaneBind,
            // and answers with its View, as a client does on that read: the
            // host captures a pane's frames only once a View shows it.
            std::vector<std::uint8_t> down;
            std::array<std::uint8_t, 4096> buffer;
            for (;;) {
                const auto count = ::read(peer.get(), buffer.data(), buffer.size());
                if (count > 0) {
                    down.insert(down.end(), buffer.begin(), buffer.begin() + count);
                    continue;
                }
                if (count < 0 && errno == EINTR)
                    continue;
                break;
            }
            auto& admitted = d.m_clients[index];
            const auto queued = std::span(admitted.outputBuffer).subspan(admitted.outputConsumed);
            down.insert(down.end(), queued.begin(), queued.end());
            admitted.clearPendingOutput();
            (*readerOut)->receiveStream(down, ScreenPlaneAccess::now());
            const auto up = (*readerOut)->takeStreamOutput();
            if (!up.empty())
                ScreenPlaneAccess::receive(d, index, up);
        }
        return index;
    }

    // A phone that says in its Greet that it shows a person no question, as
    // the editor clients do.
    static std::size_t addSilentPhone(Daemon& d, std::uint32_t sessionID, PAL::UnixFileDescriptor& peer, Protocol::Sync::ID128 instance)
    {
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.hasToken = true;
        c.hasBootstrapToken = true;
        c.isInteractiveUser = true;
        c.tokenID = "owner-phone";
        c.tokenScope = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive };
        attach(c, peer);
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        Protocol::Sync::ClientCapabilities capabilities;
        capabilities.add(Protocol::Sync::ClientCapability::ShowsNoQuestions);
        greet(d, index, sessionID, instance, Protocol::Sync::ClientKind::Human, false, std::nullopt, nullptr, capabilities);
        return index;
    }
    static void depart(Daemon& d, std::size_t index) { d.dropClient(index); }
    static Protocol::Sync::ResumeRequest resumeRequest(const Daemon& d, std::size_t index)
    {
        const auto* attachment = d.m_clients[index].syncConnection->attachment();
        CHECK(attachment && !attachment->panes.empty());
        if (!attachment || attachment->panes.empty())
            std::abort();
        const auto& pane = *attachment->panes.front();
        return { .attachmentID = attachment->attachmentID, .resumeSecret = attachment->resumeSecret, .panes = { Protocol::Sync::PaneResume { .paneID = pane.paneID, .effectsAcknowledged = pane.effectsAcknowledged } } };
    }
    static Protocol::Sync::ID128 attachmentID(const Daemon& d, std::size_t index)
    {
        return d.m_clients[index].syncConnection->attachment()->attachmentID;
    }
    static Protocol::Sync::WriteStatus refuseUnprovedInput(Daemon& d, Daemon::Session& session, Protocol::Sync::ID128 attachmentID)
    {
        constexpr std::array<std::uint8_t, 1> input { 'x' };
        return d.writeRemoteInput(session, attachmentID, { 0xbad, 0xbad }, input);
    }
    // A reader too far behind, which the host drops with a farewell.
    static void dropBehind(Daemon& d, std::size_t index) { d.dropForOutputBacklog(index); }

    // A phone whose own policy asks its person about what reaches it, as a
    // client of a host on another machine says in its Greet.
    static std::size_t addAskingPhone(Daemon& d, std::uint32_t sessionID, PAL::UnixFileDescriptor& peer,
        Protocol::Sync::ID128 instance, std::string tokenID = "owner-phone")
    {
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.hasToken = true;
        c.hasBootstrapToken = true;
        c.isInteractiveUser = true;
        c.tokenID = std::move(tokenID);
        c.tokenScope = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive };
        attach(c, peer);
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        Protocol::Sync::ClientCapabilities capabilities;
        capabilities.add(Protocol::Sync::ClientCapability::AsksAboutHostEffects);
        greet(d, index, sessionID, instance, Protocol::Sync::ClientKind::Human, false, std::nullopt, nullptr, capabilities);
        CHECK(d.asksAboutHostEffects(d.m_clients[index]));
        return index;
    }

    static std::size_t addSSHPhone(Daemon& d, std::uint32_t sessionID, PAL::UnixFileDescriptor& peer, Protocol::Sync::ID128 instance)
    {
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.hasOwnerLocalCredentials = true;
        seedTrustedLocalPeer(c);
        c.isRelayedFromSSH = true;
        attach(c, peer);
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        Protocol::Sync::ClientCapabilities capabilities;
        capabilities.add(Protocol::Sync::ClientCapability::AsksAboutHostEffects);
        greet(d, index, sessionID, instance, Protocol::Sync::ClientKind::Human, false, std::nullopt, nullptr, capabilities);
        CHECK(d.originOf(d.m_clients[index]) == CapabilityOrigin::Local);
        CHECK(d.capabilityOriginOf(d.m_clients[index]) == CapabilityOrigin::Remote);
        return index;
    }

    static std::size_t addTokenlessAgent(Daemon& d, PAL::UnixFileDescriptor& peer)
    {
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.hasOwnerLocalCredentials = true;
        c.peerProcess = "/usr/bin/true";
        c.agentLabel = "tool";
        attach(c, peer);
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    // The configuration the next session is created under.
    static void configure(Daemon& d, const CapabilityDecisionTable& decisions) { d.m_capabilityDecisions = decisions; }
    // Records the session's banners for other devices, as createSession does.
    static void recordBanners(Daemon& d, Daemon::Session& s) { d.wireBackgroundNotifications(s); }
    // The banners recorded for other devices and background refresh to post.
    static std::size_t recordedBanners(Daemon& d) { return d.m_backgroundEvents.since(0, d.m_wallClockMilliseconds()).events.size(); }
    static std::size_t pendingQuestions(const Daemon& d) { return d.m_pendingCapabilityGrants.size(); }
    static void expireQuestions(Daemon& d, std::uint64_t now) { d.expireCapabilityQuestions(now); }

    // An automation client on the same session, with drive access.
    static std::size_t addAgent(Daemon& d, std::uint32_t sessionID, PAL::UnixFileDescriptor& peer)
    {
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        attach(c, peer);
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        constexpr std::string_view credential = "effects-fixture-agent-credential-123";
        const auto digest = Crypto::sha256Hex(std::string(credential));
        d.m_tokenPath = d.m_socketPath + ".tokens";
        FILE* store = openOwnerOnly(d.m_tokenPath, "w");
        if (!store)
            std::abort();
        const Protocol::Sync::ScopeSet scope { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive };
        std::fprintf(store, "%s %u 0 effects-agent\n", digest.c_str(), static_cast<unsigned>(scope.toRaw()));
        std::fclose(store);
        // Independent prior owner consent for this exact stored-token principal.
        // Neither the candidate's Greet nor its eventual Welcome creates it.
        auto* session = d.findSession(sessionID);
        if (!session)
            std::abort();
        (void)session->addSessionGrant("token:" + digest);
        greet(d, index, sessionID, { 0xA6E17, d.m_clients[index].clientID }, Protocol::Sync::ClientKind::Agent, false,
            std::vector<std::uint8_t>(credential.begin(), credential.end()));
        CHECK(d.m_clients[index].hasToken && d.m_clients[index].tokenID == digest && d.isAgentClient(d.m_clients[index]));
        return index;
    }

    static void attach(Daemon::Client& c, PAL::UnixFileDescriptor& peer)
    {
        // Consent may deliver an effect immediately, so this client must own
        // a writable transport rather than a sentinel descriptor.
        int descriptors[2] { -1, -1 };
        const int result = ::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors);
        CHECK(result == 0);
        if (result != 0)
            return;
        c.fileDescriptor.reset(descriptors[0]);
        peer.reset(descriptors[1]);
        const int senderFlags = ::fcntl(c.fileDescriptor.get(), F_SETFL, O_NONBLOCK);
        const int peerFlags = ::fcntl(peer.get(), F_SETFL, O_NONBLOCK);
        CHECK(senderFlags == 0);
        CHECK(peerFlags == 0);
        if (senderFlags != 0 || peerFlags != 0) {
            c.fileDescriptor.reset();
            peer.reset();
            return;
        }
    }

    static void greet(Daemon& d, std::size_t index, std::uint32_t sessionID, Protocol::Sync::ID128 instance,
        Protocol::Sync::ClientKind kind, bool servesClipboard, std::optional<std::vector<std::uint8_t>> credential = std::nullopt,
        std::unique_ptr<Protocol::Sync::ClientConnection>* readerOut = nullptr, Protocol::Sync::ClientCapabilities capabilities = {},
        std::optional<Protocol::Sync::ResumeRequest> resume = std::nullopt)
    {
        using namespace Protocol::Sync;
        auto* selected = d.findSession(sessionID);
        if (!selected)
            std::abort();
        const auto expectedWorkspace = d.hostedWorkspaceFor(*selected).workspaceID;
        Greet greeting;
        greeting.wireVersion = wireVersion;
        greeting.appVersion = "effects-fixture";
        greeting.clientInstance = instance;
        greeting.clientKind = kind;
        greeting.requestedRole = kind == ClientKind::Agent ? Role::Agent : Role::Driver;
        greeting.request = { .target = AdmissionTarget::Session, .sessionID = sessionID, .expectedDaemonInstanceID = d.m_daemonInstanceID, .agentLabel = kind == ClientKind::Agent ? "build-bot" : "", .isAttachOnly = true, .servesClipboard = servesClipboard };
        greeting.credential = std::move(credential);
        greeting.resume = std::move(resume);
        greeting.facts = UbiquittyTest::syncDefaultFacts();
        greeting.capabilities = capabilities;
        std::vector<std::uint8_t> body, wire;
        if (readerOut) {
            ClientIdentity identity;
            identity.clientInstance = instance;
            identity.kind = kind;
            identity.requestedRole = greeting.requestedRole;
            identity.facts = greeting.facts;
            identity.request = greeting.request;
            identity.credential = greeting.credential;
            *readerOut = std::make_unique<ClientConnection>(UbiquittyTest::syncClientPolicy, std::move(identity));
            (*readerOut)->start();
            wire = (*readerOut)->takeStreamOutput();
        } else {
            encode(body, greeting);
            if (body.empty() || !decodeGreet(body) || !appendFrame(wire, FrameKind::Greet, 0, body))
                std::abort();
        }
        ScreenPlaneAccess::receive(d, index, wire);
        d.serviceSyncConnection(index, ScreenPlaneAccess::now());
        auto& client = d.m_clients[index];
        if (!client.syncConnection || client.syncConnection->state() != ConnectionState::Attached || !client.subscriptionFor(sessionID))
            std::abort();
        CHECK(client.sessionID == sessionID && client.workspaceID == expectedWorkspace);
        CHECK(client.syncConnection->clientInstance() == instance && d.hasLiveStructureAttachment(client));
        CHECK(client.syncConnection->attachment()->servingConnection == client.clientID);
        const auto* subscription = client.subscriptionFor(sessionID);
        if (!subscription->screenHandle)
            std::abort();
        if (readerOut)
            return; // Its retained real reader sends the View when it reads PaneBind.
        body.clear();
        wire.clear();
        encode(body, View { .state = kind == ClientKind::Agent ? ViewState::RecordsOnly : ViewState::Live });
        CHECK(appendFrame(wire, FrameKind::View, subscription->screenHandle, body));
        ScreenPlaneAccess::receive(d, index, wire);
    }

    // What `stampInputOrigin` does when this client types.
    static void typed(Daemon& d, Daemon::Session& s, std::size_t index)
    {
        d.stampInputOrigin(s, d.m_clients[index]);
    }

    static void setSocketPath(Daemon& d, const std::string& path) { d.m_socketPath = path; }
    static void service(Daemon& d, Daemon::Session& s) { d.servicePTY(s); }
    static bool isLive(const Daemon& d, std::size_t index) { return !d.m_clients[index].didOverflow; }
    /// Reads the exact handle and serving intent production admission bound.
    /// No test binder or clipboard-admission callback replaces that authority.
    ///
    /// - Returns: The admitted session handle.
    static std::uint32_t installClipboardReader(Daemon& daemon, Daemon::Session& session, std::size_t index)
    {
        auto& client = daemon.m_clients[index];
        CHECK(client.intents.contains(Protocol::Sync::HelloIntent::ServesClipboard));
        const auto* subscription = client.subscriptionFor(session.id);
        return subscription ? subscription->screenHandle : 0;
    }

    // A fresh local helper, not another admitted screen/Agent connection.
    // Only process-tree membership is modeled by the session's StreamDouble.
    static std::size_t addHelper(Daemon& d, PAL::UnixFileDescriptor& peer, TestDoubles::StreamDouble& stream)
    {
        Daemon::Client client;
        client.clientID = d.m_nextClientID++;
        attach(client, peer);
        client.hasOwnerLocalCredentials = PAL::peerIsSameUser(client.fileDescriptor);
        d.capturePeerIdentity(client);
        if (!client.hasOwnerLocalCredentials || client.peerProcessID != static_cast<std::uint32_t>(::getpid()))
            std::abort();
        stream.facts.processes.push_back({ .processID = client.peerProcessID });
        d.m_clients.push_back(std::move(client));
        return d.m_clients.size() - 1;
    }

    /// Starts a request after the local helper's process proof has been admitted.
    ///
    /// - Returns: The remote correlation ID, distinct from the local helper ID.
    static std::uint32_t requestClipboard(Daemon& daemon, std::size_t helper, std::uint32_t sessionID, std::uint32_t localID)
    {
        const auto request = Protocol::Local::encodeLocalClipboardRequest(localID,
            { .kind = Protocol::Local::LocalClipboardKind::Item, .mime = "image/png" });
        if (!request)
            std::abort();
        ScreenPlaneAccess::receive(daemon, helper, request->view());
        CHECK(daemon.m_clients[helper].clipboardToolSessionID == sessionID);
        CHECK(daemon.m_clients[helper].localClipboardCorrelation == localID);
        return daemon.m_clients[helper].clipboardToolRequestID;
    }

    /// Admits only the compact refusal, not the incoming successful payload.
    static void limitClipboardOutput(Daemon& daemon, std::size_t helper)
    {
        daemon.m_clients[helper].imageSourceBudget = std::make_shared<Protocol::Sync::BulkSourceBudget>(Protocol::Local::localClipboardReplyHeaderBytes);
    }

    /// Delivers exact canonical bytes through the normal daemon dispatch boundary.
    static void screenBytes(Daemon& daemon, std::size_t index, std::span<const std::uint8_t> bytes) { daemon.receiveScreenBytes(index, bytes); }
    /// Expires a participating endpoint without running the periodic expiry sweep.
    static void expireToken(Daemon& daemon, std::size_t index)
    {
        // Fault injection after admission/selection, never a token authority grant.
        daemon.m_clients[index].hasToken = true;
        daemon.m_clients[index].tokenExpiryUnix = 1;
    }
    /// Expires the original helper operation without running its timeout sweep.
    static void expireRequest(Daemon& daemon, std::size_t index) { daemon.m_clients[index].clipboardToolDeadlineMilliseconds = 1; }
    /// Returns retained remote reply bytes, excluding the framing decoder.
    static std::size_t clipboardBytes(const Daemon& daemon, std::size_t index)
    {
        const auto* connection = daemon.m_clients[index].syncConnection.get();
        return connection ? connection->inboundClipboardBytes() : 0;
    }
    /// Returns whether this reader's canonical connection is still admitted.
    static bool isAttached(const Daemon& daemon, std::size_t index)
    {
        const auto* connection = daemon.m_clients[index].syncConnection.get();
        return connection && connection->state() == Protocol::Sync::ConnectionState::Attached;
    }
    /// Returns the outstanding physical reader operation count.
    static std::size_t pendingReads(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].pendingClipboardRequests.size(); }
    /// Expires requests through the ordinary helper lifecycle.
    static void expireRequests(Daemon& daemon) { daemon.expireClipboardTools(); }

    /// Retries an already-admitted queued notification at the daemon boundary.
    static void deliver(Daemon& daemon, Daemon::Session& session) { daemon.deliverSessionEffects(session); }

    /// Retries captured parser-time sounds at the live producer boundary.
    static void sounds(Daemon& daemon, Daemon::Session& session) { daemon.emitPendingSounds(session); }
    /// Publishes parsed program output using the daemon's ordinary effect producer.
    static void publish(Daemon& daemon, Daemon::Session& session) { daemon.publishFrame(session, Runtime::FrameClass::Settled); }
    /// Frees retry capacity only through this endpoint's real complete-batch ACK.
    /// No sent/acknowledged reader cursor or admission field is manufactured.
    static void acknowledgeEffects(Daemon& d, Daemon::Session& session, std::size_t index, const PAL::UnixFileDescriptor& peer,
        Protocol::Sync::ClientConnection& reader)
    {
        auto& log = session.screenSource->effects();
        const auto through = log.newestSequence();
        std::uint64_t at = ScreenPlaneAccess::now();
        for (std::size_t round = 0; round < 1024 && log.retiredThrough() < through; ++round) {
            d.serviceSyncConnection(index, ScreenPlaneAccess::now());
            std::vector<std::uint8_t> down;
            std::array<std::uint8_t, 4096> buffer;
            for (;;) {
                const auto count = ::read(peer.get(), buffer.data(), buffer.size());
                if (count > 0) {
                    down.insert(down.end(), buffer.begin(), buffer.begin() + count);
                    continue;
                }
                if (count < 0 && errno == EINTR)
                    continue;
                if (count >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                    std::abort();
                break;
            }
            auto& client = d.m_clients[index];
            const auto queued = std::span(client.outputBuffer).subspan(client.outputConsumed);
            down.insert(down.end(), queued.begin(), queued.end());
            client.clearPendingOutput();
            reader.receiveStream(down, at);
            if (reader.state() == Protocol::Sync::ConnectionState::Closed)
                std::abort();
            if (const auto due = reader.nextTickMicroseconds())
                at = std::max(at, *due);
            reader.tick(at);
            const auto up = reader.takeStreamOutput();
            if (!up.empty())
                ScreenPlaneAccess::receive(d, index, up);
            log.trim(at);
            ++at;
        }
        CHECK(log.retiredThrough() == through);
    }
    /// Publishes image state on the existing admitted pane, without reconnecting.
    /// Serves a client's connection, as the loop does on every pass.
    static void serve(Daemon& daemon, std::size_t index) { daemon.serviceSyncConnection(index, ScreenPlaneAccess::now()); }
    static void images(Daemon& daemon, Daemon::Session& session, std::size_t index)
    {
        CHECK(daemon.m_clients[index].syncConnection);
        daemon.seedPaneState(session);
        daemon.publishFrame(session, Runtime::FrameClass::Settled);
    }
    static std::string grantKey(const Daemon& d, std::size_t index) { return d.grantKeyFor(d.m_clients[index]); }
    static std::optional<std::string> firstInputPrincipal(const Daemon::Session& session) { return session.firstInputPrincipal; }
    static CapabilityOrigin inputOrigin(const Daemon::Session& session) { return session.term->currentCapabilityOrigin; }

    static void answer(Daemon& d, std::size_t index, std::uint32_t requestID, Protocol::Sync::ConsentChoice decision)
    {
        const auto call = Protocol::Control::consentResponseRequest(
            { requestID, Protocol::Sync::ConsentKind::Capability, decision }, "fixture.answer");
        CHECK(!call.empty());
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Call { call });
        std::vector<std::uint8_t> stream;
        CHECK(Protocol::Sync::appendFrame(stream, Protocol::Sync::FrameKind::Call, 0, body));
        d.receiveScreenBytes(index, stream);
        d.serviceSyncConnection(index, ScreenPlaneAccess::now());
    }

    static void injectPending(Daemon& d, std::uint32_t requestID, Capability capability, CapabilityOrigin origin, const std::string& identity, std::uint32_t sessionID)
    {
        auto& pending = d.m_pendingCapabilityGrants[requestID];
        pending.capability = capability;
        pending.origin = origin;
        pending.identity = identity;
        pending.sessionID = sessionID;
        pending.deadlineMilliseconds = std::numeric_limits<std::uint64_t>::max(); // asked, and not yet due
        for (const auto& candidate : d.m_clients)
            if (d.mayRuleOnConsentFor(candidate, sessionID))
                pending.recipients.push_back({ candidate.clientID, sessionID });
    }

    static bool isGranted(const Daemon& d, Capability capability, CapabilityOrigin origin, const std::string& identity) { return d.m_grantStore.isGranted(capability, origin, identity); }

    // What the daemon would answer the next check with, without consuming a
    // OneShot: a remembered grant answers twice.
    static bool isRemembered(const Daemon& d, Capability capability, CapabilityOrigin origin, const std::string& identity)
    {
        for (const auto& [key, record] : d.m_grantStore.records())
            if (key.capability == capability && key.origin == origin && key.identity == identity)
                return record.scope != CapabilityGrantScope::OneShot;
        return false;
    }

    // Every frame sent or queued to a client since the last call, decoded by type.
    struct Received {
        std::vector<Protocol::Sync::ConsentQuestion> consentRequests;
        unsigned clipboardWrites = 0;
        unsigned notifications = 0;
        std::vector<Protocol::Sync::ClipboardDataMessage> clipboardData;
        unsigned imageFrames = 0;
        std::vector<Protocol::Sync::ImagePlacements> placements;
        bool isCanonical = true;
    };
    /// Takes what the daemon sent a client and parses it. A client with a
    /// reader reads it too, and its acknowledgements go back to the daemon,
    /// as a client's do on every read.
    static Received take(Daemon& d, std::size_t index, const PAL::UnixFileDescriptor& peer,
        Protocol::Sync::ClientConnection* reader = nullptr)
    {
        Received received;
        d.drainSyncConnection(index, ScreenPlaneAccess::now());
        auto& client = d.m_clients[index];
        CHECK(!client.didOverflow);
        std::vector<std::uint8_t> collected;
        std::array<std::byte, 4096> bytes;
        for (;;) {
            const ssize_t count = ::read(peer.get(), bytes.data(), bytes.size());
            if (count > 0) {
                const auto input = std::span(bytes).first(static_cast<std::size_t>(count));
                const auto raw = std::span(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
                collected.insert(collected.end(), raw.begin(), raw.end());
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            CHECK(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
            break;
        }
        // A flush may have stopped at a short write. Its unsent suffix follows
        // the peer bytes; never parse the already-consumed prefix twice.
        const auto pending = std::span(client.outputBuffer).subspan(client.outputConsumed);
        collected.insert(collected.end(), pending.begin(), pending.end());
        if (client.localClipboardCorrelation) {
            if (!collected.empty()) {
                Protocol::Local::LocalClipboardReplyReader reader(client.localClipboardCorrelation);
                CHECK(reader.consume(collected) == Protocol::Local::LocalClipboardRequestReader::State::Ready);
                if (const auto reply = reader.take()) {
                    CHECK(reply->daemonInstanceID == d.m_daemonInstanceID);
                    CHECK(!reply->isHostSelected);
                    std::vector<Protocol::Sync::ClipboardServeItem> items;
                    for (const auto& item : std::span(reply->bundle.items).first(reply->bundle.count))
                        items.push_back({ std::string(item.mime), { item.data.begin(), item.data.end() } });
                    received.clipboardData.push_back(Protocol::Sync::makeClipboardData(static_cast<std::uint32_t>(reply->correlation),
                        static_cast<Protocol::Sync::ClipboardServeStatus>(reply->status), std::move(items)));
                }
            }
            client.clearPendingOutput();
            return received;
        }
        using namespace Protocol::Sync;
        FrameDecoder decoder;
        decoder.append(collected);
        while (const auto frame = decoder.next()) {
            CHECK(directionOf(frame->kind) != Direction::ClientToHost);
            if (frame->kind == FrameKind::Welcome) {
                const auto welcome = decodeWelcome(frame->body);
                CHECK(welcome && welcome->admission == AttachmentAdmission::Granted && welcome->selection.target == AdmissionTarget::Session);
                CHECK(welcome && welcome->selection.sessionID == client.sessionID && welcome->selection.workspaceID == client.workspaceID
                    && welcome->selection.daemonInstanceID == d.m_daemonInstanceID && welcome->selection.socketName == d.socketBasename());
            } else if (frame->kind == FrameKind::Notify) {
                const auto notify = UbiquittyTest::notifyOf(frame->body);
                CHECK(notify);
                if (notify)
                    if (auto request = UbiquittyTest::consentQuestionOf(*notify))
                        received.consentRequests.push_back(*request);
            } else if (frame->kind == FrameKind::Effect) {
                const auto bindings = client.syncConnection->boundPanes();
                const auto pane = std::ranges::find(bindings, frame->channel, &std::pair<std::uint32_t, ID128>::first);
                if (pane == bindings.end()) {
                    CHECK(false);
                    continue;
                }
                const auto effect = decodeEffectPacket(pane->second, frame->body);
                CHECK(effect);
                if (effect && effect->record)
                    CHECK(decodeEffectBody(effect->record->kind, effect->record->payload).has_value());
                if (effect && effect->record && effect->record->primary == client.syncConnection->clientInstance()) {
                    received.clipboardWrites += effect->record->kind == EffectKind::ClipboardWrite;
                    received.notifications += effect->record->kind == EffectKind::Notification;
                }
            } else if (frame->kind == FrameKind::ImagePlacements) {
                const auto placements = decodeImagePlacements(frame->body);
                CHECK(placements);
                if (placements) {
                    ++received.imageFrames;
                    received.placements.push_back(*placements);
                }
            }
        }
        received.isCanonical = decoder.error() == EnvelopeError::None && decoder.bufferedBytes() == 0;
        CHECK(received.isCanonical);
        client.clearPendingOutput();
        if (reader) {
            reader->receiveStream(collected, ScreenPlaneAccess::now());
            const auto up = reader->takeStreamOutput();
            if (!up.empty())
                ScreenPlaneAccess::receive(d, index, up);
        }
        return received;
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Capability;
using Ubiquitty::CapabilityOrigin;
using Ubiquitty::Host::Daemon;
using Ubiquitty::TestDoubles::StreamDouble;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

using A = Ubiquitty::Host::ConsentRemoteEffectsAccess;

std::string textOf(const Ubiquitty::Protocol::Sync::EffectView& write)
{
    using namespace Ubiquitty::Protocol::Sync;
    const auto bundle = decodeEffectClipboard(write.data, maxEffectClipboardWriteBytes);
    if (!bundle || bundle->count != 1 || bundle->items[0].mime != "text/plain")
        return {};
    const auto bytes = bundle->items[0].data;
    return std::string(bytes.begin(), bytes.end());
}

/// Allows every effect, so a write an agent's principal drives reaches the
/// clipboard sink and the delivery rule is the only thing deciding where it goes.
///
/// A terminal with no policy answers PAL's Agent default, which prompts for a
/// clipboard write and, with nothing installed to ask, refuses it.
class AllowingPolicy final : public Ubiquitty::CapabilityPolicy {
public:
    /// Allows the request.
    ///
    /// - Returns: Allow, whatever is asked.
    Ubiquitty::CapabilityDecision check(Capability, CapabilityOrigin, std::string_view) override { return Ubiquitty::CapabilityDecision::Allow; }
};

} // namespace

int main()
{
    char directoryTemplate[] = "/tmp/ubiquitty-remote-effects-XXXXXX";
    const char* directory = ::mkdtemp(directoryTemplate);
    CHECK(directory != nullptr);
    if (!directory)
        return 1;
    const std::string grantsFile = std::string(directory) + "/capability_grants";

    // ---- 1. The phone is asked once, about its own device, with every
    // lifetime on offer; says Always; and then both copies and the banner reach
    // the phone and nothing reaches the agent.
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor phonePeer, agentPeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const std::size_t phone = A::addPhone(d, 1, phonePeer, { 51, 52 });
        const std::size_t agent = A::addAgent(d, 1, agentPeer);
        const ID128 recipient { 51, 52 };
        A::typed(d, s, phone);

        stream->pushInbound("\033]52;c;Zmlyc3Q=\a"); // "first"
        A::service(d, s);
        const A::Received asked = A::take(d, phone, phonePeer);
        CHECK_MSG(asked.clipboardWrites == 0 && asked.isCanonical, "a copy reached the phone before the phone allowed it");
        CHECK(s.screenSource->effects().newestSequence() == 0);
        CHECK(asked.consentRequests.size() == 1);
        CHECK(A::take(d, agent, agentPeer).consentRequests.empty());
        if (asked.consentRequests.size() == 1) {
            const Sync::ConsentQuestion& request = asked.consentRequests[0];
            CHECK(request.capabilityCode == static_cast<std::uint16_t>(std::to_underlying(Capability::ClipboardWrite)));
            CHECK(request.capabilityOrigin == std::to_underlying(CapabilityOrigin::Remote));
            CHECK_MSG(request.isRecipientsOwnRequest, "the phone was not told the request is its own");
            CHECK_MSG(!Ubiquitty::capabilityGrantIsPerUse(Capability::ClipboardWrite, Ubiquitty::Security::capabilityOriginOf(request.capabilityOrigin)),
                "the phone would be offered only Allow Once for its own clipboard write");
            A::answer(d, phone, request.requestID, Sync::ConsentChoice::AllowAlways);
        }
        CHECK_MSG(A::isLive(d, phone), "immediate clipboard delivery dropped the recipient");
        CHECK_MSG(A::isRemembered(d, Capability::ClipboardWrite, CapabilityOrigin::Remote, A::grantKey(d, phone)),
            "the phone's Always was cut to a single use");

        // Two copies, the banner's grant, and the banner.
        stream->pushInbound("\033]52;c;c2Vjb25k\a"); // "second"
        A::service(d, s);
        stream->pushInbound("\033]52;c;dGhpcmQ=\a"); // "third"
        A::service(d, s);
        stream->pushInbound("\033]9;build finished\a");
        A::service(d, s);
        const A::Received banner = A::take(d, phone, phonePeer);
        CHECK(banner.consentRequests.size() == 1);
        if (banner.consentRequests.size() == 1) {
            CHECK(banner.consentRequests[0].capabilityCode == static_cast<std::uint16_t>(std::to_underlying(Capability::DesktopNotification)));
            A::answer(d, phone, banner.consentRequests[0].requestID, Sync::ConsentChoice::AllowAlways);
        }
        stream->pushInbound("\033]9;tests passed\a");
        A::service(d, s);
        A::publish(d, s);
        A::deliver(d, s);

        const A::Received toPhone = A::take(d, phone, phonePeer);
        CHECK(banner.isCanonical && toPhone.isCanonical);
        const auto delivered = s.screenSource->effects().readAfter(0, EffectReadMode::Consecutive);
        CHECK_MSG(delivered && delivered->size == 4, "the typed log did not retain the original, both later copies and the banner");
        if (delivered && delivered->size == 4) {
            const std::array<std::string_view, 3> expected { "first", "second", "third" };
            for (std::size_t index = 0; index < expected.size(); ++index) {
                const auto& copy = delivered->records[index];
                CHECK(copy.kind == EffectKind::ClipboardWrite && copy.primary == recipient);
                const auto body = decodeEffectBody(copy.kind, copy.payload);
                CHECK(body && std::holds_alternative<ClipboardWriteEffect>(*body));
                if (body && std::holds_alternative<ClipboardWriteEffect>(*body))
                    CHECK(std::get<ClipboardWriteEffect>(*body).buffer == Ubiquitty::ClipboardBuffer::Clipboard);
                CHECK(textOf(copy) == expected[index]);
            }
            const auto& bannerEffect = delivered->records[3];
            CHECK(bannerEffect.kind == EffectKind::Notification && bannerEffect.primary == recipient);
            const auto body = decodeEffectBody(bannerEffect.kind, bannerEffect.payload);
            CHECK(body && std::holds_alternative<NotificationEffect>(*body));
            if (body && std::holds_alternative<NotificationEffect>(*body))
                CHECK(std::get<NotificationEffect>(*body).body == "tests passed");
        }

        const A::Received toAgent = A::take(d, agent, agentPeer);
        CHECK_MSG(toAgent.clipboardWrites == 0 && toAgent.notifications == 0, "an agent received a person's clipboard write or banner");
    }

    // ---- 2. The agent typed last. The program is still the phone's person's
    // session, and the effects still go to the human, never to the agent.
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor phonePeer, agentPeer;
        AllowingPolicy allowing; // outlives the daemon whose terminal holds it
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const std::size_t phone = A::addPhone(d, 1, phonePeer, { 61, 62 });
        const std::size_t agent = A::addAgent(d, 1, agentPeer);
        const ID128 recipient { 61, 62 };
        A::typed(d, s, phone);
        A::typed(d, s, agent);
        // Drive the gate open for the agent's principal so the delivery rule is
        // the only thing under test.
        s.term->capabilityPolicy = &allowing;

        stream->pushInbound("\033]52;c;Zm91cnRo\a"); // "fourth"
        A::service(d, s);
        A::publish(d, s);
        A::deliver(d, s);
        const auto delivered = s.screenSource->effects().readAfter(0, EffectReadMode::Consecutive);
        CHECK(delivered && delivered->size == 1);
        if (delivered && delivered->size == 1) {
            CHECK(delivered->records[0].kind == EffectKind::ClipboardWrite && delivered->records[0].primary == recipient);
            CHECK(textOf(delivered->records[0]) == "fourth");
        }
        CHECK(A::take(d, phone, phonePeer).isCanonical);
        CHECK(A::take(d, agent, agentPeer).clipboardWrites == 0);
    }

    // ---- 3. Any other host-escaping capability keeps its single use: an
    // ExecuteTrigger the owner configured to prompt and the phone answered
    // Always is recorded once, and its prompt offers only Allow Once.
    {
        Ubiquitty::PAL::UnixFileDescriptor phonePeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& session = A::addSession(d, 1, &stream);
        const std::size_t phone = A::addPhone(d, 1, phonePeer, { 71, 72 });
        A::injectPending(d, 77, Capability::ExecuteTrigger, CapabilityOrigin::Remote, A::grantKey(d, phone), 1);
        A::answer(d, phone, 77, Sync::ConsentChoice::AllowAlways);
        CHECK(A::isGranted(d, Capability::ExecuteTrigger, CapabilityOrigin::Remote, A::grantKey(d, phone)));
        CHECK_MSG(!A::isRemembered(d, Capability::ExecuteTrigger, CapabilityOrigin::Remote, A::grantKey(d, phone)),
            "a remote ExecuteTrigger grant outlived one use");
        CHECK(Ubiquitty::capabilityGrantIsPerUse(Capability::ExecuteTrigger, CapabilityOrigin::Remote));
        // And the agent origin's clipboard write is unchanged: single use.
        CHECK(Ubiquitty::capabilityGrantIsPerUse(Capability::ClipboardWrite, CapabilityOrigin::Agent));
    }

    // The typed producer preserves replacement/timing, waits for the picture,
    // and retains the selected primary even if log capacity forces a retry.
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor phonePeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const auto phone = A::addPhone(d, 1, phonePeer, { 71, 72 });
        CHECK(A::isLive(d, phone));
        const ID128 originalPrimary { 71, 72 };
        s.isFrameDue = true;
        s.shellClient.postNotificationRequest({ .title = "Build", .body = "Finished", .identifier = "build-job", .occasion = Ubiquitty::NotificationOccasion::Unfocused });
        A::deliver(d, s);
        CHECK(!s.effects.isEmpty());
        const auto before = s.screenSource->effects().readAfter(0, EffectReadMode::Consecutive);
        CHECK(before && before->size == 0);
        s.isFrameDue = false;
        A::deliver(d, s);
        CHECK(s.effects.isEmpty());
        const auto after = s.screenSource->effects().readAfter(0, EffectReadMode::Consecutive);
        CHECK(after && after->size == 1);
        if (after && after->size == 1) {
            CHECK(after->records[0].primary == originalPrimary);
            const auto decoded = decodeEffectBody(after->records[0].kind, after->records[0].payload);
            CHECK(decoded && std::holds_alternative<NotificationEffect>(*decoded));
            if (decoded && std::holds_alternative<NotificationEffect>(*decoded)) {
                const auto& notification = std::get<NotificationEffect>(*decoded);
                CHECK(notification.title == "Build" && notification.body == "Finished");
                CHECK(notification.identifier == "build-job" && notification.occasion == Ubiquitty::NotificationOccasion::Unfocused);
            }
        }
        // A rejected append leaves the held question's identity in the queue.
        auto& log = s.screenSource->effects();
        while (s.screenSource->appendEffect(BellEffect {}, originalPrimary)) {
        }
        s.shellClient.postNotificationRequest({ .title = "Next", .body = "Wait", .identifier = "next-job", .occasion = Ubiquitty::NotificationOccasion::Invisible });
        A::deliver(d, s);
        auto held = s.effects.take();
        CHECK(held.size() == 1);
        if (held.size() == 1) {
            const auto* notification = std::get_if<Ubiquitty::Host::HostNotification>(&held.front());
            CHECK(notification && notification->primary == originalPrimary);
            CHECK(notification && notification->identifier == "next-job");
        }
        CHECK(log.readAfter(0, EffectReadMode::Consecutive).has_value());
    }

    // A real admitted recipient's picture barrier holds the selected identity.
    // A newly admitted physical recipient cannot steal an already captured effect.
    for (const bool shouldChangeRecipient : { false, true }) {
        using namespace Ubiquitty;
        using namespace Ubiquitty::Protocol::Sync;
        PAL::UnixFileDescriptor phonePeer, neighborPeer;
        Daemon daemon;
        A::setSocketPath(daemon, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& session = A::addSession(daemon, 1, &stream);
        const ID128 recipient { 121, 122 };
        auto phone = A::addPhone(daemon, 1, phonePeer, recipient);
        A::typed(daemon, session, phone);
        session.isFrameDue = true;
        const std::array<ClipboardItem, 1> items { ClipboardItem { "text/plain", std::string_view("held before greet") } };
        session.clipboardClient.writeClipboardItems(ClipboardBuffer::Clipboard, items);
        session.shellClient.postNotificationRequest({ .title = "Before greet", .body = "Held", .identifier = "queued", .occasion = NotificationOccasion::Always });
        A::deliver(daemon, session);
        CHECK(!session.effects.isEmpty());
        CHECK(A::take(daemon, phone, phonePeer).isCanonical);
        if (shouldChangeRecipient) {
            phone = A::addPhone(daemon, 1, neighborPeer, { 123, 124 });
            A::typed(daemon, session, phone);
        }
        session.isFrameDue = false;
        A::deliver(daemon, session);
        const auto batch = session.screenSource->effects().readAfter(0, EffectReadMode::Consecutive);
        CHECK(batch && batch->size == 2);
        if (batch && batch->size == 2) {
            CHECK(batch->records[0].kind == EffectKind::ClipboardWrite && batch->records[0].primary == recipient);
            CHECK(textOf(batch->records[0]) == "held before greet");
            CHECK(batch->records[1].kind == EffectKind::Notification && batch->records[1].primary == recipient);
        }
        CHECK(session.effects.isEmpty());
        const auto toCurrent = A::take(daemon, phone, shouldChangeRecipient ? neighborPeer : phonePeer);
        CHECK(toCurrent.isCanonical);
        if (shouldChangeRecipient)
            CHECK(toCurrent.clipboardWrites == 0 && toCurrent.notifications == 0);
    }

    // Typed image admission must not also serialize the old complete image set
    // on attach or publication; only the bounded typed image owners are charged.
    {
        Ubiquitty::PAL::UnixFileDescriptor phonePeer;
        Daemon daemon;
        A::setSocketPath(daemon, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& session = A::addSession(daemon, 1, &stream);
        // A client that acknowledges what it reads: the image owners follow
        // the checkpoint the client has taken.
        std::unique_ptr<Ubiquitty::Protocol::Sync::ClientConnection> reader;
        const auto phone = A::addPhone(daemon, 1, phonePeer, { 101, 102 }, false, &reader);
        Ubiquitty::processBytes(session.term.get(), "\033_Gf=24,s=1,v=1,a=T;/wAA\033\\");
        CHECK(std::ranges::any_of(session.term->images.stored, [](const auto& image) { return image.used && image.width == 1 && image.height == 1; }));
        A::images(daemon, session, phone);
        A::Received images;
        for (int round = 0; round < 64 && images.isCanonical && images.imageFrames == 0; ++round) {
            A::serve(daemon, phone);
            auto taken = A::take(daemon, phone, phonePeer, reader.get());
            images.isCanonical = taken.isCanonical;
            images.imageFrames += taken.imageFrames;
            images.placements.insert(images.placements.end(), taken.placements.begin(), taken.placements.end());
        }
        CHECK(images.isCanonical && images.imageFrames > 0);
        CHECK(std::ranges::any_of(images.placements, [](const auto& state) {
            return state.imageCount == 1 && state.images[0].asset.width == 1 && state.images[0].asset.height == 1
                && state.placementCount == 1 && state.placements[0].hash == state.images[0].asset.hash;
        }));
    }

    // Typed clipboard ownership preserves all representations and the selected
    // recipient while its picture is waiting to publish.
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor phonePeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const auto phone = A::addPhone(d, 1, phonePeer, { 91, 92 });
        CHECK(A::isLive(d, phone));
        const ID128 originalPrimary { 91, 92 };
        const std::array<Ubiquitty::ClipboardItem, 2> items { Ubiquitty::ClipboardItem { "text/plain", "exact text" },
            Ubiquitty::ClipboardItem { "text/html", "<b>exact text</b>" } };
        s.isFrameDue = true;
        s.clipboardClient.writeClipboardItems(Ubiquitty::ClipboardBuffer::Clipboard, items);
        A::deliver(d, s);
        auto pending = s.effects.take();
        CHECK(pending.size() == 1);
        if (pending.size() == 1) {
            const auto* write = std::get_if<Ubiquitty::Host::HostEffectQueue::ClipboardWrite>(&pending.front());
            CHECK(write && write->primary == originalPrimary);
            s.effects.push(std::move(pending.front()));
        }
        Ubiquitty::PAL::UnixFileDescriptor replacementPeer;
        const auto replacement = A::addPhone(d, 1, replacementPeer, { 93, 94 });
        A::typed(d, s, replacement);
        s.isFrameDue = false;
        A::deliver(d, s);
        CHECK(s.effects.isEmpty());
        const auto batch = s.screenSource->effects().readAfter(0, EffectReadMode::Consecutive);
        CHECK(batch && batch->size == 1);
        if (batch && batch->size == 1) {
            CHECK(batch->records[0].primary == originalPrimary);
            const auto decoded = decodeEffectClipboard(batch->records[0].data, maxEffectClipboardWriteBytes);
            CHECK(decoded && decoded->count == 2);
            if (decoded && decoded->count == 2) {
                CHECK(decoded->items[0].mime == "text/plain");
                CHECK(decoded->items[1].mime == "text/html");
                CHECK(decoded->items[1].data.size() == items[1].bytes.size());
            }
        }
        const auto toReplacement = A::take(d, replacement, replacementPeer);
        CHECK(toReplacement.isCanonical && toReplacement.clipboardWrites == 0);
    }

    // Frame-owned effects preserve each command completion and do not consume
    // their pending state when the bounded log refuses an append.
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor phonePeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        std::unique_ptr<ClientConnection> reader;
        const auto phone = A::addPhone(d, 1, phonePeer, { 81, 82 }, false, &reader);
        const ID128 primary { 81, 82 };
        stream->pushInbound("\a\033]133;D;7\a\033]133;D;0\a");
        A::service(d, s);
        s.term->currentDirectory = "/work/tree";
        s.term->commandLine = "build --check";
        ++s.term->commandLineGeneration;
        s.term->commandDurationTicksRing[0] = 3;
        s.term->commandDurationTicksRing[1] = 60;
        A::publish(d, s);
        auto batch = s.screenSource->effects().readAfter(0, EffectReadMode::Consecutive);
        CHECK(batch && batch->size == 3);
        if (batch && batch->size == 3) {
            CHECK(batch->records[0].kind == EffectKind::Bell && batch->records[0].primary == primary);
            for (std::size_t index = 1; index < 3; ++index) {
                const auto body = decodeEffectBody(batch->records[index].kind, batch->records[index].payload);
                CHECK(body && std::holds_alternative<CommandFinishedEffect>(*body));
                if (body && std::holds_alternative<CommandFinishedEffect>(*body)) {
                    const auto& command = std::get<CommandFinishedEffect>(*body);
                    CHECK(command.exitStatus == (index == 1 ? 7 : 0));
                    CHECK(command.durationMicroseconds == (index == 1 ? 50'000u : 1'000'000u));
                    CHECK(command.currentDirectory == "/work/tree");
                    CHECK(command.commandLine == (index == 1 ? "" : "build --check"));
                }
            }
        }
        CHECK(!s.term->bellPending && s.lastEmittedCommandGeneration == 2);
        while (s.screenSource->appendEffect(BellEffect {}, primary)) {
        }
        stream->pushInbound("\a\033]133;D;9\a");
        A::service(d, s);
        A::publish(d, s);
        CHECK(s.term->bellPending);
        CHECK(s.lastEmittedCommandGeneration == 2);
        A::acknowledgeEffects(d, s, phone, phonePeer, *reader);
        A::publish(d, s);
        CHECK(!s.term->bellPending && s.lastEmittedCommandGeneration == 3);
        const auto retried = s.screenSource->effects().readAfter(s.screenSource->effects().retiredThrough(), EffectReadMode::Consecutive);
        CHECK(retried && retried->size == 2);
    }

    // Each bell a program rings reaches the primary, and only from the pane
    // that rang it: ten in one read are one Bell{count=10} on that pane's
    // log, and a second session in the same daemon logs none.
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor phonePeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* ringing = nullptr;
        StreamDouble* quiet = nullptr;
        auto& rang = A::addSession(d, 1, &ringing);
        auto& silent = A::addSession(d, 2, &quiet);
        std::unique_ptr<ClientConnection> reader;
        (void)A::addPhone(d, 1, phonePeer, { 91, 92 }, false, &reader);
        CHECK(rang.term->effectRoute == &rang.effectRoute && silent.term->effectRoute == &silent.effectRoute);
        ringing->pushInbound("\a\a\a\a\a\a\a\a\a\a");
        A::service(d, rang);
        A::service(d, silent);
        CHECK(rang.effectRoute.pendingBells() == 10 && silent.effectRoute.pendingBells() == 0);
        A::publish(d, rang);
        A::publish(d, silent);
        const auto batch = rang.screenSource->effects().readAfter(0, EffectReadMode::Consecutive);
        CHECK(batch && batch->size == 1);
        if (batch && batch->size == 1)
            CHECK_MSG(batch->records[0].kind == EffectKind::Bell && batch->records[0].count == 10,
                ("ten bells arrived as Bell{count=" + std::to_string(batch->records[0].count) + "}").c_str());
        CHECK(rang.effectRoute.pendingBells() == 0 && !rang.term->bellPending);
        const auto other = silent.screenSource->effects().readAfter(0, EffectReadMode::Consecutive);
        CHECK(!other || other->size == 0);
    }

    // The actual daemon retry leaves the same captured PCM owner pending
    // while the effect log is full, then moves the original sound in order.
    {
        using namespace Ubiquitty;
        using namespace Ubiquitty::Protocol::Sync;
        PAL::UnixFileDescriptor phonePeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        std::unique_ptr<ClientConnection> reader;
        const auto phone = A::addPhone(d, 1, phonePeer, { 111, 112 }, false, &reader);
        const ID128 primary { 111, 112 };
        auto& pending = ensurePendingSound(*s.term);
        std::array<std::int16_t, 2> pcm { -9, 17 };
        PAL::PlayTonesRequest request;
        request.wave = 6;
        request.sampleSlot = 1;
        request.samplePCM = pcm.data();
        request.sampleFrames = static_cast<std::uint32_t>(pcm.size());
        CHECK(pending.push(request, 73) == VT::PendingSoundAdmission::Queued);
        const auto* original = pending.front();
        pcm.fill(99);
        for (std::size_t index = 0; index < maxEffectRecords; ++index)
            CHECK(s.screenSource->appendEffect(BellEffect {}, primary).has_value());
        A::sounds(d, s);
        CHECK(pending.front() == original && pending.pcmBytes == 4);
        if (original)
            CHECK(original->request.samplePCM[0] == -9);
        A::acknowledgeEffects(d, s, phone, phonePeer, *reader);
        A::sounds(d, s);
        CHECK(!pending.front() && pending.pcmBytes == 0);
        const auto batch = s.screenSource->effects().readAfter(s.screenSource->effects().retiredThrough(), EffectReadMode::Consecutive);
        CHECK(batch && batch->size == 1);
        if (batch && batch->size == 1) {
            const auto body = decodeEffectBody(batch->records[0].kind, batch->records[0].payload);
            CHECK(body && std::holds_alternative<SoundEffect>(*body));
            if (body && std::holds_alternative<SoundEffect>(*body))
                CHECK(std::get<SoundEffect>(*body).sample.serial == 73);
            const std::array<std::uint8_t, 4> canonical { 247, 255, 17, 0 };
            CHECK(std::ranges::equal(batch->records[0].data, canonical));
        }
    }

    // A remote typed answer is delivered only to its original local helper,
    // after final hash verification and a fresh source/operation authority check.
    enum class ReadOutcome {
        Delivered,
        SourceExpired,
        HelperExpired,
        DeadlineExpired,
        DriverChanged,
        BadHash,
        OutputCapacityRefused
    };
    for (const auto outcome : { ReadOutcome::Delivered, ReadOutcome::SourceExpired, ReadOutcome::HelperExpired,
             ReadOutcome::DeadlineExpired, ReadOutcome::DriverChanged, ReadOutcome::BadHash, ReadOutcome::OutputCapacityRefused }) {
        using namespace Ubiquitty;
        using namespace Ubiquitty::Protocol::Sync;
        PAL::UnixFileDescriptor phonePeer, helperPeer, neighborPeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& session = A::addSession(d, 1, &stream);
        const auto phone = A::addPhone(d, 1, phonePeer, { 81, 82 }, true);
        const auto helper = A::addHelper(d, helperPeer, *stream);
        const auto neighbor = A::addPhone(d, 1, neighborPeer, { 83, 84 }, true);
        const auto handle = A::installClipboardReader(d, session, phone);
        A::typed(d, session, phone);
        const std::uint32_t localID = 0xf1234567u;
        const auto remoteID = A::requestClipboard(d, helper, 1, localID);
        CHECK(handle && remoteID && remoteID != localID);
        CHECK(A::pendingReads(d, phone) == 1);
        CHECK(A::take(d, helper, helperPeer).clipboardData.empty());
        const std::vector<std::uint8_t> pixels(180'017, 0xa5);
        EffectClipboardBundle bundle;
        bundle.count = 1;
        bundle.items[0] = { "image/png", pixels };
        auto encoded = encodeEffectClipboard(bundle, maxEffectClipboardReadBytes);
        CHECK(encoded.has_value());
        if (!encoded)
            continue;
        const auto head = encodeClipboardData({ .requestID = remoteID, .status = EffectClipboardStatus::OK, .payload = { .transfer = 93, .encodedBytes = static_cast<std::uint32_t>(encoded->size), .inlineBytes = {} } });
        CHECK(head.has_value());
        if (!head)
            continue;
        std::vector<std::uint8_t> frame;
        CHECK(appendFrame(frame, FrameKind::ClipboardData, handle, head->view()));
        A::screenBytes(d, phone, frame);
        CHECK(A::isAttached(d, phone));
        CHECK(A::clipboardBytes(d, phone) > pixels.size());
        CHECK(A::take(d, helper, helperPeer).clipboardData.empty());
        switch (outcome) {
        case ReadOutcome::SourceExpired:
            A::expireToken(d, phone);
            break;
        case ReadOutcome::HelperExpired:
            A::expireToken(d, helper);
            break;
        case ReadOutcome::DeadlineExpired:
            A::expireRequest(d, helper);
            break;
        case ReadOutcome::DriverChanged:
            A::typed(d, session, neighbor);
            CHECK(A::clipboardBytes(d, phone) == 0);
            break;
        default:
            break;
        }
        std::array<std::uint8_t, 32> digest {};
        Crypto::sha256(encoded->view(), digest);
        if (outcome == ReadOutcome::BadHash)
            digest[0] ^= 1;
        if (outcome == ReadOutcome::OutputCapacityRefused)
            A::limitClipboardOutput(d, helper);
        std::size_t offset = 0;
        std::uint32_t chunkIndex = 0;
        while (offset < encoded->size && A::isAttached(d, phone)) {
            const auto count = std::min(bulkChunkPayloadBytes, encoded->size - offset);
            const bool isLast = count == encoded->size - offset;
            const BulkChunk chunk { .transfer = 93, .index = chunkIndex++, .isLast = isLast, .bytes = encoded->view().subspan(offset, count), .sha256 = digest };
            std::vector<std::uint8_t> body;
            encode(body, chunk);
            frame.clear();
            CHECK(appendFrame(frame, FrameKind::BulkChunk, 0, body));
            A::screenBytes(d, phone, frame);
            offset += count;
            if (!isLast && A::isAttached(d, phone))
                CHECK(A::take(d, helper, helperPeer).clipboardData.empty());
        }
        CHECK(A::clipboardBytes(d, phone) == 0);
        const auto reply = A::take(d, helper, helperPeer);
        if (outcome == ReadOutcome::Delivered) {
            CHECK(reply.clipboardData.size() == 1);
            if (reply.clipboardData.size() == 1) {
                const auto& answer = reply.clipboardData[0];
                CHECK(answer.requestID == localID && answer.status == Sync::ClipboardServeStatus::OK);
                CHECK(answer.items.size() == 1);
                if (answer.items.size() == 1)
                    CHECK(answer.items[0].mime == "image/png" && answer.items[0].data == pixels);
            }
            CHECK(A::isAttached(d, phone));
            CHECK(A::pendingReads(d, phone) == 0);
        } else {
            for (const auto& answer : reply.clipboardData)
                CHECK(answer.status == Sync::ClipboardServeStatus::Refused && answer.items.empty());
            if (outcome == ReadOutcome::OutputCapacityRefused || outcome == ReadOutcome::DriverChanged) {
                CHECK(reply.clipboardData.size() == 1);
                CHECK(A::pendingReads(d, phone) == 0);
            } else {
                CHECK(!A::isAttached(d, phone));
                A::expireRequest(d, helper);
                A::expireRequests(d);
                CHECK(A::pendingReads(d, phone) == 0);
            }
        }
        CHECK(A::take(d, neighbor, neighborPeer).clipboardData.empty());
    }

    // ---- A phone whose own policy asks its person about what reaches it gets
    // the copy and the banner the host would have asked about, without the
    // host's question, so the person answers once, on the phone. A Deny the
    // host's configuration says still refuses them there. A sound, emitted
    // after its frame when someone else may be the recipient, is still the
    // host's question.
    for (const auto& [isDeniedOnTheHost, didAnotherPrincipalType] : { std::pair { false, false }, std::pair { true, false }, std::pair { false, true }, std::pair { true, true } }) {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor phonePeer, agentPeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        A::configure(d, Ubiquitty::CapabilityDecisionTable {}.replacing(CapabilityOrigin::Remote, Ubiquitty::CapabilityTier::HostEscaping, isDeniedOnTheHost ? Ubiquitty::CapabilityDecision::Deny : Ubiquitty::CapabilityDecision::Prompt));
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        A::recordBanners(d, s);
        const ID128 recipient { 141, 142 };
        const std::size_t phone = A::addAskingPhone(d, 1, phonePeer, recipient);
        const auto agent = A::addAgent(d, 1, agentPeer);
        A::typed(d, s, phone);
        if (didAnotherPrincipalType) {
            A::typed(d, s, agent);
            A::typed(d, s, phone);
        }
        CHECK(s.hasMultipleInputPrincipals == didAnotherPrincipalType);
        stream->pushInbound("\033]52;pc;Zmlyc3Q=\a"); // "first", to two buffers
        stream->pushInbound("\033]9;build finished\a");
        A::service(d, s);
        A::publish(d, s);
        A::deliver(d, s);
        const A::Received received = A::take(d, phone, phonePeer);
        CHECK_MSG(received.consentRequests.empty() && A::pendingQuestions(d) == 0, "the host asked a question of a phone that asks its own");
        const auto delivered = s.screenSource->effects().readAfter(0, EffectReadMode::Consecutive);
        CHECK_MSG(delivered.has_value(), "the typed log could not be read");
        if (!delivered)
            continue;
        std::size_t copies = 0, banners = 0;
        for (std::size_t index = 0; index < delivered->size; ++index) {
            const auto& record = delivered->records[index];
            CHECK(record.primary == recipient);
            copies += record.kind == EffectKind::ClipboardWrite && textOf(record) == "first" ? 1 : 0;
            banners += record.kind == EffectKind::Notification ? 1 : 0;
        }
        if (isDeniedOnTheHost || didAnotherPrincipalType)
            CHECK_MSG(delivered->size == 0, "an effect denied by configuration or ambiguous input reached the phone");
        else
            CHECK_MSG(copies == 2 && banners == 1 && delivered->size == 3, "the copy to both buffers and the banner did not reach the phone");
        // Other devices post what is recorded without asking, so a banner
        // nobody here was asked about is not recorded for them.
        CHECK_MSG(A::recordedBanners(d) == 0, "a banner forwarded on the phone's own question was recorded for other devices");
        stream->pushInbound("\033[5;8;1,~");
        A::service(d, s);
        const A::Received tone = A::take(d, phone, phonePeer);
        if (isDeniedOnTheHost || didAnotherPrincipalType) {
            CHECK(tone.consentRequests.empty());
        } else {
            CHECK_MSG(tone.consentRequests.size() == 1, "the host forwarded a sound it could not hold to its recipient");
            if (tone.consentRequests.size() == 1)
                CHECK(tone.consentRequests[0].capabilityCode == static_cast<std::uint16_t>(std::to_underlying(Capability::PlaySound)));
        }
    }

    // ---- A newly bootstrapped human connection keeps the host-authenticated
    // account's input principal even when its token and attachment change.
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor phonePeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const auto phone = A::addPhone(d, 1, phonePeer, { 201, 202 });
        CHECK(A::refuseUnprovedInput(d, s, A::attachmentID(d, phone)) == WriteStatus::Withdrawn);
        CHECK_MSG(!A::firstInputPrincipal(s) && !s.hasMultipleInputPrincipals,
            "input refused by attachment proof changed the session's provenance");
    }
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor firstPeer, resumedPeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const ID128 instance { 211, 212 };
        const auto first = A::addPhone(d, 1, firstPeer, instance);
        const auto originalAttachment = A::attachmentID(d, first);
        const auto request = A::resumeRequest(d, first);
        A::typed(d, s, first);
        CHECK(!s.hasMultipleInputPrincipals);
        A::depart(d, first);
        const auto resumed = A::addPhone(d, 1, resumedPeer, instance, false, nullptr, request);
        CHECK(A::attachmentID(d, resumed) == originalAttachment);
        A::typed(d, s, resumed);
        CHECK_MSG(!s.hasMultipleInputPrincipals, "a resumed phone became a second input principal");
    }
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor firstPeer, nextPeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        A::configure(d, Ubiquitty::CapabilityDecisionTable {}.replacing(CapabilityOrigin::Remote, Ubiquitty::CapabilityTier::HostEscaping, Ubiquitty::CapabilityDecision::Prompt));
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const auto first = A::addAskingPhone(d, 1, firstPeer, { 221, 222 }, "first-bootstrap-token");
        const auto oldAttachment = A::attachmentID(d, first);
        A::typed(d, s, first);
        A::depart(d, first);
        const auto next = A::addAskingPhone(d, 1, nextPeer, { 223, 224 }, "next-bootstrap-token");
        CHECK(A::attachmentID(d, next) != oldAttachment);
        A::typed(d, s, next);
        CHECK_MSG(!s.hasMultipleInputPrincipals, "a new bootstrap by the same authenticated account became a second principal");
        stream->pushInbound("\033]52;c;Zmlyc3Q=\a");
        A::service(d, s);
        A::publish(d, s);
        A::deliver(d, s);
        CHECK_MSG(s.screenSource->effects().newestSequence() != 0, "the newly bootstrapped human's clipboard write did not reach its recipient");
    }
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor firstPeer, secondPeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        A::configure(d, Ubiquitty::CapabilityDecisionTable {}.replacing(CapabilityOrigin::Remote, Ubiquitty::CapabilityTier::HostEscaping, Ubiquitty::CapabilityDecision::Prompt));
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const auto first = A::addSSHPhone(d, 1, firstPeer, { 231, 232 });
        A::typed(d, s, first);
        stream->pushInbound("\033]52;c;Zmlyc3Q=\a");
        A::service(d, s);
        A::publish(d, s);
        A::deliver(d, s);
        CHECK_MSG(s.screenSource->effects().newestSequence() != 0, "the SSH stream client's clipboard write did not reach its recipient");
        const auto firstEffect = s.screenSource->effects().newestSequence();
        const std::string bridgeGrant = A::grantKey(d, first);
        A::depart(d, first);
        const auto second = A::addSSHPhone(d, 1, secondPeer, { 233, 234 });
        CHECK(bridgeGrant == A::grantKey(d, second));
        A::typed(d, s, second);
        CHECK_MSG(!s.hasMultipleInputPrincipals, "the SSH bridge's authenticated account changed across a new bootstrap");
        stream->pushInbound("\033]52;c;c2Vjb25k\a");
        A::service(d, s);
        A::publish(d, s);
        A::deliver(d, s);
        CHECK_MSG(s.screenSource->effects().newestSequence() > firstEffect,
            "the new SSH bridge's clipboard write did not reach its recipient");
    }
    {
        Ubiquitty::PAL::UnixFileDescriptor sshPeer, quicPeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const auto ssh = A::addSSHPhone(d, 1, sshPeer, { 237, 238 });
        A::typed(d, s, ssh);
        A::depart(d, ssh);
        const auto quic = A::addAskingPhone(d, 1, quicPeer, { 239, 240 }, "next-carrier-token");
        A::typed(d, s, quic);
        CHECK_MSG(!s.hasMultipleInputPrincipals, "switching one authenticated account from SSH to QUIC made a second principal");
    }
    {
        Ubiquitty::PAL::UnixFileDescriptor phonePeer, agentPeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const auto phone = A::addPhone(d, 1, phonePeer, { 235, 236 });
        const auto agent = A::addAgent(d, 1, agentPeer);
        A::typed(d, s, phone);
        A::typed(d, s, agent);
        CHECK_MSG(s.hasMultipleInputPrincipals, "agent input was attributed to the human account");
    }
    {
        Ubiquitty::PAL::UnixFileDescriptor agentPeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const auto agent = A::addTokenlessAgent(d, agentPeer);
        A::typed(d, s, agent);
        CHECK(A::firstInputPrincipal(s) == A::grantKey(d, agent));
        CHECK(A::inputOrigin(s) == CapabilityOrigin::Agent);
        CHECK(!s.hasMultipleInputPrincipals);
    }

    // ---- A banner this host admits by its own configuration is recorded for
    // other devices, whoever receives it live.
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor phonePeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        A::configure(d, Ubiquitty::CapabilityDecisionTable {}.replacing(CapabilityOrigin::Remote, Ubiquitty::CapabilityTier::HostEscaping, Ubiquitty::CapabilityDecision::Allow));
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        A::recordBanners(d, s);
        const std::size_t phone = A::addAskingPhone(d, 1, phonePeer, { 171, 172 });
        A::typed(d, s, phone);
        stream->pushInbound("\033]9;build finished\a");
        A::service(d, s);
        CHECK_MSG(A::recordedBanners(d) == 1, "a banner the host admitted was not recorded for other devices");
    }

    // ---- A recipient that cannot show a question declines it as unshown. It
    // stays with the others asked, so one busy device never refuses it for a
    // person looking at it on another; once none is left it is refused at once
    // rather than at its deadline, and the next reach is asked about again.
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor firstPeer, secondPeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const std::size_t first = A::addPhone(d, 1, firstPeer, { 181, 182 });
        const std::size_t second = A::addPhone(d, 1, secondPeer, { 183, 184 });
        A::typed(d, s, first);
        stream->pushInbound("\033]9;build finished\a");
        A::service(d, s);
        const A::Received asked = A::take(d, first, firstPeer);
        CHECK(A::take(d, second, secondPeer).consentRequests.size() == 1);
        CHECK_MSG(asked.consentRequests.size() == 1, "the first phone was not asked about the banner");
        const std::uint32_t question = asked.consentRequests.empty() ? 0 : asked.consentRequests[0].requestID;
        A::answer(d, first, question, Sync::ConsentChoice::Unshown);
        CHECK_MSG(A::pendingQuestions(d) == 1, "one phone that could not show the question refused it for the other");
        A::answer(d, second, question, Sync::ConsentChoice::Unshown);
        CHECK_MSG(A::pendingQuestions(d) == 0, "a question nobody could show waited for its deadline");
        stream->pushInbound("\033]9;tests passed\a");
        A::service(d, s);
        CHECK_MSG(A::take(d, first, firstPeer).consentRequests.size() == 1, "the capability was never asked about again after nobody could show its question");
    }

    // ---- A question stays open exactly while some recipient could still show
    // it. One that shows a person no question is never asked; one that departs
    // leaves every question it was asked; and when the last that could show it
    // goes, the question is refused at once, not at its deadline, and its
    // capability is asked about again.
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor silentPeer, firstPeer, secondPeer, laterPeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const std::size_t silent = A::addSilentPhone(d, 1, silentPeer, { 191, 192 });
        const std::size_t first = A::addPhone(d, 1, firstPeer, { 193, 194 });
        const std::size_t second = A::addPhone(d, 1, secondPeer, { 195, 196 });
        A::typed(d, s, first);
        stream->pushInbound("\033]9;build finished\a");
        A::service(d, s);
        CHECK_MSG(A::take(d, silent, silentPeer).consentRequests.empty(), "a client that shows no question was asked one");
        CHECK(A::take(d, first, firstPeer).consentRequests.size() == 1 && A::take(d, second, secondPeer).consentRequests.size() == 1);
        A::depart(d, first);
        CHECK_MSG(A::pendingQuestions(d) == 1, "one departing recipient refused a question another could still show");
        A::depart(d, second);
        CHECK_MSG(A::pendingQuestions(d) == 0, "a question no remaining recipient could show waited for its deadline");
        const std::size_t later = A::addPhone(d, 1, laterPeer, { 197, 198 });
        stream->pushInbound("\033]9;tests passed\a");
        A::service(d, s);
        CHECK_MSG(A::take(d, later, laterPeer).consentRequests.size() == 1, "the capability was never asked about again after its recipients departed");
    }

    // ---- A recipient the host drops for a reader too far behind leaves its
    // questions as it is dropped. A capability question is never asked again
    // of a person who resumes, so it is refused at once, not when the dropped
    // client is reaped.
    {
        Ubiquitty::PAL::UnixFileDescriptor phonePeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const std::size_t phone = A::addPhone(d, 1, phonePeer, { 199, 200 });
        A::typed(d, s, phone);
        stream->pushInbound("\033]9;build finished\a");
        A::service(d, s);
        CHECK(A::take(d, phone, phonePeer).consentRequests.size() == 1);
        A::dropBehind(d, phone);
        CHECK_MSG(A::pendingQuestions(d) == 0, "a question its dropped recipient could no longer show waited for the reap");
    }

    // ---- A question the host asks expires when nobody answers it, whatever
    // its kind: the host retires it unanswered and refuses for itself, and the
    // next reach for the capability is asked about again rather than held
    // behind a question nobody will see.
    {
        using namespace Ubiquitty::Protocol::Sync;
        Ubiquitty::PAL::UnixFileDescriptor phonePeer;
        Daemon d;
        A::setSocketPath(d, std::string(directory) + "/socket");
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        const std::size_t phone = A::addPhone(d, 1, phonePeer, { 151, 152 });
        A::typed(d, s, phone);
        stream->pushInbound("\033]9;build finished\a");
        A::service(d, s);
        const A::Received asked = A::take(d, phone, phonePeer);
        CHECK_MSG(asked.consentRequests.size() == 1, "the phone was not asked about the banner");
        const std::uint32_t question = asked.consentRequests.empty() ? 0 : asked.consentRequests[0].requestID;
        CHECK(!asked.consentRequests.empty() && asked.consentRequests[0].capabilityCode == static_cast<std::uint16_t>(std::to_underlying(Capability::DesktopNotification)));
        stream->pushInbound("\033]9;still building\a");
        A::service(d, s);
        CHECK_MSG(A::take(d, phone, phonePeer).consentRequests.empty(), "a second banner asked while the first question stood");
        A::expireQuestions(d, Ubiquitty::Host::ScreenPlaneAccess::acceptedAt());
        CHECK_MSG(A::pendingQuestions(d) == 1, "a question was retired before its deadline");
        A::expireQuestions(d, std::numeric_limits<std::uint64_t>::max());
        CHECK_MSG(A::pendingQuestions(d) == 0, "a banner question outlived its deadline");
        A::answer(d, phone, question, Sync::ConsentChoice::AllowAlways);
        CHECK_MSG(!A::isRemembered(d, Capability::DesktopNotification, CapabilityOrigin::Remote, A::grantKey(d, phone)), "an answer after the deadline granted the capability");
        stream->pushInbound("\033]9;tests passed\a");
        A::service(d, s);
        CHECK_MSG(A::take(d, phone, phonePeer).consentRequests.size() == 1, "the capability was never asked about again after its question expired");
    }

    ::unlink(grantsFile.c_str());
    ::unlink((grantsFile + ".lock").c_str());
    ::unlink((std::string(directory) + "/socket.tokens").c_str());
    ::rmdir(directory);

    if (failures == 0)
        printf("consent_remote_effects: PASSED — the phone is asked about its own device and may say Always; "
               "both copies and the banner reach it and not the agent; other host-escaping grants stay single-use\n");
    else
        printf("consent_remote_effects: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
