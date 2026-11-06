// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/service_pty_turn.cpp — one turn of the PTY pump.
//
// servicePTY is where a program's output becomes everything else: the bytes
// reach Core, a shell that went away is noticed, mouse tracking a dead job left
// on is cleared, and — the part this file exists for — a capability the output
// reached for gets a human asked about it.
//
// That last one is the ASK side of the consent-prompt flow, and nothing else
// covers it. Its answer side has a coverer (capability_consent.cpp drives
// consent.respond with a pending record injected by hand), which tests what the
// daemon does with a human's ruling but not the code that produces the
// question — mints the request id, records what was actually asked, and picks
// who to ask. A test that hands the daemon its own pending state cannot notice
// that nothing ever creates one.
//
// Three properties here are the gate, not decoration:
//
//   * The pending record is keyed by the id the DAEMON minted and holds the
//     capability the daemon asked about. A response can then only approve what
//     was asked, never a capability of the responder's choosing.
//   * The question goes to the humans on that session. Not the agent whose
//     output triggered it — an agent that could answer its own prompt is not
//     gated at all — and not a human watching a different session.
//   * The same triple asks once. A program looping on a refused capability
//     would otherwise turn the consent prompt into a denial of service against
//     the person being asked.
//
// The fixture has to repeat four things the daemon does, and each one is silent
// when omitted: the three lines createSession runs (grant store, consent queue,
// policy installed on the Terminal) and the driving identity `stampInputOrigin`
// stamps. Miss any of them and the session parses the same
// bytes and prompts for nothing — which reads as a passing no-prompt case
// rather than a broken fixture. So every null here is asserted in a fixture
// shown to prompt when the one thing under test is changed back.
//
// Exit 0 on success.
//

#include "NotifyRead.h"
#include "Platform/Host/Daemon.h"
#include "TrustedLocalPeer.h"

#include "Assertions.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/StreamDouble.h"
#include "TerminalFixture.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Mouse/Mouse.h"
#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Capability.h"
#include "PAL/Stream.h"
#include "Platform/Host/TokenStore.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace Ubiquitty {
namespace Host {

struct ServicePTYAccess {
    // A session wired the way createSession wires one: the capability policy
    // reaches the host-wide grant store and the daemon's consent queue, and the
    // Terminal routes its checks through that policy. Without these three the
    // session parses the same output and prompts for nothing.
    static Daemon::Session& addSession(Daemon& d, std::uint32_t id, int rows, int columns,
        TestDoubles::StreamDouble** streamOut, Ubiquitty::CapabilityOrigin origin)
    {
        auto session = std::make_unique<Daemon::Session>();
        session->id = id;
        session->term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*session->term, rows, columns);
        // The three lines createSession runs, repeated here: without them the
        // policy has no grant store, the Prompt has nowhere to queue, and the
        // Terminal never consults the policy at all.
        session->capabilityPolicy.setGrantStore(&d.m_grantStore);
        session->capabilityPolicy.setConsentQueue(&d.m_consentQueue);
        session->term->capabilityPolicy = &session->capabilityPolicy;
        session->driverOriginFloor = origin;
        session->outputOrigin = origin;
        auto stream = std::make_unique<TestDoubles::StreamDouble>();
        if (streamOut)
            *streamOut = stream.get();
        session->stream = std::move(stream);
        session->screenSource = std::make_unique<Protocol::Sync::PaneSource>(*session->term, Protocol::Sync::ID128 { .low = id, .high = 1 });
        Daemon::Session& ref = *session;
        d.m_sessions.push_back(std::move(session));
        return ref;
    }

    // A human on `sessionID`: verified-local and no agent label, so it is one
    // of the clients a prompt can be put to.
    static std::size_t addHuman(Daemon& d, std::uint32_t sessionID)
    {
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        Ubiquitty::Host::seedTrustedLocalPeer(c);
        c.sessionID = sessionID;
        auto* session = d.findSession(sessionID);
        CHECK(session);
        if (!session)
            return static_cast<std::size_t>(-1);
        c.workspaceID = d.hostedWorkspaceFor(*session).workspaceID;
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Human, Protocol::Sync::Role::Viewer);
        CHECK(d.hasLiveStructureAttachment(d.m_clients[index]));
        return index;
    }

    static std::size_t addRemoteHuman(Daemon& d, std::uint32_t sessionID, std::string tokenID,
        Protocol::Sync::ScopeSet scope = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive },
        Protocol::Sync::Role role = Protocol::Sync::Role::Driver)
    {
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.hasToken = true;
        c.hasBootstrapToken = true;
        c.isInteractiveUser = true;
        c.tokenID = std::move(tokenID);
        c.tokenScope = scope;
        c.sessionID = sessionID;
        auto* session = d.findSession(sessionID);
        CHECK(session);
        if (!session)
            return static_cast<std::size_t>(-1);
        c.workspaceID = d.hostedWorkspaceFor(*session).workspaceID;
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Human, role);
        CHECK(d.hasLiveStructureAttachment(d.m_clients[index]));
        if (role == Protocol::Sync::Role::Viewer)
            CHECK(d.m_clients[index].syncConnection && d.m_clients[index].syncConnection->grantedRole() == Protocol::Sync::Role::Viewer);
        return index;
    }

    static std::size_t addAgent(Daemon& d, std::uint32_t sessionID, const std::string& tokenPath, const std::string& token)
    {
        d.m_tokenPath = tokenPath;
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.peerProcess = "/usr/bin/some-agent";
        c.agentLabel = "agent";
        c.sessionID = sessionID;
        // A standing grant welcomes the agent inside its admission, which
        // flushes it; a client with no descriptor fails that write and is
        // dropped. Nothing here reads what the daemon sends it.
        c.fileDescriptor = PAL::UnixFileDescriptor(::open("/dev/null", O_WRONLY | O_CLOEXEC));
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        auto* session = d.findSession(sessionID);
        CHECK(session);
        if (!session)
            return index;
        // The exact stored-token principal has prior consent for this session.
        // Production Greet admission still verifies its credential and scope.
        (void)session->addSessionGrant("token:" + Crypto::sha256Hex(token));
        Protocol::Sync::Greet greeting;
        greeting.wireVersion = Protocol::Sync::wireVersion;
        greeting.appVersion = Protocol::Sync::appVersion();
        greeting.clientInstance = { .low = d.m_clients[index].clientID, .high = 0x707479 };
        greeting.clientKind = Protocol::Sync::ClientKind::Agent;
        greeting.requestedRole = Protocol::Sync::Role::Agent;
        greeting.request = { .target = Protocol::Sync::AdmissionTarget::Session, .sessionID = sessionID, .expectedDaemonInstanceID = d.m_daemonInstanceID, .agentLabel = "agent", .isAttachOnly = true };
        greeting.credential.emplace(token.begin(), token.end());
        std::vector<std::uint8_t> body, wire;
        Protocol::Sync::encode(body, greeting);
        CHECK(!body.empty() && Protocol::Sync::decodeGreet(body));
        CHECK(Protocol::Sync::appendFrame(wire, Protocol::Sync::FrameKind::Greet, 0, body));
        ScreenPlaneAccess::receive(d, index, wire);
        CHECK(d.m_clients[index].hasToken && digestsEqual(d.m_clients[index].tokenID, Crypto::sha256Hex(token)));
        const Protocol::Sync::ScopeSet expectedScope { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive };
        CHECK(d.m_clients[index].tokenScope == expectedScope);
        CHECK(d.hasLiveStructureAttachment(d.m_clients[index]));
        CHECK(d.m_clients[index].syncConnection && d.m_clients[index].syncConnection->grantedRole() == Protocol::Sync::Role::Agent);
        return index;
    }

    // What `stampInputOrigin` does when a client drives the session:
    // stamp the driving principal's grant key on the policy.
    // The consent queue refuses an empty identity, so a session nobody has
    // driven queues nothing however its program behaves.
    static void stampDriver(Daemon& d, Daemon::Session& s, std::size_t index)
    {
        d.stampInputOrigin(s, d.m_clients[index]);
    }

    static void service(Daemon& d, Daemon::Session& s) { d.servicePTY(s); }

    static void grantCapability(Daemon& d, std::size_t index, Ubiquitty::Capability capability)
    {
        d.m_grantStore.grant(capability, Ubiquitty::CapabilityOrigin::Remote,
            d.grantKeyFor(d.m_clients[index]), Ubiquitty::CapabilityGrantScope::Always);
    }

    static std::string grantKey(const Daemon& d, std::size_t index) { return d.grantKeyFor(d.m_clients[index]); }

    /// Samples the session's foreground without reading more PTY output.
    ///
    /// - Parameters:
    ///   - d: The owning daemon.
    ///   - s: The session whose current foreground is sampled.
    static void probeForeground(Daemon& d, Daemon::Session& s) { d.probeForegroundIfDue(s); }

    // Every canonical question queued to this client, in order. Keep its bytes
    // so repeated checks below still inspect the same admitted history.
    static std::vector<Protocol::Sync::ConsentQuestion> consentRequests(Daemon& d, std::size_t index)
    {
        std::vector<Protocol::Sync::ConsentQuestion> requests;
        d.drainSyncConnection(index, ScreenPlaneAccess::now());
        const std::vector<std::uint8_t>& out = d.m_clients[index].outputBuffer;
        Protocol::Sync::FrameDecoder decoder;
        decoder.append(out);
        while (const auto frame = decoder.next()) {
            if (frame->kind == Protocol::Sync::FrameKind::Notify)
                if (const auto notify = UbiquittyTest::notifyOf(frame->body))
                    if (const auto question = UbiquittyTest::consentQuestionOf(*notify))
                        requests.push_back(*question);
        }
        CHECK(decoder.error() == Protocol::Sync::EnvelopeError::None && decoder.bufferedBytes() == 0);
        return requests;
    }

    // What the daemon recorded for a request id: the capability it actually
    // asked about, and the session it asked for.
    static bool pendingGrant(const Daemon& d, std::uint32_t requestID, Ubiquitty::Capability& capability,
        Ubiquitty::CapabilityOrigin& origin, std::uint32_t& sessionID)
    {
        const auto it = d.m_pendingCapabilityGrants.find(requestID);
        if (it == d.m_pendingCapabilityGrants.end())
            return false;
        capability = it->second.capability;
        origin = it->second.origin;
        sessionID = it->second.sessionID;
        return true;
    }

    static std::size_t pendingGrantCount(const Daemon& d) { return d.m_pendingCapabilityGrants.size(); }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::ServicePTYAccess;
using Ubiquitty::TestDoubles::StreamDouble;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

using A = ServicePTYAccess;

// DECUDK — reprograms what a key sends, so a stream that can write it can
// inject input. Host-escaping, which makes it a prompt for an agent-driven
// session rather than an outright allow or refusal.
constexpr const char* reprogramsAKey = "\033P1;1|17/4142\033\\";

} // namespace

int main()
{
    char directoryTemplate[] = "/tmp/ubq-service-pty-XXXXXX";
    const char* made = ::mkdtemp(directoryTemplate);
    CHECK(made);
    if (!made)
        return UBIQUITTY_TEST_SUMMARY();
    const std::string directory(made);
    const std::string tokenPath = directory + "/tokens";
    struct TokenCleanup {
        const std::string& directory;
        const std::string& path;
        ~TokenCleanup()
        {
            ::unlink(path.c_str());
            ::unlink((path + ".lock").c_str());
            ::unlink((path + ".metadata").c_str());
            ::rmdir(directory.c_str());
        }
    } cleanup { directory, tokenPath };
    const std::string token = "service-pty-owned-agent-token";
    Sync::TokenRecord record;
    record.hashHex = Ubiquitty::Crypto::sha256Hex(token);
    record.scope = { Sync::Scope::Observe, Sync::Scope::Drive };
    record.label = "service-pty";
    CHECK(Ubiquitty::Host::writeTokens(tokenPath, { record }) == Ubiquitty::Host::TokenWriteOutcome::Durable);
    // ---- 1. A read that fills the buffer is followed by another, and so is a
    // short one: the turn ends at a would-block (Runtime::runDrainPass, the pass
    // every frontend drains by). Every byte the program wrote reaches Core in
    // this turn, rather than a chunk per service tick — a full-buffer burst that
    // stopped after one read would deliver a screen at the rate the loop happens
    // to wake.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Local);
        const std::string burst(65536 + 4096, 'x');
        stream->pushInbound(burst);
        stream->setQuantumBytes(65536); // one pty-sized quantum per read

        A::service(d, s);

        CHECK(stream->bytesRemaining() == 0);
        CHECK(stream->readCalls >= 3); // full, short, then the would-block that ends it
        CHECK(!s.shellExited);
    }

    // ---- 1b. A writer's refill gap ends the turn. The poll loop is shared by
    // every session and client, and a ride's budget starts over after each
    // read, so riding the gaps of a writer that trickles a byte every few
    // microseconds would hold the loop until the pass's byte budget — seconds
    // in which no other session's output, no input and no timer is served. The
    // rest is read on the next turn, at once, because the fd is still readable.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Local);
        stream->pushInbound(std::string(3 * 4096, 'y'));
        stream->setQuantumBytes(4096);
        stream->refillAfterEagainCount(2);

        A::service(d, s);

        CHECK_MSG(stream->bytesRemaining() > 0, "the turn rode the writer's refill gap");
        CHECK(stream->againCount == 1);
        CHECK(!s.shellExited);
    }

    // ---- 2. Nothing to read is not a shell that left.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Local);

        A::service(d, s);

        CHECK(!s.shellExited);
    }

    // ---- 3 + 4. End of file and a fatal read both mean the shell is going
    // away, and the session has to say so or the run loop never reaps it and
    // every later keystroke is dropped into a PTY nobody owns.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Local);
        stream->signalEndOfFile();

        A::service(d, s);
        CHECK(s.shellExited);
    }
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Local);
        stream->failWith(EIO);

        A::service(d, s);
        CHECK(s.shellExited);
    }

    // ---- 4b. Nobody to ask. On a host with no human attached — the ordinary
    // state of a server — the ruler loop matches nothing and the request
    // reaches no one. The triple must not be left recorded as asked: the queue
    // dedupes, so an entry nobody saw suppresses every later request for it,
    // and the first reach for each capability would consume its own prompt into
    // silence. A human attaching an hour later would be shown nothing, and the
    // program reaching again would emit nothing.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Agent);
        const std::size_t agent = A::addAgent(d, 1, tokenPath, token);
        A::stampDriver(d, s, agent);
        stream->pushInbound(reprogramsAKey);
        A::service(d, s); // no human on the session: nobody is asked

        // The human arrives, and the program reaches again.
        const std::size_t human = A::addHuman(d, 1);
        stream->pushInbound(reprogramsAKey);
        A::service(d, s);

        CHECK(A::consentRequests(d, human).size() == 1);

        // And exactly once. A triple delivered to someone who can answer stays
        // recorded until they do, so reaching again while the question is
        // outstanding adds nothing — which is the dedup doing the job it is for,
        // and the reason "asked" has to mean delivered rather than reached.
        stream->pushInbound(reprogramsAKey);
        A::service(d, s);
        CHECK(A::consentRequests(d, human).size() == 1);
    }

    // ---- 5. The ask. An agent-driven program reprograms a key: the
    // human on that session is asked, the request says it is a capability
    // grant rather than an attach, and the daemon records what it asked under
    // the id it minted. A response can then only approve that.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Agent);
        const std::size_t human = A::addHuman(d, 1);
        const std::size_t agent = A::addAgent(d, 1, tokenPath, token);
        A::stampDriver(d, s, agent);
        stream->pushInbound(reprogramsAKey);

        A::service(d, s);

        const std::vector<Sync::ConsentQuestion> asked = A::consentRequests(d, human);
        CHECK(asked.size() == 1);
        if (asked.size() == 1) {
            CHECK(asked[0].action == 2); // a capability grant, not an attach or a kill
            CHECK(asked[0].sessionID == 1);
            CHECK(asked[0].capabilityCode == static_cast<std::uint16_t>(std::to_underlying(Ubiquitty::Capability::ProgramKeys)));

            Ubiquitty::Capability capability {};
            Ubiquitty::CapabilityOrigin origin {};
            std::uint32_t sessionID = 0;
            CHECK(A::pendingGrant(d, asked[0].requestID, capability, origin, sessionID));
            CHECK(capability == Ubiquitty::Capability::ProgramKeys);
            CHECK(origin == Ubiquitty::CapabilityOrigin::Agent);
            CHECK(sessionID == 1);
        }
    }

    // ---- 6. Who is asked, and who is not. The agent whose output triggered
    // the prompt is not asked — one that could answer its own question is not
    // gated at all. Neither is a human on another session; every human on this
    // one is.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Agent);
        A::addSession(d, 2, 24, 80, nullptr, Ubiquitty::CapabilityOrigin::Agent);
        const std::size_t human = A::addHuman(d, 1);
        const std::size_t agent = A::addAgent(d, 1, tokenPath, token);
        const std::size_t elsewhere = A::addHuman(d, 2);
        const std::size_t second = A::addHuman(d, 1);
        A::stampDriver(d, s, agent);
        stream->pushInbound(reprogramsAKey);

        A::service(d, s);

        CHECK(A::consentRequests(d, human).size() == 1);
        CHECK(A::consentRequests(d, agent).empty());
        CHECK(A::consentRequests(d, elsewhere).empty());
        CHECK(A::consentRequests(d, second).size() == 1);
    }

    // ---- 7. The same capability asks once. A program looping on a refused
    // capability would otherwise put the prompt on a loop too, which is a
    // denial of service against the person being asked and trains them to
    // dismiss it.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Agent);
        const std::size_t human = A::addHuman(d, 1);
        A::stampDriver(d, s, A::addAgent(d, 1, tokenPath, token));

        for (int attempt = 0; attempt < 5; ++attempt) {
            stream->pushInbound(reprogramsAKey);
            A::service(d, s);
        }

        CHECK(A::consentRequests(d, human).size() == 1);
        CHECK(A::pendingGrantCount(d) == 1);
    }

    // ---- 8. The null for 5 through 7: the same output from a LOCAL session
    // asks nobody. A local program already runs with the user's own privileges,
    // so prompting for it would be theatre — and a daemon that asked for
    // everything would pass every case above.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Local);
        const std::size_t human = A::addHuman(d, 1);
        A::stampDriver(d, s, human);
        stream->pushInbound(reprogramsAKey);

        A::service(d, s);

        CHECK(A::consentRequests(d, human).empty());
        CHECK(A::pendingGrantCount(d) == 0);
    }

    // ---- 8a. Two authenticated human clients of this host account share an
    // input principal, while each connection keeps its own capability grant.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Remote);
        const std::size_t first = A::addRemoteHuman(d, 1, "remote-human-a");
        const std::size_t second = A::addRemoteHuman(d, 1, "remote-human-b");
        CHECK(first != static_cast<std::size_t>(-1) && second != static_cast<std::size_t>(-1));
        if (first != static_cast<std::size_t>(-1) && second != static_cast<std::size_t>(-1)) {
            CHECK(A::grantKey(d, first) != A::grantKey(d, second));
            A::stampDriver(d, s, first);
            A::stampDriver(d, s, second);
            A::stampDriver(d, s, first);
            A::grantCapability(d, first, Ubiquitty::Capability::DesktopNotification);
            CHECK(s.capabilityPolicy.currentIdentity() == A::grantKey(d, first));
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));

            A::service(d, s);

            CHECK(s.term->currentCapabilityOrigin == Ubiquitty::CapabilityOrigin::Remote);
            CHECK(s.capabilityPolicy.currentIdentity() == A::grantKey(d, first));
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));

            A::stampDriver(d, s, second);
            A::service(d, s);
            CHECK(s.capabilityPolicy.currentIdentity() == A::grantKey(d, second));
            CHECK(!Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));

            A::stampDriver(d, s, first);
            A::service(d, s);
            CHECK(s.capabilityPolicy.currentIdentity() == A::grantKey(d, first));
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));
        }
    }

    // An agent that has sent input can leave output running while a remote
    // person is the last sender, even though its origin is less restrictive.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Remote);
        const std::size_t remote = A::addRemoteHuman(d, 1, "remote-human-a");
        const std::size_t agent = A::addAgent(d, 1, tokenPath, token);
        CHECK(remote != static_cast<std::size_t>(-1) && agent != static_cast<std::size_t>(-1));
        if (remote != static_cast<std::size_t>(-1) && agent != static_cast<std::size_t>(-1)) {
            CHECK(A::grantKey(d, remote) != A::grantKey(d, agent));
            A::stampDriver(d, s, remote);
            A::stampDriver(d, s, agent);
            A::stampDriver(d, s, remote);
            A::grantCapability(d, remote, Ubiquitty::Capability::DesktopNotification);
            CHECK(s.term->currentCapabilityOrigin == Ubiquitty::CapabilityOrigin::Remote);
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));

            A::service(d, s);

            CHECK(s.term->currentCapabilityOrigin == Ubiquitty::CapabilityOrigin::Remote);
            CHECK(s.capabilityPolicy.currentIdentity().empty());
            CHECK(!Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));
        }
    }

    // An attached driver that has never sent input does not change the last
    // sender's remembered grant identity.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Remote);
        const std::size_t remote = A::addRemoteHuman(d, 1, "remote-human-a");
        const std::size_t agent = A::addAgent(d, 1, tokenPath, token);
        CHECK(remote != static_cast<std::size_t>(-1) && agent != static_cast<std::size_t>(-1));
        if (remote != static_cast<std::size_t>(-1) && agent != static_cast<std::size_t>(-1)) {
            CHECK(A::grantKey(d, remote) != A::grantKey(d, agent));
            A::stampDriver(d, s, remote);
            A::grantCapability(d, remote, Ubiquitty::Capability::DesktopNotification);
            CHECK(s.capabilityPolicy.currentIdentity() == A::grantKey(d, remote));
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));

            A::service(d, s);

            CHECK(s.term->currentCapabilityOrigin == Ubiquitty::CapabilityOrigin::Remote);
            CHECK(s.capabilityPolicy.currentIdentity() == A::grantKey(d, remote));
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));
        }
    }

    // A principal's input history outlives its connection. Removing a sender
    // does not make the remaining person's remembered grant reusable.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Remote);
        const std::size_t remote = A::addRemoteHuman(d, 1, "remote-human-a");
        const std::size_t agent = A::addAgent(d, 1, tokenPath, token);
        CHECK(remote != static_cast<std::size_t>(-1) && agent != static_cast<std::size_t>(-1));
        if (remote != static_cast<std::size_t>(-1) && agent != static_cast<std::size_t>(-1)) {
            CHECK(A::grantKey(d, remote) != A::grantKey(d, agent));
            A::stampDriver(d, s, remote);
            A::stampDriver(d, s, agent);
            A::stampDriver(d, s, remote);
            A::grantCapability(d, remote, Ubiquitty::Capability::DesktopNotification);
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));
            CHECK(remote < agent);
            Ubiquitty::Host::ScreenPlaneAccess::reap(d, agent);
            CHECK(s.capabilityPolicy.currentIdentity() == A::grantKey(d, remote));

            A::service(d, s);

            CHECK(s.term->currentCapabilityOrigin == Ubiquitty::CapabilityOrigin::Remote);
            CHECK(s.capabilityPolicy.currentIdentity().empty());
            CHECK(!Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));
        }
    }

    // Two connections holding the same principal do not make it ambiguous.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Remote);
        const std::size_t first = A::addRemoteHuman(d, 1, "remote-human-a");
        const std::size_t second = A::addRemoteHuman(d, 1, "remote-human-a");
        CHECK(first != static_cast<std::size_t>(-1) && second != static_cast<std::size_t>(-1));
        if (first != static_cast<std::size_t>(-1) && second != static_cast<std::size_t>(-1)) {
            CHECK(A::grantKey(d, first) == A::grantKey(d, second));
            A::stampDriver(d, s, first);
            A::stampDriver(d, s, second);
            A::grantCapability(d, first, Ubiquitty::Capability::DesktopNotification);
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));

            A::service(d, s);

            CHECK(s.term->currentCapabilityOrigin == Ubiquitty::CapabilityOrigin::Remote);
            CHECK(s.capabilityPolicy.currentIdentity() == A::grantKey(d, first));
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));
        }
    }

    // An observe-only principal cannot inject the output being attributed.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Remote);
        const std::size_t driver = A::addRemoteHuman(d, 1, "remote-human-a");
        const std::size_t observer = A::addRemoteHuman(d, 1, "remote-observer", { Sync::Scope::Observe });
        CHECK(driver != static_cast<std::size_t>(-1) && observer != static_cast<std::size_t>(-1));
        if (driver != static_cast<std::size_t>(-1) && observer != static_cast<std::size_t>(-1)) {
            CHECK(A::grantKey(d, driver) != A::grantKey(d, observer));
            A::stampDriver(d, s, driver);
            A::grantCapability(d, driver, Ubiquitty::Capability::DesktopNotification);
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));

            A::service(d, s);

            CHECK(s.term->currentCapabilityOrigin == Ubiquitty::CapabilityOrigin::Remote);
            CHECK(s.capabilityPolicy.currentIdentity() == A::grantKey(d, driver));
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));
        }
    }

    // A viewer's granted role prevents input even when its token carries Drive.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Remote);
        const std::size_t driver = A::addRemoteHuman(d, 1, "remote-human-a");
        const std::size_t viewer = A::addRemoteHuman(d, 1, "remote-viewer",
            { Sync::Scope::Observe, Sync::Scope::Drive }, Sync::Role::Viewer);
        CHECK(driver != static_cast<std::size_t>(-1) && viewer != static_cast<std::size_t>(-1));
        if (driver != static_cast<std::size_t>(-1) && viewer != static_cast<std::size_t>(-1)) {
            CHECK(A::grantKey(d, driver) != A::grantKey(d, viewer));
            A::stampDriver(d, s, driver);
            A::grantCapability(d, driver, Ubiquitty::Capability::DesktopNotification);
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));

            A::service(d, s);

            CHECK(s.term->currentCapabilityOrigin == Ubiquitty::CapabilityOrigin::Remote);
            CHECK(s.capabilityPolicy.currentIdentity() == A::grantKey(d, driver));
            CHECK(Ubiquitty::effectAllowed(*s.term, Ubiquitty::Capability::DesktopNotification));
        }
    }

    // ---- 8b. The second null: an agent-driven session whose driver the daemon
    // could not identify queues nothing. A prompt naming nobody cannot be
    // answered meaningfully, and a grant recorded against an empty identity
    // would be held by every unidentified principal at once.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Agent);
        const std::size_t human = A::addHuman(d, 1);
        A::addAgent(d, 1, tokenPath, token); // attached, but nothing has stamped a driving identity
        stream->pushInbound(reprogramsAKey);

        A::service(d, s);

        CHECK(A::consentRequests(d, human).empty());
        CHECK(A::pendingGrantCount(d) == 0);
    }

    // ---- 9. Mouse tracking left on by a job that died without turning it off
    // is cleared once the shell is back in the foreground. Without this a
    // dropped ssh inside tmux leaves the terminal reporting every click to a
    // shell that never asked, and the person has to type the reset themselves.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Local);

        // The job enables tracking and is the foreground group while it runs.
        stream->facts.group = static_cast<Ubiquitty::PAL::ProcessID>(4242);
        stream->facts.isShell = false;
        stream->pushInbound("\033[?1000h");
        A::service(d, s);
        CHECK(Ubiquitty::Mouse::tracksPointer(s.term.get()));
        CHECK(s.mouseTrackingProcessGroup == 4242);

        // It dies without a DECRST and the shell is the foreground group again.
        stream->facts.group = static_cast<Ubiquitty::PAL::ProcessID>(99);
        stream->facts.isShell = true;
        stream->pushInbound("some shell output\r\n");
        A::service(d, s);

        CHECK(!Ubiquitty::Mouse::tracksPointer(s.term.get()));
    }

    // ---- 10. The null for 9: a foreground group that has not changed keeps
    // its tracking. Clearing on every turn would break the mouse for every
    // program that legitimately asked for it.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Local);
        stream->facts.group = static_cast<Ubiquitty::PAL::ProcessID>(4242);
        stream->facts.isShell = false;
        stream->pushInbound("\033[?1000h");
        A::service(d, s);

        stream->pushInbound("still drawing\r\n");
        A::service(d, s);

        CHECK(Ubiquitty::Mouse::tracksPointer(s.term.get()));
        CHECK(s.mouseTrackingProcessGroup == 4242);
    }

    // ---- 10b. The other half of the null: tracking the SHELL itself turned on
    // survives, because the guard is "the group that enabled it is gone", not
    // "the shell is in the foreground". A guard reading only the second would
    // clear the mouse under an interactive shell widget on its next turn.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Local);
        stream->facts.group = static_cast<Ubiquitty::PAL::ProcessID>(4242);
        stream->facts.isShell = true; // the shell's own widget asked for the mouse
        stream->pushInbound("\033[?1000h");
        A::service(d, s);
        CHECK(Ubiquitty::Mouse::tracksPointer(s.term.get()));

        stream->pushInbound("more of the same prompt\r\n");
        A::service(d, s);

        CHECK(Ubiquitty::Mouse::tracksPointer(s.term.get()));
        CHECK(s.mouseTrackingProcessGroup == 4242);
    }

    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Local);
        stream->facts.group = static_cast<Ubiquitty::PAL::ProcessID>(4242);
        stream->facts.isShell = false;
        stream->facts.name = "mc";
        stream->pushInbound("\033]2;mc fixture\007");
        A::service(d, s);
        CHECK(s.term->titleUTF8 == "mc fixture");
        CHECK(s.term->titleOwnerProcessGroup == 4242);
        A::probeForeground(d, s);
        CHECK(s.term->titleUTF8 == "mc fixture");

        // A quiet program exit may be observed without another PTY read.
        s.term->titleChanged = false;
        s.isFrameDue = false;
        stream->facts.group = static_cast<Ubiquitty::PAL::ProcessID>(99);
        stream->facts.isShell = true;
        stream->facts.name = "-zsh";
        A::probeForeground(d, s);
        CHECK(s.term->titleUTF8.empty());
        CHECK(s.term->titleChanged);
        CHECK(s.isFrameDue);

        // A new prompt title belongs to the shell, even within the probe budget.
        stream->pushInbound("\033]2;shell fixture\007");
        A::service(d, s);
        CHECK(s.term->titleUTF8 == "shell fixture");
        CHECK(s.term->titleOwnerProcessGroup == 99);
        A::probeForeground(d, s);
        CHECK(s.term->titleUTF8 == "shell fixture");
        stream->facts.group.reset();
        A::probeForeground(d, s);
        CHECK(s.term->titleUTF8 == "shell fixture");
    }

    for (const std::string_view mode : { "\033[?1000h", "\033[?7403h", "\033[?1000;7403h" }) {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, 24, 80, &stream, Ubiquitty::CapabilityOrigin::Local);
        stream->facts.group = static_cast<Ubiquitty::PAL::ProcessID>(4242);
        stream->facts.isShell = false;
        stream->facts.name = "application";
        stream->pushInbound(std::string(mode) + "\033[?1016;7402h");
        A::service(d, s);
        CHECK(s.mouseTrackingProcessGroup == 4242);
        const auto isCaptured = [&] {
            return Ubiquitty::Mouse::tracksPointer(s.term.get()) || s.term->modes.mouseRelativePointerLock;
        };
        CHECK(isCaptured());

        // Neither the same owner nor an unresolved or non-shell successor
        // authorizes removing application input modes.
        A::probeForeground(d, s);
        CHECK(isCaptured());
        stream->facts.group.reset();
        A::probeForeground(d, s);
        CHECK(isCaptured());
        stream->facts.group = static_cast<Ubiquitty::PAL::ProcessID>(5252);
        A::probeForeground(d, s);
        CHECK(isCaptured());

        stream->facts.group = static_cast<Ubiquitty::PAL::ProcessID>(99);
        stream->facts.isShell = true;
        stream->facts.name = "-zsh";
        s.isFrameDue = false;
        A::probeForeground(d, s);
        CHECK(!isCaptured());
        CHECK(!s.term->modes.mouseRelativePointerLock);
        CHECK(!Ubiquitty::Mouse::tracksPointer(s.term.get()));
        CHECK(s.isFrameDue);
        CHECK(s.term->modes.mouseSGRPixels);
        CHECK(s.term->modes.fullscreenContent);

        stream->pushInbound(mode);
        A::service(d, s);
        CHECK(s.mouseTrackingProcessGroup == 99);
        A::probeForeground(d, s);
        CHECK(isCaptured());
    }

    if (failures == 0)
        std::printf("service_pty_turn: one turn of the pump drains what the program wrote, notices a shell "
                    "that left, clears tracking a dead job abandoned, and asks the session's humans — and "
                    "only them, once per capability — about a capability that output reached for, recording "
                    "what it asked under the id it minted\n");
    return UBIQUITTY_TEST_SUMMARY() == 0 ? 0 : 1;
}
