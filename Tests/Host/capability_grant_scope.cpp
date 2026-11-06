// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/capability_grant_scope.cpp — capability-grant lifetime model:
// OneShot, Session, Always scopes; tier gate; token-only persistence.
//
// Each test is a POSITIVE assertion + a verify-by-revert description:
//
// (1) OneShot consume: a OneShot grant allows exactly one check(), then is
//     consumed. The second check() re-prompts (Prompt returned, queue armed).
//     Revert: remove the erase in checkAndConsumeIfGranted — the grant is
//     never consumed, the second check still returns Allow → the second-check-
//     is-Prompt assertion fails.
//
// (2) Tier gate — HostEscaping + AllowAlways → OneShot: the handler records
//     OneShot regardless of the human's requested scope. Prove by: first check
//     → Allow, second check → Prompt (consumed). Revert: remove the tier gate
//     (the `if (tier == HostEscaping || tier == ReadExfil)` force) — the
//     handler records Always, the grant is never consumed, the second check
//     still returns Allow → the second-check-is-Prompt assertion fails.
//
// (3) LowRiskInfo + AllowAlways:
//     (a) token identity → Always scope, persisted to disk; a fresh Daemon
//         loading from that path finds the grant still in force.
//         Revert: remove the persistence call (editAlwaysAllowedCapabilityGrants /
//         the `scope == Always` branch) — the file is never written, the reload
//         finds no grant → the post-reload-isGranted assertion fails.
//     (b) non-token identity → degrades to Session; file not written.
//         Revert: remove the `!starts_with("token:")` degrade in the handler —
//         the non-token identity is recorded as Always and persisted → the
//         not-in-file assertion fails.
//
// (4) Session grant dropped after dropSession(sessionID): a Session grant for
//     session S is gone once dropSession(S) fires; a concurrent grant for a
//     different session is unaffected.
//     Revert: remove the dropSession() call from destroySession() — the grant
//     survives teardown → the post-drop-isGranted assertion fails.
//
// Driven through the current consent.respond Call ingress and service with the
// pending-grant record injected, using the CapabilityGrantScopeAccess
// friend seam declared in Daemon.h.
//

#include "Assertions.h"

#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Sync/HostManagement.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "PAL/Capability.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostCapabilityPolicy.h"
#include "Platform/Host/TokenStore.h"
#include "ScreenPlaneAccess.h"
#include "TrustedLocalPeer.h"

#include <fcntl.h>
#include <limits>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace Ubiquitty;
using namespace Ubiquitty::Host;

// ---------------------------------------------------------------------------
// Friend seam — must live in Ubiquitty::Host.
// ---------------------------------------------------------------------------

namespace Ubiquitty {
namespace Host {

struct CapabilityGrantScopeAccess {
    static void ensureSession(Daemon& d, std::uint32_t sessionID)
    {
        if (d.findSession(sessionID))
            return;
        CHECK(sessionID >= d.m_nextSessionID);
        d.m_nextSessionID = sessionID;
        CHECK(ScreenPlaneAccess::makeSession(d, 24, 80).id == sessionID);
    }

    // Local human client on `sessionID` — originOf → Local, presence-capable.
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
        c.sessionID = sessionID;
        c.paneSubscriptions = { { .sessionID = sessionID } };
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Human, Protocol::Sync::Role::Viewer);
        CHECK(d.hasLiveStructureAttachment(d.m_clients[index]));
        return index;
    }

    // Token-bound agent on `sessionID`. grantKeyFor → "token:<tokenID>".
    static std::size_t addTokenAgent(Daemon& d, std::uint32_t sessionID, const std::string& tokenID)
    {
        ensureSession(d, sessionID);
        int fds[2];
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
        Daemon::Client c {};
        c.clientID = d.m_nextClientID++;
        c.fileDescriptor = PAL::UnixFileDescriptor(fds[0]);
        c.peerProcess = "/usr/bin/ubq";
        c.agentLabel = "test-agent";
        c.hasToken = true;
        c.tokenID = tokenID;
        c.tokenScope = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive };
        c.sessionID = sessionID;
        c.paneSubscriptions = { { .sessionID = sessionID } };
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        // An agent opens a session, never a workspace, and is admitted without
        // a question only on a standing grant: the one a person's
        // allow-for-this-session answer gives.
        ScreenPlaneAccess::grantAgentSession(d, *d.findSession(sessionID), index);
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Agent, Protocol::Sync::Role::Agent);
        CHECK(d.hasLiveStructureAttachment(d.m_clients[index]));
        return index;
    }

    static void injectPending(Daemon& d, std::uint32_t requestID, Capability capability, CapabilityOrigin origin, const std::string& identity, std::uint32_t sessionID)
    {
        auto& pending = d.m_pendingCapabilityGrants[requestID];
        pending.capability = capability;
        pending.origin = origin;
        pending.identity = identity;
        pending.sessionID = sessionID;
        pending.deadlineMilliseconds = std::numeric_limits<std::uint64_t>::max(); // asked, and not yet due
        // The injected record represents an already delivered question.
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

    static Protocol::Sync::ManagementStatus revokeManagedGrantStatus(Daemon& daemon, std::size_t index, Capability capability, CapabilityOrigin origin, const std::string& identity)
    {
        using namespace Protocol::Sync;
        daemon.m_clients[index].intents.add(HelloIntent::RequestsManagement);
        ManagementRequest request;
        request.requestID = 1;
        request.daemonInstanceID = daemon.m_daemonInstanceID;
        request.operation = ManagementOperation::RevokeGrant;
        request.identity = "capability:" + std::to_string(std::to_underlying(capability)) + ":" + std::to_string(std::to_underlying(origin)) + ":" + identity;
        // This fixture owns grant lifetime, not transport admission. Exercise
        // the same typed operation body without manufacturing a legacy frame.
        Daemon::AdministrationEffects effects;
        return daemon.performHostAdministration(daemon.m_clients[index], request, effects).status;
    }

    static void revokeManagedGrant(Daemon& daemon, std::size_t index, Capability capability, CapabilityOrigin origin, const std::string& identity)
    {
        CHECK(revokeManagedGrantStatus(daemon, index, capability, origin, identity) == Protocol::Sync::ManagementStatus::Success);
    }

    static bool isGranted(const Daemon& d, Capability capability, CapabilityOrigin origin, const std::string& identity) { return d.m_grantStore.isGranted(capability, origin, identity); }

    static void injectGrant(Daemon& d, Capability capability, CapabilityOrigin origin, const std::string& identity, CapabilityGrantScope scope, std::uint32_t sessionID) { d.m_grantStore.grant(capability, origin, identity, scope, sessionID); }

    // Run a single check() through a fresh per-session policy tied to the
    // daemon's store + consent queue at the given identity.
    static CapabilityDecision policyCheck(Daemon& d, Capability capability, CapabilityOrigin origin, const std::string& identity)
    {
        HostCapabilityPolicy policy;
        policy.setGrantStore(&d.m_grantStore);
        policy.setConsentQueue(&d.m_consentQueue);
        policy.setCurrentIdentity(identity);
        return policy.check(capability, origin, "");
    }

    static bool isConsentPending(const Daemon& d, Capability capability, CapabilityOrigin origin, const std::string& identity) { return d.m_consentQueue.isPending(capability, origin, identity); }

    static void setSocketPath(Daemon& d, const std::string& path) { d.m_socketPath = path; }

    static void loadAlwaysAllowedCapabilityGrants(Daemon& d) { d.loadAlwaysAllowedCapabilityGrants(); }

    static void dropSession(Daemon& d, std::uint32_t sessionID) { d.m_grantStore.dropSession(sessionID); }

    static std::size_t sessionGrantCount(const Daemon& d, std::uint32_t sessionID) { return d.m_grantStore.sessionGrantCount(sessionID); }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::CapabilityGrantScopeAccess;

// ---------------------------------------------------------------------------
// Helper: make a unique temp directory for the persistence tests and return
// its path. Returns "" on failure.
// ---------------------------------------------------------------------------
static std::string makeTempDirectory()
{
    char tmpl[] = "/tmp/ubiquitty-scope-test-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    return dir ? std::string(dir) : std::string();
}

static void removeTempDirectory(const std::string& dir)
{
    // Best-effort cleanup of the grant file, its lock, and the directory the
    // persistence tests write.
    if (dir.empty())
        return;
    ::unlink((dir + "/capability_grants").c_str());
    ::unlink((dir + "/capability_grants.lock").c_str());
    ::rmdir(dir.c_str());
}

// ---------------------------------------------------------------------------
// Test (1) — OneShot consume
// ---------------------------------------------------------------------------
static void testOneShotConsume()
{
    constexpr CapabilityOrigin origin = CapabilityOrigin::Agent;
    constexpr Capability capability = Capability::SetWorkingDirectory; // LowRiskInfo
    const std::string identity = "token:oneshottestid";

    Daemon d;
    CapabilityGrantScopeAccess::injectGrant(d, capability, origin, identity, CapabilityGrantScope::OneShot, 0);

    // First check: grant exists → Allow, and OneShot is consumed.
    CHECK(CapabilityGrantScopeAccess::policyCheck(d, capability, origin, identity) == CapabilityDecision::Allow);

    // Second check: grant consumed → re-prompts. LowRiskInfo defaults to Allow
    // for Agent, so a consumed grant there falls back to Allow and shows
    // nothing; test the re-prompt with HostEscaping at Agent (default = Prompt).
    CapabilityGrantScopeAccess::injectGrant(d, Capability::ClipboardWrite, origin, identity, CapabilityGrantScope::OneShot, 0);
    CHECK(CapabilityGrantScopeAccess::policyCheck(d, Capability::ClipboardWrite, origin, identity) == CapabilityDecision::Allow); // first use: consumed
    CHECK(CapabilityGrantScopeAccess::policyCheck(d, Capability::ClipboardWrite, origin, identity) == CapabilityDecision::Prompt); // second use: no grant → Prompt (HostEscaping + Agent)
    // The consent queue must now hold the pending re-prompt.
    CHECK(CapabilityGrantScopeAccess::isConsentPending(d, Capability::ClipboardWrite, origin, identity));
}

// ---------------------------------------------------------------------------
// Test (2) — Tier gate: HostEscaping + AllowAlways → forced OneShot
// ---------------------------------------------------------------------------
static void testTierGateHostEscaping()
{
    constexpr std::uint32_t sessionID = 200;
    constexpr CapabilityOrigin origin = CapabilityOrigin::Agent;
    // ClipboardWrite is HostEscaping — top tier, must be forced to OneShot.
    constexpr Capability capability = Capability::ClipboardWrite;
    const std::string tokenID = "clipboardtesttoken";
    const std::string identity = "token:" + tokenID;

    Daemon d;
    const std::size_t human = CapabilityGrantScopeAccess::addHuman(d, sessionID);
    CapabilityGrantScopeAccess::addTokenAgent(d, sessionID, tokenID);
    // Inject pending with the token agent's identity.
    CapabilityGrantScopeAccess::injectPending(d, 42, capability, origin, identity, sessionID);
    // Human responds with AllowAlways — tier gate must force this to OneShot.
    CapabilityGrantScopeAccess::driveConsentResponse(d, human, 42, Protocol::Sync::ConsentChoice::AllowAlways);

    // The grant is in the store (isGranted = true).
    CHECK(CapabilityGrantScopeAccess::isGranted(d, capability, origin, identity));

    // First check: Allow (grant in store, OneShot not yet consumed).
    CHECK(CapabilityGrantScopeAccess::policyCheck(d, capability, origin, identity) == CapabilityDecision::Allow);
    // Second check: grant was OneShot → consumed → Prompt (HostEscaping + Agent).
    CHECK(CapabilityGrantScopeAccess::policyCheck(d, capability, origin, identity) == CapabilityDecision::Prompt);
}

// ---------------------------------------------------------------------------
// Test (3a) — LowRiskInfo + AllowAlways + token identity → persists + reloads
// ---------------------------------------------------------------------------
static void testAlwaysPersistenceToken()
{
    constexpr std::uint32_t sessionID = 300;
    constexpr CapabilityOrigin origin = CapabilityOrigin::Agent;
    constexpr Capability capability = Capability::SetWorkingDirectory; // LowRiskInfo
    const std::string tokenID = "persisttesttokenid";
    const std::string identity = "token:" + tokenID;

    const std::string tempDir = makeTempDirectory();
    if (tempDir.empty()) {
        CHECK_MSG(false, "testAlwaysPersistenceToken fixture: mkdtemp failed");
        return;
    }
    // Socket path just needs the right directory; no real socket is created.
    const std::string socketPath = tempDir + "/test.sock";

    // ---- Daemon A: record the grant and persist it. ----
    {
        Daemon d;
        CapabilityGrantScopeAccess::setSocketPath(d, socketPath);
        const std::size_t human = CapabilityGrantScopeAccess::addHuman(d, sessionID);
        CapabilityGrantScopeAccess::addTokenAgent(d, sessionID, tokenID);
        CapabilityGrantScopeAccess::injectPending(d, 43, capability, origin, identity, sessionID);
        CapabilityGrantScopeAccess::driveConsentResponse(d, human, 43, Protocol::Sync::ConsentChoice::AllowAlways);
        CHECK(CapabilityGrantScopeAccess::isGranted(d, capability, origin, identity));
    }

    // Verify the file was actually written with a token-bound entry.
    const std::string grantsFile = tempDir + "/capability_grants";
    FILE* f = std::fopen(grantsFile.c_str(), "r");
    CHECK(f != nullptr);
    if (f) {
        char line[4096] = {};
        bool found = false;
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            if (std::string_view(line).find("token:") != std::string_view::npos)
                found = true;
        }
        std::fclose(f);
        CHECK(found); // the persisted file contains the token identity
    }

    // ---- Daemon B: fresh load from the same control directory. ----
    {
        Daemon d2;
        CapabilityGrantScopeAccess::setSocketPath(d2, socketPath);
        CapabilityGrantScopeAccess::loadAlwaysAllowedCapabilityGrants(d2);
        // The grant survives a daemon restart.
        CHECK(CapabilityGrantScopeAccess::isGranted(d2, capability, origin, identity));
        // And it allows a check on the new daemon.
        CHECK(CapabilityGrantScopeAccess::policyCheck(d2, capability, origin, identity) == CapabilityDecision::Allow);
    }

    // A record under the header loads; the same record without it states no
    // layout, so the file is refused whole and nothing it holds is granted.
    {
        std::string contents;
        if (FILE* file = std::fopen(grantsFile.c_str(), "r")) {
            char buffer[4096];
            std::size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
                contents.append(buffer, got);
            std::fclose(file);
        }
        const std::string record = std::to_string(std::to_underlying(capability)) + "\t" + std::to_string(std::to_underlying(origin)) + "\t" + identity + "\n";
        for (const bool hasHeader : { true, false }) {
            if (FILE* file = std::fopen(grantsFile.c_str(), "w")) {
                std::fputs(((hasHeader ? std::string("# ubiquitty capability-grants v1\n") : std::string()) + record).c_str(), file);
                std::fclose(file);
            }
            Daemon reader;
            CapabilityGrantScopeAccess::setSocketPath(reader, socketPath);
            CapabilityGrantScopeAccess::loadAlwaysAllowedCapabilityGrants(reader);
            CHECK(CapabilityGrantScopeAccess::isGranted(reader, capability, origin, identity) == hasHeader);
        }
        if (FILE* file = std::fopen(grantsFile.c_str(), "w")) {
            std::fputs(contents.c_str(), file);
            std::fclose(file);
        }
    }

    {
        Daemon daemon;
        CapabilityGrantScopeAccess::setSocketPath(daemon, socketPath);
        CapabilityGrantScopeAccess::loadAlwaysAllowedCapabilityGrants(daemon);
        const auto human = CapabilityGrantScopeAccess::addHuman(daemon, sessionID);
        const std::string otherIdentity = "token:other-management-grant";
        CapabilityGrantScopeAccess::injectGrant(daemon, capability, origin, otherIdentity, CapabilityGrantScope::Session, sessionID);
        CapabilityGrantScopeAccess::revokeManagedGrant(daemon, human, capability, origin, identity);
        CHECK(!CapabilityGrantScopeAccess::isGranted(daemon, capability, origin, identity));
        CHECK(CapabilityGrantScopeAccess::isGranted(daemon, capability, origin, otherIdentity));
    }
    {
        Daemon daemon;
        CapabilityGrantScopeAccess::setSocketPath(daemon, socketPath);
        CapabilityGrantScopeAccess::loadAlwaysAllowedCapabilityGrants(daemon);
        CHECK(!CapabilityGrantScopeAccess::isGranted(daemon, capability, origin, identity));
    }

    removeTempDirectory(tempDir);
}

// ---------------------------------------------------------------------------
// Test (3c) — An Always answer the grant file cannot record holds for the
//             session only; a withdrawal it cannot record is reported and
//             keeps the grant. Both succeed once the file can be edited.
//
// Revert: ignore the edit's result in the handler — the answer is kept as an
// Always grant no file holds, and the not-a-session-grant assertion fails.
// ---------------------------------------------------------------------------
static void testAlwaysUnrecordable()
{
    constexpr std::uint32_t sessionID = 350;
    constexpr CapabilityOrigin origin = CapabilityOrigin::Agent;
    constexpr Capability capability = Capability::SetWorkingDirectory; // LowRiskInfo
    const std::string tokenID = "unrecordabletokenid";
    const std::string identity = "token:" + tokenID;

    const std::string tempDir = makeTempDirectory();
    if (tempDir.empty()) {
        CHECK_MSG(false, "testAlwaysUnrecordable fixture: mkdtemp failed");
        return;
    }
    const std::string socketPath = tempDir + "/test.sock";
    const std::string grantsFile = tempDir + "/capability_grants";
    const auto isStored = [&] {
        Daemon reader;
        CapabilityGrantScopeAccess::setSocketPath(reader, socketPath);
        CapabilityGrantScopeAccess::loadAlwaysAllowedCapabilityGrants(reader);
        return CapabilityGrantScopeAccess::isGranted(reader, capability, origin, identity);
    };

    Daemon d;
    CapabilityGrantScopeAccess::setSocketPath(d, socketPath);
    const std::size_t human = CapabilityGrantScopeAccess::addHuman(d, sessionID);
    CapabilityGrantScopeAccess::addTokenAgent(d, sessionID, tokenID);
    {
        TokenStoreLock held(grantsFile);
        CHECK(held.isLocked());
        CapabilityGrantScopeAccess::injectPending(d, 45, capability, origin, identity, sessionID);
        CapabilityGrantScopeAccess::driveConsentResponse(d, human, 45, Protocol::Sync::ConsentChoice::AllowAlways);
    }
    CHECK(CapabilityGrantScopeAccess::isGranted(d, capability, origin, identity));
    CHECK_MSG(CapabilityGrantScopeAccess::sessionGrantCount(d, sessionID) == 1, "an Always answer the file cannot record holds for the session only");
    CHECK(!isStored());
    CapabilityGrantScopeAccess::dropSession(d, sessionID);
    CHECK(!CapabilityGrantScopeAccess::isGranted(d, capability, origin, identity));

    CapabilityGrantScopeAccess::injectPending(d, 46, capability, origin, identity, sessionID);
    CapabilityGrantScopeAccess::driveConsentResponse(d, human, 46, Protocol::Sync::ConsentChoice::AllowAlways);
    CHECK(CapabilityGrantScopeAccess::isGranted(d, capability, origin, identity));
    CHECK(CapabilityGrantScopeAccess::sessionGrantCount(d, sessionID) == 0);
    CHECK(isStored());

    {
        TokenStoreLock held(grantsFile);
        CHECK(held.isLocked());
        CHECK(CapabilityGrantScopeAccess::revokeManagedGrantStatus(d, human, capability, origin, identity) == Protocol::Sync::ManagementStatus::Failed);
    }
    CHECK_MSG(CapabilityGrantScopeAccess::isGranted(d, capability, origin, identity) && isStored(), "a withdrawal the file cannot record keeps the grant");
    CapabilityGrantScopeAccess::revokeManagedGrant(d, human, capability, origin, identity);
    CHECK(!CapabilityGrantScopeAccess::isGranted(d, capability, origin, identity));
    CHECK(!isStored());

    removeTempDirectory(tempDir);
}

// ---------------------------------------------------------------------------
// Test (3b) — LowRiskInfo + AllowAlways + non-token identity → Session scope,
//             not persisted; grant is dropped on dropSession (Session, not Always).
//
// The key observable: if the degrade were absent, the grant would be Always-
// scoped and would survive dropSession. With the degrade it is Session-scoped
// and is removed when the session tears down. Revert the `!starts_with("token:")`
// degrade in the handler — the grant becomes Always, dropSession leaves it in
// place → the post-drop-isGranted assertion fails.
// ---------------------------------------------------------------------------
static void testAlwaysDegradedNonToken()
{
    constexpr std::uint32_t sessionID = 400;
    constexpr CapabilityOrigin origin = CapabilityOrigin::Agent;
    constexpr Capability capability = Capability::SetWorkingDirectory; // LowRiskInfo
    // Path-keyed agent: grantKeyFor returns peerProcess, not "token:..."
    const std::string identity = "/usr/bin/ubq";

    const std::string tempDir = makeTempDirectory();
    if (tempDir.empty()) {
        CHECK_MSG(false, "testAlwaysDegradedNonToken fixture: mkdtemp failed");
        return;
    }
    const std::string socketPath = tempDir + "/test.sock";

    Daemon d;
    CapabilityGrantScopeAccess::setSocketPath(d, socketPath);
    const std::size_t human = CapabilityGrantScopeAccess::addHuman(d, sessionID);
    // No agent is attached: Greet admission holds a tokenless agent to a
    // token's scope and refuses it, so the path-keyed question stands alone.
    CapabilityGrantScopeAccess::injectPending(d, 44, capability, origin, identity, sessionID);
    // Human requests AllowAlways — non-token identity → handler degrades to Session.
    CapabilityGrantScopeAccess::driveConsentResponse(d, human, 44, Protocol::Sync::ConsentChoice::AllowAlways);
    // Granted in-session (Session scope).
    CHECK(CapabilityGrantScopeAccess::isGranted(d, capability, origin, identity));
    CHECK(CapabilityGrantScopeAccess::policyCheck(d, capability, origin, identity) == CapabilityDecision::Allow);

    // Session-scoped grant is cleared when the session ends.
    CHECK(CapabilityGrantScopeAccess::sessionGrantCount(d, sessionID) == 1);
    CapabilityGrantScopeAccess::dropSession(d, sessionID);
    CHECK(CapabilityGrantScopeAccess::sessionGrantCount(d, sessionID) == 0);
    CHECK(!CapabilityGrantScopeAccess::isGranted(d, capability, origin, identity));

    removeTempDirectory(tempDir);
}

// ---------------------------------------------------------------------------
// Test (4) — Session grant dropped on dropSession
// ---------------------------------------------------------------------------
static void testSessionDrop()
{
    constexpr CapabilityOrigin origin = CapabilityOrigin::Agent;
    // Use ClipboardWrite (HostEscaping): its Agent default is Prompt, so after
    // the Session grant is removed by dropSession the next check re-prompts.
    constexpr Capability capability = Capability::ClipboardWrite;
    constexpr std::uint32_t targetSession = 500;
    constexpr std::uint32_t otherSession = 501;
    const std::string identity = "token:sessiondroptestid";
    const std::string otherIdentity = "token:sessiondropother";

    Daemon d;
    // Session grant for the target session: injected directly (the tier gate
    // lives in the ConsentResponse handler, not the store, so we can exercise
    // dropSession on any capability).
    CapabilityGrantScopeAccess::injectGrant(d, capability, origin, identity, CapabilityGrantScope::Session, targetSession);
    // Grant for a different session — must survive the drop of targetSession.
    CapabilityGrantScopeAccess::injectGrant(d, capability, origin, otherIdentity, CapabilityGrantScope::Session, otherSession);

    CHECK(CapabilityGrantScopeAccess::isGranted(d, capability, origin, identity));
    CHECK(CapabilityGrantScopeAccess::isGranted(d, capability, origin, otherIdentity));
    CHECK(CapabilityGrantScopeAccess::policyCheck(d, capability, origin, identity) == CapabilityDecision::Allow);

    // Drop only the target session's grants.
    CapabilityGrantScopeAccess::dropSession(d, targetSession);

    // The target session's grant is gone; HostEscaping + Agent default = Prompt.
    CHECK(!CapabilityGrantScopeAccess::isGranted(d, capability, origin, identity));
    CHECK(CapabilityGrantScopeAccess::policyCheck(d, capability, origin, identity) == CapabilityDecision::Prompt);
    // The other session's grant is unaffected.
    CHECK(CapabilityGrantScopeAccess::isGranted(d, capability, origin, otherIdentity));
    CHECK(CapabilityGrantScopeAccess::policyCheck(d, capability, origin, otherIdentity) == CapabilityDecision::Allow);
}

// ---------------------------------------------------------------------------

int main()
{
    testOneShotConsume();
    testTierGateHostEscaping();
    testAlwaysPersistenceToken();
    testAlwaysUnrecordable();
    testAlwaysDegradedNonToken();
    testSessionDrop();

    if (failures == 0)
        printf("capability_grant_scope: PASSED — OneShot consumed on first Allow; "
               "HostEscaping/ReadExfil forced to OneShot; LowRiskInfo + token-identity "
               "Always persists and reloads, or holds for the session when it cannot "
               "be recorded; non-token degrades to Session without "
               "persistence; dropSession clears Session grants, spares others\n");
    else
        printf("capability_grant_scope: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
