// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/revoke_disconnect.cpp — revoke must disconnect the live agent.
//
// Erasing a grant (StopAlways or SessionGrant) without kicking the live agent leaves
// it attached and acting: the human revoked consent but the agent keeps its session
// because the grant check runs only at ATTACH and per-op, not per-frame. So each
// grant clear is followed immediately by a reverse kick loop. This negative proves
// both paths under a friend seam.
//
// Setup per path:
//   StopAlways   — agent's grantKey in m_alwaysAllowedAgents (persisted always-allow);
//                  agentHasStandingGrant() true before revoke.
//   SessionGrant — s->agentSessionGrant=true + s->agentSessionGrantIdentity = agent's
//                  grantKey; agentHasStandingGrant() true before revoke.
//
// Drive: build a canonical consent.revoke Call for each action, feed its typed
// frame through the human's current connection, then run the production Call
// service. Peer identity and tokens are seeded; each client then attaches by its
// canonical greeting, the agent under the grant the path revokes.
//
// The human client uses a real socketpair so broadcastPresence's flushClient write
// succeeds (fd -1 would EBADF → dropClient → wrongly overflow the human).
//
// Assert: the agent client is kicked after revoke — an attached client finishes
// its stream and leaves on a deadline, one that never greeted is dropped at once —
// and the human client is neither.
//
// The agent-kicked assertions specifically pin the kick loops after the
// StopAlways and SessionGrant authority is withdrawn.
//
// Exit 0 on success.
//

#include "Assertions.h"
#include "FullCarrier.h"

#include "Core/Protocol/Control/Consent.h"
#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"
#include "ScreenPlaneAccess.h"
#include "TrustedLocalPeer.h"

#include <cassert>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {
std::string revokeScratchDirectory;
}

namespace Ubiquitty {
namespace Host {

// Friend seam: access the minimal private Daemon state needed to construct
// the test fixture and observe the post-revoke client table.
struct RevokeDCAccess {
    // Add a trusted human client with an actual current serving attachment.
    // The human gets a real socketpair
    // write-end so broadcastPresence's flushClient write succeeds (fd=-1 would
    // EBADF → dropClient → wrongly overflow the human).
    // Returns {humanIndex, peerFileDescriptor} — caller must close peerFileDescriptor.
    static std::pair<std::size_t, int> addHumanClient(Daemon& d, std::uint32_t sessionID)
    {
        int fileDescriptors[2];
        const auto paired = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fileDescriptors);
        CHECK(paired == 0);
        if (paired != 0)
            std::abort();
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.fileDescriptor = PAL::UnixFileDescriptor(fileDescriptors[0]); // daemon writes to this end; ownership transfers to the Client
        // The session is the target its greeting names; admission attaches
        // and subscribes it.
        c.sessionID = sessionID;
        // The trusted Local attach bridge (isAgentClient derives from
        // originOf, and origin Local requires exactly this peerProcess) — anything
        // else now classifies Agent, and the consent.revoke entry gate
        // (human-only) would reject this client outright.
        Ubiquitty::Host::seedTrustedLocalPeer(c);
        c.peerProcessID = 1000;
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Human, Protocol::Sync::Role::Viewer);
        CHECK(d.hasLiveStructureAttachment(d.m_clients[index]));
        return { index, fileDescriptors[1] };
    }

    // Add an agent client (hasToken + tokenID) that rides the standing grant the
    // caller seeded for its key: the daemon admits it without asking anyone.
    // isAgentClient() returns true for an unverified token bearer, and
    // grantKeyFor() returns "token:<tokenID>". The carrier holds what the
    // daemon writes, so only the revoke can mark this client.
    static std::size_t addAgentClient(Daemon& d, std::uint32_t sessionID, const std::string& tokenID, Ubiquitty::HostTest::FullCarrier& carrier)
    {
        CHECK(carrier.isReady());
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.fileDescriptor = carrier.takeDaemonEnd();
        c.sessionID = sessionID;
        c.hasToken = true;
        c.tokenID = tokenID;
        c.tokenScope = { Protocol::Sync::Scope::Observe };
        c.agentLabel = "test-agent";
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Agent, Protocol::Sync::Role::Agent);
        CHECK(d.hasLiveStructureAttachment(d.m_clients[index]));
        return index;
    }

    // A TOKENLESS agent with no verified peerProcess — grantKeyFor() falls back to
    // peerProcess (empty here), so this client's grant key is "". isAgentClient()
    // still reads true via its unverified origin/automation label. Such a peer
    // has no Observe scope and the daemon refuses its greeting, so it is still
    // connected with its session selected and not greeted: the one place an
    // empty grant key sits beside a session. Any client whose grantKeyFor() is ""
    // would wrongly match a revoke naming the empty identity of a grant that was
    // never active.
    static std::size_t addTokenlessAgentClient(Daemon& d, std::uint32_t sessionID, Ubiquitty::HostTest::FullCarrier& carrier)
    {
        CHECK(carrier.isReady());
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.fileDescriptor = carrier.takeDaemonEnd();
        c.sessionID = sessionID;
        c.agentLabel = "tokenless-agent";
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    // Seed the StopAlways persisted always-allow grant.
    static void grantAlwaysAllow(Daemon& d, const std::string& grantKey) { d.m_alwaysAllowedAgents.push_back(grantKey); }

    // Seed the SessionGrant on the given session.
    static void grantSessionGrant(Daemon& d, std::uint32_t sessionID, const std::string& grantKey)
    {
        Daemon::Session* s = d.findSession(sessionID);
        if (!s)
            return;
        (void)s->addSessionGrant(grantKey);
    }

    // Verify agentHasStandingGrant for the agent client.
    static bool hasStandingGrant(const Daemon& d, std::size_t agentIndex)
    {
        const Daemon::Client& agent = d.m_clients[agentIndex];
        const Daemon::Session* s = const_cast<Daemon&>(d).findSession(agent.sessionID);
        if (!s)
            return false;
        return d.agentHasStandingGrant(agent, *s);
    }

    // A real terminal and PaneSource over a closed stream, never a spawned PTY.
    static std::uint32_t makeSession(Daemon& d)
    {
        // Revocation persists grants beside this fixture-owned socket path,
        // never in the caller's working directory or a live host's store.
        d.m_socketPath = revokeScratchDirectory + "/daemon.sock";
        return ScreenPlaneAccess::makeSession(d, 2, 4).id;
    }

    static void fireCall(Daemon& d, std::size_t humanIndex, std::string text)
    {
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Call { std::move(text) });
        std::vector<std::uint8_t> stream;
        CHECK(Protocol::Sync::appendFrame(stream, Protocol::Sync::FrameKind::Call, 0, body));
        d.receiveScreenBytes(humanIndex, stream);
        d.serviceSyncConnection(humanIndex, ScreenPlaneAccess::now());
    }

    // Drive the canonical producer and production typed Call dispatch.
    static void fireRevoke(Daemon& d, std::size_t humanIndex, const Protocol::Control::ConsentRevocation& revocation)
    {
        const auto call = Protocol::Control::consentRevocationRequest(revocation, "fixture.revoke");
        CHECK(!call.empty());
        fireCall(d, humanIndex, call);
    }

    // The grant key a token-authed agent carries.
    static std::string grantKey(const std::string& tokenID) { return "token:" + tokenID; }

    // The capability grants the human also gave this agent — a different store
    // from the attach grants above, and the one a partial revoke leaves behind.
    static void grantCapability(Daemon& d, Ubiquitty::Capability capability, const std::string& identity, Ubiquitty::CapabilityGrantScope scope, std::uint32_t sessionID = 0)
    {
        d.m_grantStore.grant(capability, Ubiquitty::CapabilityOrigin::Agent, identity, scope, sessionID);
    }
    static bool hasCapability(const Daemon& d, Ubiquitty::Capability capability, const std::string& identity)
    {
        return d.m_grantStore.isGranted(capability, Ubiquitty::CapabilityOrigin::Agent, identity);
    }

    // Kicked: dropped at once, or finishing its stream and leaving.
    static bool wasKicked(const Daemon& d, std::size_t i) { return d.m_clients[i].didOverflow || d.m_clients[i].sessionExitDeadlineMilliseconds != 0; }
    static std::size_t clientCount(const Daemon& d) { return d.m_clients.size(); }
    static bool alwaysAllowEmpty(const Daemon& d) { return d.m_alwaysAllowedAgents.empty(); }

    // Close the daemon-side fd through the RAII owner before the test closes
    // the peer end via ::close(peerFileDescriptor). Without this, ~Daemon's
    // teardownAll() writes an EXIT frame to the human client's still-open fd
    // after the peer is gone, raising SIGPIPE and killing the test process.
    // (Closing through the owner avoids a double-close of the RAII-managed fd.)
    static void closeClientFileDescriptor(Daemon& d, std::size_t i) { (void)d.m_clients[i].fileDescriptor.close(); }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::RevokeDCAccess;
using Ubiquitty::Protocol::Control::ConsentRevocation;
using Ubiquitty::Protocol::Sync::ConsentRevocationKind;

// --- StopAlways path ---
// Human revokes a PERSISTED always-allow grant. The agent must be kicked.
static void testStopAlways()
{
    Daemon d;
    const std::uint32_t sessionID = RevokeDCAccess::makeSession(d);
    const std::string tokenID = "tok-abc123";
    const std::string key = RevokeDCAccess::grantKey(tokenID);

    auto [humanIndex, peerFileDescriptor] = RevokeDCAccess::addHumanClient(d, sessionID);
    // Seed the persisted always-allow grant the agent attaches under.
    RevokeDCAccess::grantAlwaysAllow(d, key);
    Ubiquitty::HostTest::FullCarrier agentCarrier;
    const std::size_t agentIndex = RevokeDCAccess::addAgentClient(d, sessionID, tokenID, agentCarrier);

    // The agent genuinely rides a standing grant before revoke.
    CHECK(RevokeDCAccess::hasStandingGrant(d, agentIndex));

    // Human names the verified grant identity observed on this exact session.
    ConsentRevocation rm {};
    rm.sessionID = sessionID;
    rm.action = ConsentRevocationKind::StopAlways;
    rm.identity = key;
    RevokeDCAccess::fireRevoke(d, humanIndex, rm);

    // Persisted grant erased.
    CHECK(RevokeDCAccess::alwaysAllowEmpty(d));
    // Agent is kicked.
    CHECK(RevokeDCAccess::wasKicked(d, agentIndex));
    // Human is not kicked.
    CHECK(!RevokeDCAccess::wasKicked(d, humanIndex));

    RevokeDCAccess::closeClientFileDescriptor(d, humanIndex);
    ::close(peerFileDescriptor);
}

// --- SessionGrant path ---
// Human revokes a per-session allow-session grant. The agent must be kicked.
static void testSessionGrant()
{
    Daemon d;
    const std::uint32_t sessionID = RevokeDCAccess::makeSession(d);
    const std::string tokenID = "tok-xyz789";
    const std::string key = RevokeDCAccess::grantKey(tokenID);

    auto [humanIndex, peerFileDescriptor] = RevokeDCAccess::addHumanClient(d, sessionID);
    // Seed the session-scoped grant the agent attaches under.
    RevokeDCAccess::grantSessionGrant(d, sessionID, key);
    Ubiquitty::HostTest::FullCarrier agentCarrier;
    const std::size_t agentIndex = RevokeDCAccess::addAgentClient(d, sessionID, tokenID, agentCarrier);

    // The agent genuinely rides a standing grant before revoke.
    CHECK(RevokeDCAccess::hasStandingGrant(d, agentIndex));

    // Human fires SessionGrant revoke.
    ConsentRevocation rm {};
    rm.sessionID = sessionID;
    rm.action = ConsentRevocationKind::SessionGrant;
    rm.identity = key;
    RevokeDCAccess::fireRevoke(d, humanIndex, rm);

    // Agent is kicked.
    CHECK(RevokeDCAccess::wasKicked(d, agentIndex));
    // Human is not kicked.
    CHECK(!RevokeDCAccess::wasKicked(d, humanIndex));

    RevokeDCAccess::closeClientFileDescriptor(d, humanIndex);
    ::close(peerFileDescriptor);
}

// --- SessionGrant, no active grant: empty-clearedIdentity over-kick guard ---
// A consent.revoke/forgetSession with an empty identity, fired when no session
// grant is active (agentSessionGrantIdentity == ""), must kick nobody — not a
// live tokenless agent (whose grantKeyFor() is also "") and not the human.
// Without the `!clearedIdentity.empty()` guard, the kick loop's empty-vs-empty
// match wrongly kicks every tokenless agent on the session even though nothing
// was actually revoked.
static void testSessionGrantNoActiveGrantKicksNobody()
{
    Daemon d;
    const std::uint32_t sessionID = RevokeDCAccess::makeSession(d);

    auto [humanIndex, peerFileDescriptor] = RevokeDCAccess::addHumanClient(d, sessionID);
    Ubiquitty::HostTest::FullCarrier agentCarrier;
    const std::size_t agentIndex = RevokeDCAccess::addTokenlessAgentClient(d, sessionID, agentCarrier);

    // No grantSessionGrant() call — agentSessionGrant stays false, identity stays "".

    // Feed independent malformed params rather than relying on the producer
    // refusing to serialize an empty selector before dispatch can see it.
    RevokeDCAccess::fireCall(d, humanIndex,
        "{\"jsonrpc\":\"2.0\",\"id\":\"fixture.empty\",\"method\":\"consent.revoke\",\"params\":{\"session\":\"session:"
            + std::to_string(sessionID) + "\",\"action\":\"forgetSession\",\"identity\":\"\"}}");
    // A syntactically valid but nonexistent exact grant must also be inert.
    ConsentRevocation absent {};
    absent.sessionID = sessionID;
    absent.action = ConsentRevocationKind::SessionGrant;
    absent.identity = "token:no-active-grant";
    RevokeDCAccess::fireRevoke(d, humanIndex, absent);

    // Nothing was revoked — the tokenless agent must not be kicked.
    CHECK(!RevokeDCAccess::wasKicked(d, agentIndex));
    CHECK(!RevokeDCAccess::wasKicked(d, humanIndex));

    RevokeDCAccess::closeClientFileDescriptor(d, humanIndex);
    ::close(peerFileDescriptor);
}

// Revoking the durable trust takes the capabilities with it. Every capability the
// human allowed this agent — clipboard, file read, a trigger's exec — is a sticky
// grant under the same identity, and an Always one is written to disk. Leaving
// them made "stop always allowing" mean only "ask me to attach again", after
// which everything the agent could already do it could do again unprompted.
static void testStopAlwaysRevokesCapabilityGrants()
{
    Daemon d;
    const std::uint32_t sessionID = RevokeDCAccess::makeSession(d);
    const std::string tokenID = "tok-caps";
    const std::string key = RevokeDCAccess::grantKey(tokenID);

    auto [humanIndex, peerFileDescriptor] = RevokeDCAccess::addHumanClient(d, sessionID);
    RevokeDCAccess::grantAlwaysAllow(d, key);
    Ubiquitty::HostTest::FullCarrier agentCarrier;
    const std::size_t agentIndex = RevokeDCAccess::addAgentClient(d, sessionID, tokenID, agentCarrier);

    // Two capabilities the human allowed, one of each durable scope.
    RevokeDCAccess::grantCapability(d, Ubiquitty::Capability::FileReadAccess, key, Ubiquitty::CapabilityGrantScope::Always);
    RevokeDCAccess::grantCapability(d, Ubiquitty::Capability::ClipboardWrite, key, Ubiquitty::CapabilityGrantScope::Session, sessionID);
    CHECK(RevokeDCAccess::hasCapability(d, Ubiquitty::Capability::FileReadAccess, key));
    CHECK(RevokeDCAccess::hasCapability(d, Ubiquitty::Capability::ClipboardWrite, key));

    ConsentRevocation rm {};
    rm.sessionID = sessionID;
    rm.action = ConsentRevocationKind::StopAlways;
    rm.identity = key;
    RevokeDCAccess::fireRevoke(d, humanIndex, rm);

    CHECK(RevokeDCAccess::wasKicked(d, agentIndex)); // still kicked
    CHECK(RevokeDCAccess::alwaysAllowEmpty(d)); // attach grant still forgotten
    // And now the capabilities are gone too, so a re-consented attach starts over.
    CHECK(!RevokeDCAccess::hasCapability(d, Ubiquitty::Capability::FileReadAccess, key));
    CHECK(!RevokeDCAccess::hasCapability(d, Ubiquitty::Capability::ClipboardWrite, key));

    RevokeDCAccess::closeClientFileDescriptor(d, humanIndex);
    ::close(peerFileDescriptor);
}

// Revoking one session takes that session's capability grants and leaves the
// durable ones, because that is the difference between the two menu items. It
// must also leave another agent's grants on the same session alone — the reason
// this revokes by (identity, session) rather than by dropping the session.
static void testSessionGrantRevokesThatSessionsCapabilitiesOnly()
{
    Daemon d;
    const std::uint32_t sessionID = RevokeDCAccess::makeSession(d);
    const std::string key = RevokeDCAccess::grantKey("tok-session-caps");
    const std::string otherKey = RevokeDCAccess::grantKey("tok-bystander");

    auto [humanIndex, peerFileDescriptor] = RevokeDCAccess::addHumanClient(d, sessionID);
    RevokeDCAccess::grantSessionGrant(d, sessionID, key);
    Ubiquitty::HostTest::FullCarrier agentCarrier;
    const std::size_t agentIndex = RevokeDCAccess::addAgentClient(d, sessionID, "tok-session-caps", agentCarrier);

    RevokeDCAccess::grantCapability(d, Ubiquitty::Capability::ClipboardWrite, key, Ubiquitty::CapabilityGrantScope::Session, sessionID);
    RevokeDCAccess::grantCapability(d, Ubiquitty::Capability::FileReadAccess, key, Ubiquitty::CapabilityGrantScope::Always);
    RevokeDCAccess::grantCapability(d, Ubiquitty::Capability::ClipboardWrite, otherKey, Ubiquitty::CapabilityGrantScope::Session, sessionID);

    ConsentRevocation rm {};
    rm.sessionID = sessionID;
    rm.action = ConsentRevocationKind::SessionGrant;
    rm.identity = key;
    RevokeDCAccess::fireRevoke(d, humanIndex, rm);

    CHECK(RevokeDCAccess::wasKicked(d, agentIndex));
    // This session's grant for this agent is gone.
    CHECK(!RevokeDCAccess::hasCapability(d, Ubiquitty::Capability::ClipboardWrite, key));
    // Its Always grant is not: withdrawing a session is not withdrawing the agent.
    CHECK(RevokeDCAccess::hasCapability(d, Ubiquitty::Capability::FileReadAccess, key));
    // And a bystander on the same session keeps what it was granted.
    CHECK(RevokeDCAccess::hasCapability(d, Ubiquitty::Capability::ClipboardWrite, otherKey));

    RevokeDCAccess::closeClientFileDescriptor(d, humanIndex);
    ::close(peerFileDescriptor);
}

int main()
{
    char directoryTemplate[] = "/tmp/ubq-revoke-disconnect-XXXXXX";
    const auto* directory = ::mkdtemp(directoryTemplate);
    if (!directory) {
        std::fprintf(stderr, "revoke_disconnect: could not create scratch directory\n");
        return 1;
    }
    revokeScratchDirectory = directory;
    // The daemon ignores SIGPIPE (main.cpp does the same): a write to a client
    // whose peer end is gone must surface as EPIPE, not kill the test process.
    ::signal(SIGPIPE, SIG_IGN);
    testStopAlways();
    testSessionGrant();
    testSessionGrantNoActiveGrantKicksNobody();
    testStopAlwaysRevokesCapabilityGrants();
    testSessionGrantRevokesThatSessionsCapabilitiesOnly();

    ::unlink((revokeScratchDirectory + "/consent").c_str());
    ::unlink((revokeScratchDirectory + "/capability_grants").c_str());
    ::unlink((revokeScratchDirectory + "/capability_grants.lock").c_str());
    ::rmdir(revokeScratchDirectory.c_str());

    if (failures == 0)
        std::printf("revoke_disconnect: StopAlways and SessionGrant both kick the live agent "
                    "while the human client is untouched; a "
                    "no-active-grant revoke kicks nobody; and both revokes clear the\n"
                    "capability grants that outlived them\n");
    return failures == 0 ? 0 : 1;
}
