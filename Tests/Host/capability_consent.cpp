// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/capability_consent.cpp — the daemon side of the consent-PROMPT
// flow: a consent.respond Call for a capability question records a
// grant only from the daemon's own pending record (never the untrusted response
// payload) and only when the responder is a HUMAN on that session. The grant is
// what flips a future check() from Prompt to Allow.
//
// Driven through the current Call ingress and service with the
// pending-grant record injected, exercising the authorization branch directly:
//   - a HUMAN on the session + AllowAlways  → grant recorded
//   - an AGENT (the driving principal) response → no grant (can't grant itself)
//   - a HUMAN on a DIFFERENT session         → no grant (wrong session)
//   - a HUMAN + Deny                          → no grant
// Every path erases the pending record (answered once).
//
// verify-by-revert: drop the `!isAgentClient(...) && sessionID == pend.sessionID`
// guard and the agent / wrong-session cases start granting (they FAIL here).
//

#include "Assertions.h"

#include "Core/Protocol/Control/Consent.h"
#include "PAL/Capability.h"
#include "Platform/Host/Daemon.h"
#include "ScreenPlaneAccess.h"
#include "TrustedLocalPeer.h"

#include <fcntl.h>
#include <limits>
#include <sys/socket.h>

#include <algorithm>
#include <cstdio>
#include <string>

using namespace Ubiquitty;
using namespace Ubiquitty::Host;

namespace Ubiquitty {
namespace Host {

// Friend of Daemon (declared in Daemon.h) — must live in Ubiquitty::Host so the
// unqualified `friend struct CapabilityConsentAccess;` names this type.
struct CapabilityConsentAccess {
    static void ensureSession(Daemon& d, std::uint32_t sessionID)
    {
        if (d.findSession(sessionID))
            return;
        CHECK(sessionID >= d.m_nextSessionID);
        d.m_nextSessionID = sessionID;
        CHECK(ScreenPlaneAccess::makeSession(d, 24, 80).id == sessionID);
    }

    // A verified-local HUMAN (originOf → Local: bundled binary basename, no
    // agentLabel) attached to `sessionID`, presence-capable so it can rule.
    static std::size_t addHuman(Daemon& d, std::uint32_t sessionID)
    {
        ensureSession(d, sessionID);
        int fds[2];
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.fileDescriptor = PAL::UnixFileDescriptor(fds[0]);
        Ubiquitty::Host::seedTrustedLocalPeer(c);
        // The session is the target its greeting names. Attached, subscribed
        // and able to rule is what the daemon's admission of that greeting
        // makes it, so the seed sets none of it.
        c.sessionID = sessionID;
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Human, Protocol::Sync::Role::Viewer);
        CHECK(d.hasLiveStructureAttachment(d.m_clients[index]));
        return index;
    }

    // A semi-trusted AGENT (agentLabel set → originOf Agent) on `sessionID`.
    // An agent observes a session only with a token carrying Observe, and is
    // admitted to it only once a person allows it; this one was allowed once.
    static std::size_t addAgent(Daemon& d, std::uint32_t sessionID)
    {
        ensureSession(d, sessionID);
        int fds[2];
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.fileDescriptor = PAL::UnixFileDescriptor(fds[0]);
        c.peerProcess = "/usr/bin/agent-mcp";
        c.agentLabel = "test-agent"; // labeled ⇒ Agent
        c.hasToken = true;
        c.tokenID = "capability-consent-agent";
        c.tokenScope = { Protocol::Sync::Scope::Observe };
        c.sessionID = sessionID;
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetAllowedAgent(d, *d.findSession(sessionID), index);
        CHECK(d.hasLiveStructureAttachment(d.m_clients[index]));
        return index;
    }

    // Raise a grant the way the policy does, then drain the queue the way the
    // session does, so the recipient rule is exercised rather than described.
    static bool makeSession(Daemon& d)
    {
        session = &ScreenPlaneAccess::makeSession(d, 24, 80);
        return session != nullptr;
    }
    static std::uint32_t sessionID() { return session ? session->id : 0; }
    static void raisePending(Daemon& d, Capability capability, CapabilityOrigin origin, const std::string& identity)
    {
        (void)d.m_consentQueue.recordPending(capability, origin, identity);
    }
    static void sendGrantRequests(Daemon& d) { d.sendCapabilityGrantRequests(*session); }
    static Daemon::Session* session;
    static bool wasAsked(const Daemon& d, std::size_t index)
    {
        const auto clientID = d.m_clients[index].clientID;
        return std::ranges::any_of(d.m_pendingCapabilityGrants, [&](const auto& entry) {
            return std::ranges::find(entry.second.recipients, clientID, &Daemon::ConsentRecipient::clientID) != entry.second.recipients.end();
        });
    }

    // Whether this client would be asked to rule, and whether its answer counts:
    // the ask and the answer read one rule, so the test reads it too.
    static bool mayRule(const Daemon& d, std::size_t index, std::uint32_t sessionID) { return d.mayRuleOnConsentFor(d.m_clients[index], sessionID); }

    static void injectPending(Daemon& d, std::uint32_t requestID, Capability capability, CapabilityOrigin origin, const std::string& identity, std::uint32_t sessionID)
    {
        auto& pending = d.m_pendingCapabilityGrants[requestID];
        pending.capability = capability;
        pending.origin = origin;
        pending.identity = identity;
        pending.sessionID = sessionID;
        pending.deadlineMilliseconds = std::numeric_limits<std::uint64_t>::max(); // asked, and not yet due
        // Model a question already admitted to these exact current recipients.
        for (const auto& candidate : d.m_clients)
            if (d.mayRuleOnConsentFor(candidate, sessionID))
                pending.recipients.push_back({ candidate.clientID, sessionID });
    }

    static void driveConsentResponse(Daemon& d, std::size_t index, std::uint32_t requestID, Protocol::Sync::ConsentChoice decision)
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

    static bool granted(const Daemon& d, Capability capability, CapabilityOrigin origin, const std::string& identity) { return d.m_grantStore.isGranted(capability, origin, identity); }

    static std::size_t pendingCount(const Daemon& d) { return d.m_pendingCapabilityGrants.size(); }
    static void cancelClipboardWrites(Daemon& d, std::uint32_t clientID) { d.cancelPendingClipboardWrites(clientID); }
};
Daemon::Session* CapabilityConsentAccess::session = nullptr;

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::CapabilityConsentAccess;

int main()
{
    constexpr std::uint32_t sessionID = 100;
    const CapabilityOrigin origin = CapabilityOrigin::Remote;

    // Clipboard endpoint cleanup cannot consume a generic capability question,
    // including when the departing caller has no assigned client identity.
    {
        Daemon d;
        CapabilityConsentAccess::injectPending(d, 41, Capability::SetColor, origin, "agent-ident", sessionID);
        CapabilityConsentAccess::cancelClipboardWrites(d, 0);
        CHECK(CapabilityConsentAccess::pendingCount(d) == 1);
        CapabilityConsentAccess::cancelClipboardWrites(d, 1);
        CHECK(CapabilityConsentAccess::pendingCount(d) == 1);
    }

    // (1) A human on the session + AllowAlways → the grant is recorded, and the
    //     pending record is consumed.
    {
        Daemon d;
        const std::size_t human = CapabilityConsentAccess::addHuman(d, sessionID);
        CapabilityConsentAccess::injectPending(d, 42, Capability::SetColor, origin, "agent-ident", sessionID);
        CapabilityConsentAccess::driveConsentResponse(d, human, 42, Protocol::Sync::ConsentChoice::AllowAlways);
        CHECK(CapabilityConsentAccess::granted(d, Capability::SetColor, origin, "agent-ident"));
        CHECK(CapabilityConsentAccess::pendingCount(d) == 0);
    }

    // (2) An unauthorized responder (the driving agent itself) must not
    //     grant and must not consume the pending record — else it could spray
    //     guessable (sequential) requestIDs to griefing-consume the human's grant
    //     before they rule. Prove the record SURVIVES the agent response, then a
    //     human on the session still grants it.
    {
        Daemon d;
        const std::size_t agent = CapabilityConsentAccess::addAgent(d, sessionID);
        const std::size_t human = CapabilityConsentAccess::addHuman(d, sessionID);
        CapabilityConsentAccess::injectPending(d, 43, Capability::SetHyperlink, origin, "agent-ident", sessionID);
        CapabilityConsentAccess::driveConsentResponse(d, agent, 43, Protocol::Sync::ConsentChoice::AllowAlways);
        CHECK(!CapabilityConsentAccess::granted(d, Capability::SetHyperlink, origin, "agent-ident")); // agent can't grant
        CHECK(CapabilityConsentAccess::pendingCount(d) == 1); // the agent did not consume it
        // The authorized human then rules — the grant is still available to give.
        CapabilityConsentAccess::driveConsentResponse(d, human, 43, Protocol::Sync::ConsentChoice::AllowAlways);
        CHECK(CapabilityConsentAccess::granted(d, Capability::SetHyperlink, origin, "agent-ident"));
        CHECK(CapabilityConsentAccess::pendingCount(d) == 0); // consumed by the authorized human
    }

    // (3) A human on a DIFFERENT session cannot grant — and (like the agent)
    //     must not consume this session's pending record.
    {
        Daemon d;
        const std::size_t otherHuman = CapabilityConsentAccess::addHuman(d, sessionID + 1);
        CapabilityConsentAccess::injectPending(d, 44, Capability::SetColor, origin, "agent-ident", sessionID);
        CapabilityConsentAccess::driveConsentResponse(d, otherHuman, 44, Protocol::Sync::ConsentChoice::AllowAlways);
        CHECK(!CapabilityConsentAccess::granted(d, Capability::SetColor, origin, "agent-ident"));
        CHECK(CapabilityConsentAccess::pendingCount(d) == 1); // wrong-session responder didn't consume it
    }

    // (4) A human on the session but a DENY decision → no grant.
    {
        Daemon d;
        const std::size_t human = CapabilityConsentAccess::addHuman(d, sessionID);
        CapabilityConsentAccess::injectPending(d, 45, Capability::SetCursorShape, origin, "agent-ident", sessionID);
        CapabilityConsentAccess::driveConsentResponse(d, human, 45, Protocol::Sync::ConsentChoice::Deny);
        CHECK(!CapabilityConsentAccess::granted(d, Capability::SetCursorShape, origin, "agent-ident"));
        CHECK(CapabilityConsentAccess::pendingCount(d) == 0);
    }

    // A human on the session is entitled to rule and must therefore be asked. The ask and the answer read one rule: an ask narrower than its
    // answer leaves this client able to grant and never offered the chance, and
    // a capability grant is recorded as pending when raised and never offered
    // twice — so a prompt nobody receives is a grant nobody can ever give.
    {
        Daemon d;
        const std::uint32_t sessionID = 7;
        const std::size_t human = CapabilityConsentAccess::addHuman(d, sessionID);
        CHECK(CapabilityConsentAccess::mayRule(d, human, sessionID));

        const auto origin = CapabilityOrigin::Local;
        CapabilityConsentAccess::injectPending(d, 51, Capability::SetCursorShape, origin, "agent-ident", sessionID);
        CapabilityConsentAccess::driveConsentResponse(d, human, 51, Protocol::Sync::ConsentChoice::AllowOnce);
        CHECK(CapabilityConsentAccess::granted(d, Capability::SetCursorShape, origin, "agent-ident"));
        CHECK(CapabilityConsentAccess::pendingCount(d) == 0);

        // An agent on the same session still rules on nothing.
        const std::size_t agent = CapabilityConsentAccess::addAgent(d, sessionID);
        CHECK(!CapabilityConsentAccess::mayRule(d, agent, sessionID));
        // Nor does a human attached elsewhere.
        const std::size_t elsewhere = CapabilityConsentAccess::addHuman(d, sessionID + 1);
        CHECK(!CapabilityConsentAccess::mayRule(d, elsewhere, sessionID));
    }

    // The ask reaches that client too. A grant raised by the policy and drained
    // the way a session drains it queues a request to every ruler.
    {
        Daemon d;
        const bool didMakeSession = CapabilityConsentAccess::makeSession(d);
        CHECK(didMakeSession);
        if (didMakeSession) {
            const std::uint32_t sid = CapabilityConsentAccess::sessionID();
            const std::size_t human = CapabilityConsentAccess::addHuman(d, sid);
            const std::size_t agent = CapabilityConsentAccess::addAgent(d, sid);

            CapabilityConsentAccess::raisePending(d, Capability::SetCursorShape, CapabilityOrigin::Local, "agent-ident");
            CapabilityConsentAccess::sendGrantRequests(d);
            CHECK_MSG(CapabilityConsentAccess::wasAsked(d, human),
                "a human on the session was not asked to rule on a capability grant");
            CHECK_MSG(!CapabilityConsentAccess::wasAsked(d, agent),
                "an agent was asked to rule on a capability grant");
        }
    }

    if (failures == 0)
        printf("capability_consent: PASSED — a capability grant is recorded ONLY from the "
               "daemon's own pending record and ONLY by a human on that session (agent / "
               "wrong-session / deny all refused); an unauthorized responder does NOT consume "
               "the pending record, so the human's grant survives a griefing spray\n");
    else
        printf("capability_consent: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
