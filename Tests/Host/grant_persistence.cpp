// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/grant_persistence.cpp — grants that outlive the answer, and the
// pass that takes them back.
//
// Two mechanisms here decide whether an agent still has the access a human
// once gave it, and both work on state that survives the moment of the
// decision.
//
//   * The always-allow list: a human answering "always" writes a grant key to
//     disk so the next attach does not re-prompt. The key is derived from the
//     live client — a token digest when there is one, the verified binary path
//     otherwise — so an agent with neither is not grantable at all, and a list
//     that grew without bound would be a permanent, unreviewable allowlist.
//
//   * Token reverification: a token withdrawn from the store must disconnect
//     the clients already holding it. A revoke that only stopped NEW handshakes
//     would leave the withdrawn token working for as long as its holder stayed
//     connected, which is the opposite of what withdrawing it means.
//
// Reverification withdraws authority immediately; only the run loop reaps
// physical clients. Interleaved valid and withdrawn principals prove that
// every withdrawn grant ends without taking an independent client with it.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Host/ScreenPlaneAccess.h"
#include "Host/TrustedLocalPeer.h"
#include "PAL/Stream.h" // Session owns a unique_ptr<PAL::Stream>
#include "Platform/Host/CapabilityGrantFile.h"
#include "Platform/Host/ConsentStore.h"
#include "Platform/Host/HostCapabilityPolicy.h"
#include "Platform/Host/TokenStore.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace Ubiquitty {
namespace Host {

struct GrantPersistenceAccess {
    // A client on a pipe: the daemon's writes land where the test can read
    // them, rather than failing (fd -1) or vanishing (/dev/null).
    static std::size_t addClient(Daemon& d, const std::string& peerProcess, const std::string& agentLabel,
        const std::string& tokenID, int* readEndOut)
    {
        int fds[2] = { -1, -1 };
        if (::pipe(fds) != 0)
            return static_cast<std::size_t>(-1);
        if (readEndOut)
            *readEndOut = fds[0];
        Daemon::Client c {};
        c.fileDescriptor = PAL::UnixFileDescriptor(fds[1]);
        c.clientID = d.m_nextClientID++;
        c.peerProcess = peerProcess;
        c.agentLabel = agentLabel;
        if (!tokenID.empty()) {
            c.hasToken = true;
            c.tokenID = tokenID;
        }
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    static bool receiveGreeting(Daemon& d, std::size_t index, const Protocol::Sync::Greet& greeting)
    {
        std::vector<std::uint8_t> body, wire;
        Protocol::Sync::encode(body, greeting);
        if (body.empty() || !Protocol::Sync::decodeGreet(body)
            || !Protocol::Sync::appendFrame(wire, Protocol::Sync::FrameKind::Greet, 0, body))
            return false;
        ScreenPlaneAccess::receive(d, index, wire);
        return index < d.m_clients.size() && d.hasLiveStructureAttachment(d.m_clients[index])
            && d.m_clients[index].syncConnection->state() == Protocol::Sync::ConnectionState::Attached;
    }

    static bool greetSelectedSession(Daemon& d, std::size_t index, const std::string& token, Protocol::Sync::Role role = Protocol::Sync::Role::Viewer)
    {
        const auto& client = d.m_clients[index];
        Protocol::Sync::Greet greeting;
        greeting.wireVersion = Protocol::Sync::wireVersion;
        greeting.appVersion = Protocol::Sync::appVersion();
        greeting.clientInstance = { .low = client.clientID, .high = 0x6772616e74 };
        greeting.clientKind = token.empty() ? Protocol::Sync::ClientKind::Human : Protocol::Sync::ClientKind::Agent;
        greeting.requestedRole = role;
        greeting.request = { .target = Protocol::Sync::AdmissionTarget::Session, .sessionID = client.sessionID, .expectedDaemonInstanceID = d.m_daemonInstanceID, .agentLabel = client.agentLabel, .isAttachOnly = true };
        if (!token.empty())
            greeting.credential.emplace(token.begin(), token.end());
        return receiveGreeting(d, index, greeting);
    }

    // Only the live-revocation cases call this. Other clients exercise grant
    // key/persistence policy without claiming successful wire admission.
    static bool admit(Daemon& d, std::size_t index, const std::string& token)
    {
        auto& session = ScreenPlaneAccess::makeSession(d, 2, 8);
        auto& client = d.m_clients[index];
        client.sessionID = session.id;
        client.hasToken = false;
        client.tokenID.clear();
        if (token.empty())
            seedTrustedLocalPeer(client);
        else
            (void)session.addSessionGrant("token:" + Crypto::sha256Hex(token));
        return greetSelectedSession(d, index, token)
            && (token.empty() ? !d.m_clients[index].hasToken
                              : d.m_clients[index].hasToken && digestsEqual(d.m_clients[index].tokenID, Crypto::sha256Hex(token))
                        && d.m_clients[index].tokenScope == Protocol::Sync::ScopeSet { Protocol::Sync::Scope::Observe });
    }

    static bool admitBootstrap(Daemon& d, std::size_t index, const std::string& replacement = {})
    {
        auto& session = ScreenPlaneAccess::makeSession(d, 2, 8);
        auto& client = d.m_clients[index];
        client.sessionID = session.id;
        client.hasToken = true;
        client.tokenID = Crypto::sha256Hex("in-memory-bootstrap");
        client.tokenScope = { Protocol::Sync::Scope::Observe };
        client.hasBootstrapToken = true;
        client.isInteractiveUser = true;
        client.peerProcess.clear();
        client.agentLabel.clear();
        if (!replacement.empty())
            (void)session.addSessionGrant("token:" + Crypto::sha256Hex(replacement));
        return greetSelectedSession(d, index, replacement);
    }

    enum class BootstrapConsent : std::uint8_t {
        Always,
        Session
    };

    static bool admitBootstrapAgent(Daemon& d, std::size_t index, const std::string& tokenID, BootstrapConsent consent)
    {
        auto& session = ScreenPlaneAccess::makeSession(d, 2, 8);
        auto& client = d.m_clients[index];
        client.sessionID = session.id;
        client.hasToken = true;
        client.tokenID = tokenID;
        client.tokenScope = { Protocol::Sync::Scope::Observe };
        client.hasBootstrapToken = true;
        client.isInteractiveUser = false;
        client.peerProcess.clear();
        client.agentLabel = "bootstrap-agent";
        const auto key = d.grantKeyFor(client);
        (void)session.addSessionGrant(key);
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Agent, Protocol::Sync::Role::Agent);
        if (consent == BootstrapConsent::Always)
            std::erase(session.agentSessionGrantIdentities, key);
        return d.hasLiveStructureAttachment(d.m_clients[index]);
    }

    static bool admitUnbound(Daemon& d, std::size_t index, const std::string& token, Protocol::Sync::AdmissionTarget target)
    {
        auto& client = d.m_clients[index];
        client.hasToken = false;
        client.tokenID.clear();
        client.sessionID = 0;
        Protocol::Sync::Greet greeting;
        greeting.wireVersion = Protocol::Sync::wireVersion;
        greeting.appVersion = Protocol::Sync::appVersion();
        greeting.clientInstance = { .low = client.clientID, .high = 0x756e626f756e64 };
        greeting.clientKind = Protocol::Sync::ClientKind::Agent;
        greeting.requestedRole = Protocol::Sync::Role::Agent;
        greeting.request = { .target = target, .expectedDaemonInstanceID = d.m_daemonInstanceID, .agentLabel = client.agentLabel };
        greeting.credential.emplace(token.begin(), token.end());
        return receiveGreeting(d, index, greeting);
    }

    static bool beginPendingAttach(Daemon& d, std::size_t index, std::uint32_t sessionID, const std::string& token)
    {
        d.m_clients[index].sessionID = sessionID;
        d.m_clients[index].hasToken = false;
        d.m_clients[index].tokenID.clear();
        (void)greetSelectedSession(d, index, token, Protocol::Sync::Role::Agent);
        auto& client = d.m_clients[index];
        client.sessionID = 0;
        return client.syncConnection && client.syncConnection->state() == Protocol::Sync::ConnectionState::AwaitingConsent;
    }
    static bool holdAlwaysApproval(Daemon& d, std::size_t requester, std::size_t human)
    {
        return d.retainAttachConsentDecision(requester, d.m_clients[human], Protocol::Sync::ConsentChoice::AllowAlways);
    }
    static bool holdOnceApproval(Daemon& d, std::size_t requester, std::size_t human)
    {
        return d.retainAttachConsentDecision(requester, d.m_clients[human], Protocol::Sync::ConsentChoice::AllowOnce);
    }
    static bool hasHeldApproval(const Daemon& d, std::size_t index) { return d.m_clients[index].attachConsentApproval.has_value(); }
    static bool servicePendingAttach(Daemon& d, std::size_t index)
    {
        d.servicePendingAttachConsent(index);
        return d.hasLiveStructureAttachment(d.m_clients[index]);
    }
    static std::uint32_t sessionID(const Daemon& d, std::size_t index) { return d.m_clients[index].sessionID; }
    static std::uint64_t tokenExpiry(const Daemon& d, std::size_t index) { return d.m_clients[index].tokenExpiryUnix; }
    static bool hasRetainedTokenGrant(const Daemon& d, std::size_t index)
    {
        const auto* record = d.m_clients[index].syncConnection->attachment();
        return record && std::ranges::any_of(d.m_attachmentGrants, [&](const auto& grant) { return grant.attachmentID == record->attachmentID; });
    }
    static bool hasResumableAttachment(const Daemon& d, std::size_t index)
    {
        const auto* record = d.m_clients[index].syncConnection->attachment();
        return record && record->isResumable;
    }
    static std::optional<std::uint64_t> retainedExpiry(const Daemon& d, std::size_t index)
    {
        const auto* record = d.m_clients[index].syncConnection->attachment();
        if (record)
            for (const auto& grant : d.m_attachmentGrants)
                if (grant.attachmentID == record->attachmentID)
                    return grant.tokenExpiryUnix;
        return std::nullopt;
    }
    static bool feedRawBatch(Daemon& d, std::size_t index)
    {
        const auto& client = d.m_clients[index];
        if (client.paneSubscriptions.empty() || !client.paneSubscriptions.front().screenHandle)
            return false;
        const auto handle = client.paneSubscriptions.front().screenHandle;
        const std::array<std::uint8_t, 1> input { 'x' };
        std::vector<std::uint8_t> body, wire;
        Protocol::Sync::encode(body, Protocol::Sync::Raw { .inputSeq = 1, .bytes = input });
        if (!Protocol::Sync::appendFrame(wire, Protocol::Sync::FrameKind::Raw, handle, body))
            return false;
        d.feedClientFrames(index, std::as_bytes(std::span(wire)));
        return true;
    }

    static void useTokenStore(Daemon& d, const std::string& path) { d.m_tokenPath = path; }
    static void useScratchStore(Daemon& d, const std::string& directory) { d.m_socketPath = directory + "/daemon.sock"; }

    static void reverify(Daemon& d) { d.reverifyTokenClients(); }

    static void loadCapabilityGrants(Daemon& d) { d.loadAlwaysAllowedCapabilityGrants(); }
    static bool editCapabilityGrants(Daemon& d, CapabilityGrantOperation operation, const CapabilityGrantKey& key)
    {
        return d.editAlwaysAllowedCapabilityGrants(operation, key);
    }
    static bool isCapabilityCached(const Daemon& d, const CapabilityGrantKey& key) { return d.m_grantStore.isGranted(key.capability, key.origin, key.identity); }
    // One check through a session policy over the daemon's store, as an
    // effect in one of its sessions would ask.
    static CapabilityDecision checkCapability(Daemon& d, const CapabilityGrantKey& key)
    {
        HostCapabilityPolicy policy;
        policy.setGrantStore(&d.m_grantStore);
        policy.setCurrentIdentity(key.identity);
        return policy.check(key.capability, key.origin, "");
    }
    static void handOverCapabilityGrants(Daemon& d, CapabilityGrantStore&& grants) { d.replaceCapabilityGrants(std::move(grants)); }
    // The Session is built inside the daemon and referenced there: it owns a
    // stream and is neither copyable nor movable, so a test cannot hold one.
    static bool standingGrantForNewSession(Daemon& d, std::size_t agentIndex, std::uint32_t sessionID)
    {
        auto session = std::make_unique<Daemon::Session>();
        session->id = sessionID;
        const bool granted = d.agentHasStandingGrant(d.m_clients[agentIndex], *session);
        return granted;
    }
    static bool persist(Daemon& d, std::size_t agentIndex) { return d.persistAlwaysAllowedAgent(agentIndex); }
    static std::string grantKey(const Daemon& d, std::size_t index) { return d.grantKeyFor(d.m_clients[index]); }

    static std::vector<std::string>& grants(Daemon& d) { return d.m_alwaysAllowedAgents; }
    static bool save(Daemon& d)
    {
        const auto path = d.consentStorePath();
        TokenStoreLock lock(path);
        return lock.isLocked() && writeStandingConsentList(path, d.m_alwaysAllowedAgents) == ConsentEdit::Changed;
    }
    static void load(Daemon& d) { (void)d.loadAlwaysAllowedAgents(); }
    static bool loadWhole(Daemon& d) { return d.loadAlwaysAllowedAgents(); }
    static void reload(Daemon& d) { d.reloadCredentialStores(); }
    static void refresh(Daemon& d) { d.refreshCredentialStoresIfChanged(); }
    static bool mayDispatch(Daemon& d, std::size_t& index) { return d.refreshCredentialStoresBeforeDispatch(index); }
    static bool hasStoreStamp(const Daemon& d) { return d.m_credentialStoreStamp.has_value(); }
    static bool isConsentWhole(const Daemon& d) { return d.m_isConsentStoreWhole; }
    static bool areStoresWhole(const Daemon& d) { return d.m_areCredentialStoresWhole; }
    static bool revokeAlways(Daemon& d, const std::string& identity) { return d.revokeAlwaysAllowedAgent(identity); }
    static void retireWithdrawals(Daemon& d) { d.retireStandingConsentWithdrawals(); }
    static bool holdsStoreAuthority(const Daemon& d) { return d.holdsStoreBoundAuthority(); }
    static bool isLive(const Daemon& d, std::size_t index) { return index < d.m_clients.size() && d.hasLiveStructureAttachment(d.m_clients[index]); }
    static bool hasTokenInvalidGreeting(const Daemon& d, std::size_t index)
    {
        return index < d.m_clients.size() && d.m_clients[index].syncConnection
            && d.m_clients[index].syncConnection->byeReason() == Protocol::Sync::ByeReason::TokenInvalid;
    }
    static Protocol::Sync::ID128 attachmentID(const Daemon& d, std::size_t index)
    {
        const auto* record = d.m_clients[index].syncConnection->attachment();
        assert(record);
        return record->attachmentID;
    }

    // Was this client told its token was revoked?
    static bool sawRevoked(const Daemon& d, std::size_t index, int readEnd)
    {
        std::vector<std::uint8_t> bytes;
        if (readEnd >= 0) {
            const int flags = ::fcntl(readEnd, F_GETFL, 0);
            ::fcntl(readEnd, F_SETFL, flags | O_NONBLOCK);
            std::uint8_t chunk[4096];
            for (;;) {
                const ssize_t n = ::read(readEnd, chunk, sizeof chunk);
                if (n <= 0)
                    break;
                bytes.insert(bytes.end(), chunk, chunk + n);
            }
        }
        if (index < d.m_clients.size()) {
            const std::vector<std::uint8_t>& queued = d.m_clients[index].outputBuffer;
            bytes.insert(bytes.end(), queued.begin() + static_cast<std::ptrdiff_t>(d.m_clients[index].outputConsumed), queued.end());
        }
        Protocol::Sync::FrameDecoder decoder;
        decoder.append(bytes);
        while (const auto frame = decoder.next()) {
            if (frame->kind != Protocol::Sync::FrameKind::Bye)
                continue;
            const auto bye = Protocol::Sync::decodeBye(frame->body);
            if (bye && bye->reason == Protocol::Sync::ByeReason::TokenRevoked)
                return true;
        }
        return false;
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::GrantPersistenceAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "grant_persistence: FAIL — %s\n", what);
        ++failures;
    }
}

constexpr std::uint64_t farFuture = 4102444800ull;

// Write a token store holding one record per plaintext token given.
void writeStore(const std::string& path, const std::vector<std::string>& plaintexts)
{
    std::vector<Sync::TokenRecord> records;
    for (const std::string& plaintext : plaintexts) {
        Sync::TokenRecord record;
        record.hashHex = Ubiquitty::Crypto::sha256Hex(plaintext);
        record.scope = Sync::ScopeSet { Sync::Scope::Observe };
        record.expiryUnix = farFuture;
        record.label = "test";
        records.push_back(record);
    }
    check(Ubiquitty::Host::writeTokens(path, records) == Ubiquitty::Host::TokenWriteOutcome::Durable,
        "the grant store rewrote durably");
}

bool writeConsent(const std::string& path, const std::string& contents)
{
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(path.c_str(), "w"), std::fclose);
    if (!file)
        return false;
    const bool didWrite = std::fwrite(contents.data(), 1, contents.size(), file.get()) == contents.size();
    const bool didClose = std::fclose(file.release()) == 0;
    return didWrite && didClose;
}

bool writeScopedToken(const std::string& path, const std::string& token, Sync::ScopeSet scope, std::uint64_t expiry)
{
    return Ubiquitty::Host::writeTokens(path, { { Ubiquitty::Crypto::sha256Hex(token), scope, expiry, "scoped fixture" } })
        == Ubiquitty::Host::TokenWriteOutcome::Durable;
}

} // namespace

int main()
{
    // Teardown can write a final frame after a fixture releases a pipe reader.
    ::signal(SIGPIPE, SIG_IGN);
    char directoryTemplate[] = "/tmp/ubiquitty-grants-XXXXXX";
    const char* made = ::mkdtemp(directoryTemplate);
    if (!made) {
        std::fprintf(stderr, "grant_persistence: could not create a scratch directory\n");
        return 1;
    }
    const std::string scratch(made);
    const std::string tokenPath = scratch + "/tokens";

    // (a) A token withdrawn from the store disconnects the client holding it,
    // and only that one. The still-valid neighbour keeps its connection, or a
    // revocation would be a way to knock over every other agent at once.
    {
        writeStore(tokenPath, { "kept-token", "withdrawn-token" });
        const std::string keptDigest = Ubiquitty::Crypto::sha256Hex("kept-token");
        const std::string goneDigest = Ubiquitty::Crypto::sha256Hex("withdrawn-token");

        Daemon d;
        GrantPersistenceAccess::useTokenStore(d, tokenPath);
        GrantPersistenceAccess::useScratchStore(d, scratch);
        int keptRead = -1;
        int goneRead = -1;
        int tokenlessRead = -1;
        const std::size_t kept = GrantPersistenceAccess::addClient(d, "/usr/bin/a", "agent", keptDigest, &keptRead);
        const std::size_t gone = GrantPersistenceAccess::addClient(d, "/usr/bin/b", "agent", goneDigest, &goneRead);
        const std::size_t tokenless = GrantPersistenceAccess::addClient(d, "ubiquitty-host", "", "", &tokenlessRead);

        check(GrantPersistenceAccess::admit(d, kept, "kept-token"), "the kept token is admitted before withdrawal");
        check(GrantPersistenceAccess::admit(d, gone, "withdrawn-token"), "the withdrawn token starts as a valid admitted client");
        check(GrantPersistenceAccess::admit(d, tokenless, {}), "the trusted tokenless neighbour is actually admitted");
        writeStore(tokenPath, { "kept-token" });

        GrantPersistenceAccess::reverify(d);

        check(GrantPersistenceAccess::sawRevoked(d, gone, goneRead),
            "a client whose token left the store is told it was revoked");
        check(!GrantPersistenceAccess::sawRevoked(d, kept, keptRead),
            "a client whose token is still in the store keeps its connection");
        check(!GrantPersistenceAccess::sawRevoked(d, tokenless, tokenlessRead),
            "a tokenless client is not swept up by a token revocation");
    }

    // (b) Several revoked clients at once, interleaved with independent valid
    // principals, so the pass must select every withdrawal and no neighbor.
    {
        writeStore(tokenPath, { "valid-1", "valid-2", "gone-1", "gone-2", "gone-3", "gone-4" });
        Daemon d;
        GrantPersistenceAccess::useTokenStore(d, tokenPath);
        GrantPersistenceAccess::useScratchStore(d, scratch);

        int reads[6] = { -1, -1, -1, -1, -1, -1 };
        std::size_t indices[6];
        const char* digests[6] = { nullptr };
        const std::string v1 = Ubiquitty::Crypto::sha256Hex("valid-1");
        const std::string v2 = Ubiquitty::Crypto::sha256Hex("valid-2");
        const std::string gone1 = Ubiquitty::Crypto::sha256Hex("gone-1");
        const std::string gone2 = Ubiquitty::Crypto::sha256Hex("gone-2");
        const std::string gone3 = Ubiquitty::Crypto::sha256Hex("gone-3");
        const std::string gone4 = Ubiquitty::Crypto::sha256Hex("gone-4");
        const std::string order[6] = { gone1, v1, gone2, gone3, v2, gone4 };
        const std::string plaintexts[6] = { "gone-1", "valid-1", "gone-2", "gone-3", "valid-2", "gone-4" };
        (void)digests;
        for (int i = 0; i < 6; ++i) {
            indices[i] = GrantPersistenceAccess::addClient(d, "/usr/bin/agent", "agent", order[i], &reads[i]);
            check(GrantPersistenceAccess::admit(d, indices[i], plaintexts[i]), "every token starts as an actually admitted client");
        }

        writeStore(tokenPath, { "valid-1", "valid-2" });
        GrantPersistenceAccess::reverify(d);

        for (int i = 0; i < 6; ++i) {
            const bool shouldBeRevoked = (order[i] != v1 && order[i] != v2);
            const bool wasRevoked = GrantPersistenceAccess::sawRevoked(d, indices[i], reads[i]);
            check(wasRevoked == shouldBeRevoked,
                shouldBeRevoked ? "every revoked client in the table is told, whatever its position"
                                : "no client with a live token is swept up");
        }
    }

    // (c) A grant key is derived from the live client. An agent with neither a
    // token nor a verified binary path has no key, so it cannot hold a
    // standing grant — an empty key matching an empty stored entry would make
    // one unidentifiable agent inherit another's answer.
    {
        Daemon d;
        GrantPersistenceAccess::useScratchStore(d, scratch);
        int readEnd = -1;
        const std::size_t anonymous = GrantPersistenceAccess::addClient(d, "", "agent", "", &readEnd);
        check(GrantPersistenceAccess::grantKey(d, anonymous).empty(), "an unidentifiable agent has no grant key");
        check(!GrantPersistenceAccess::standingGrantForNewSession(d, anonymous, 1),
            "an agent with no grant key holds no standing grant");
        check(!GrantPersistenceAccess::persist(d, anonymous),
            "and it cannot be persisted into the always-allow list");
        check(GrantPersistenceAccess::grants(d).empty(), "the list stays empty rather than gaining a blank key");
    }

    // (d) The token digest is preferred over the binary path, so two agents
    // sharing a binary stay distinguishable and a swapped binary cannot
    // inherit the grant of the one it replaced.
    {
        Daemon d;
        GrantPersistenceAccess::useScratchStore(d, scratch);
        int firstRead = -1;
        int secondRead = -1;
        const std::size_t first = GrantPersistenceAccess::addClient(d, "/usr/bin/shared", "agent", "digest-1", &firstRead);
        const std::size_t second = GrantPersistenceAccess::addClient(d, "/usr/bin/shared", "agent", "digest-2", &secondRead);
        check(GrantPersistenceAccess::grantKey(d, first) != GrantPersistenceAccess::grantKey(d, second),
            "two agents sharing a binary have different grant keys");

        check(GrantPersistenceAccess::persist(d, first), "the first agent is persisted");
        check(GrantPersistenceAccess::grants(d).size() == 1, "one grant was written");
        check(GrantPersistenceAccess::persist(d, first), "persisting the same agent again succeeds");
        check(GrantPersistenceAccess::grants(d).size() == 1, "and adds no duplicate");
        check(GrantPersistenceAccess::persist(d, second), "the second agent is persisted separately");
        check(GrantPersistenceAccess::grants(d).size() == 2, "so the list holds both");
    }

    // (e) The list is capped. A grant list that grew without bound would be a
    // permanent allowlist nobody reviews, so the oldest entry is evicted —
    // and the newest is the one kept, since it is the answer the human just
    // gave.
    {
        Daemon d;
        GrantPersistenceAccess::useScratchStore(d, scratch);
        std::vector<std::string>& grants = GrantPersistenceAccess::grants(d);
        for (int i = 0; i < 256; ++i)
            grants.push_back("token:filler-" + std::to_string(i));
        const std::string oldest = grants.front();
        check(GrantPersistenceAccess::save(d), "the capped standing grants are seeded on disk");

        int readEnd = -1;
        const std::size_t fresh = GrantPersistenceAccess::addClient(d, "/usr/bin/new", "agent", "brand-new", &readEnd);
        check(GrantPersistenceAccess::persist(d, fresh), "a new grant is accepted at the cap");
        check(GrantPersistenceAccess::grants(d).size() <= 256, "the list does not grow past its cap");
        check(GrantPersistenceAccess::grants(d).front() != oldest, "the oldest entry was evicted");
        const std::string key = GrantPersistenceAccess::grantKey(d, fresh);
        bool holdsNewest = false;
        for (const std::string& entry : GrantPersistenceAccess::grants(d))
            if (entry == key)
                holdsNewest = true;
        check(holdsNewest, "the newest grant is the one kept");
    }

    // (f) The list round-trips through disk — what a human answered survives a
    // daemon restart, which is the only reason to persist it — but only for
    // token-bound principals. A path-keyed entry is dropped on load, because
    // a grant keyed on a binary path is inherited by whatever is swapped into
    // that path.
    {
        Daemon writer;
        GrantPersistenceAccess::useScratchStore(writer, scratch);
        GrantPersistenceAccess::grants(writer) = { "token:aaa", "/usr/bin/verified-agent", "token:bbb" };
        check(GrantPersistenceAccess::save(writer), "the round-trip consent fixture is written");

        Daemon reader;
        GrantPersistenceAccess::useScratchStore(reader, scratch);
        GrantPersistenceAccess::load(reader);
        const std::vector<std::string>& loaded = GrantPersistenceAccess::grants(reader);
        bool sawFirstToken = false;
        bool sawSecondToken = false;
        bool sawPath = false;
        for (const std::string& entry : loaded) {
            sawFirstToken = sawFirstToken || entry == "token:aaa";
            sawSecondToken = sawSecondToken || entry == "token:bbb";
            sawPath = sawPath || entry == "/usr/bin/verified-agent";
        }
        check(sawFirstToken && sawSecondToken, "every token-bound grant came back byte for byte");
        check(!sawPath, "a path-keyed grant is dropped rather than honored after a restart");
        check(loaded.size() == 2, "and nothing else came back with them");
    }

    // (g) A store that cannot be read leaves the list empty rather than
    // half-populated: a daemon that started with a partial allowlist would
    // silently re-prompt for some agents and not others.
    {
        Daemon d;
        GrantPersistenceAccess::useScratchStore(d, scratch + "/no-such-directory");
        GrantPersistenceAccess::load(d);
        check(GrantPersistenceAccess::grants(d).empty(), "an unreadable store loads nothing");
        check(!GrantPersistenceAccess::save(d), "an unwritable consent path refuses the fixture save");
    }

    ::unlink(tokenPath.c_str());
    // What a grant file says about its own layout. A capability and an origin
    // are persisted as bare integers and range-checked on load against the
    // enum's current last value, so a build that renumbers or extends either
    // reads an old file's numbers under the new meaning and grants something
    // nobody granted — and every value still passes the range check. Three
    // states have to hold: written-then-read, a file with no header, and one
    // naming another layout.
    {
        // Written by this host, read by this host.
        Daemon writer;
        GrantPersistenceAccess::useScratchStore(writer, scratch);
        GrantPersistenceAccess::grants(writer).push_back("token:roundtrip");
        check(GrantPersistenceAccess::save(writer), "the supported-header consent fixture is written");

        Daemon reader;
        GrantPersistenceAccess::useScratchStore(reader, scratch);
        GrantPersistenceAccess::load(reader);
        check(GrantPersistenceAccess::grants(reader).size() == 1
                && GrantPersistenceAccess::grants(reader)[0] == "token:roundtrip",
            "a consent file this host wrote is a consent file this host reads");
    }
    {
        // No header: the file states no layout, so none of its lines is read.
        FILE* headerless = std::fopen((scratch + "/consent").c_str(), "w");
        std::fputs("token:headerless\n", headerless);
        std::fclose(headerless);

        Daemon reader;
        GrantPersistenceAccess::useScratchStore(reader, scratch);
        GrantPersistenceAccess::load(reader);
        check(GrantPersistenceAccess::grants(reader).empty(),
            "a consent file with no header is refused whole");
    }
    {
        // A layout this host does not know. Refused whole rather than in part:
        // a line's meaning is what the version says, and reading the records
        // under a guess is how a grant nobody made gets honoured.
        FILE* future = std::fopen((scratch + "/consent").c_str(), "w");
        std::fputs("# ubiquitty consent v2\n", future);
        std::fputs("token:fromnewer\n", future);
        std::fclose(future);

        Daemon reader;
        GrantPersistenceAccess::useScratchStore(reader, scratch);
        GrantPersistenceAccess::load(reader);
        check(GrantPersistenceAccess::grants(reader).empty(),
            "a consent file naming a layout this host does not read is refused whole");
    }

    // A read chunk is not a grant boundary. The long inert line remains one
    // entry, while its valid neighbor is still a grant.
    {
        const std::string contents = Ubiquitty::Host::grantFileHeader("consent")
            + std::string(4095, 'x') + "token:aaa\ntoken:bbb\n";
        check(writeConsent(scratch + "/consent", contents), "the long-line consent fixture was written");
        Daemon reader;
        GrantPersistenceAccess::useScratchStore(reader, scratch);
        check(GrantPersistenceAccess::loadWhole(reader), "bounded logical lines are read whole");
        check(GrantPersistenceAccess::grants(reader) == std::vector<std::string> { "token:bbb" },
            "a chunk tail cannot turn an inert logical line into a grant");
    }
    {
        const std::string contents = "# ubiquitty consent v1\r\ntoken:aaa\r\ntoken:bbb";
        check(writeConsent(scratch + "/consent", contents), "the CRLF and clean-EOF fixture was written");
        Daemon reader;
        GrantPersistenceAccess::useScratchStore(reader, scratch);
        check(GrantPersistenceAccess::loadWhole(reader), "CRLF and a final unterminated text line are supported");
        check(GrantPersistenceAccess::grants(reader) == std::vector<std::string> { "token:aaa", "token:bbb" },
            "line endings do not change the granted principals");
    }
    {
        const std::string prefix = Ubiquitty::Host::grantFileHeader("consent") + "token:kept\n";
        std::vector<std::string> malformed {
            prefix + std::string("\0hidden\n", 8),
            prefix + std::string(Ubiquitty::Host::maxConsentListLineBytes + 1, 'x') + "\n",
            prefix + std::string(Ubiquitty::Host::maxConsentListEntries, '\n'),
        };
        std::string tooLarge = prefix;
        while (tooLarge.size() < Ubiquitty::Host::maxConsentListBytes) {
            const auto lineBytes = std::min(Ubiquitty::Host::maxConsentListLineBytes, Ubiquitty::Host::maxConsentListBytes - tooLarge.size());
            tooLarge.append(lineBytes - 1, 'x');
            tooLarge.push_back('\n');
        }
        tooLarge.push_back('\n');
        malformed.push_back(std::move(tooLarge));
        for (const auto& contents : malformed) {
            check(writeConsent(scratch + "/consent", contents), "the malformed consent fixture was written");
            Daemon reader;
            GrantPersistenceAccess::useScratchStore(reader, scratch);
            GrantPersistenceAccess::grants(reader).push_back("token:old");
            check(!GrantPersistenceAccess::loadWhole(reader), "a malformed consent store reports a failed read");
            check(GrantPersistenceAccess::grants(reader).empty(), "a failed read publishes no old or prefix grants");
        }
    }

    // A stale daemon applies an intent to disk, never its cached snapshot.
    for (const bool isRemoving : { false, true }) {
        const std::string target = "token:stale-target";
        const std::string unrelated = "token:unrelated";
        const std::string newer = "token:newer";
        const std::string contents = Ubiquitty::Host::grantFileHeader("consent") + target + "\n" + (isRemoving ? unrelated + "\n" : std::string {});
        check(writeConsent(scratch + "/consent", contents), "the stale-daemon store is seeded");
        Daemon first;
        Daemon second;
        GrantPersistenceAccess::useScratchStore(first, scratch);
        GrantPersistenceAccess::useScratchStore(second, scratch);
        check(GrantPersistenceAccess::loadWhole(first) && GrantPersistenceAccess::loadWhole(second), "both daemons cache the initial snapshot");
        int readEnd = -1;
        Daemon& adding = isRemoving ? second : first;
        const auto client = GrantPersistenceAccess::addClient(adding, "/usr/bin/newer-agent", "agent", "newer", &readEnd);
        Ubiquitty::PAL::UnixFileDescriptor output(readEnd);
        if (client == static_cast<std::size_t>(-1)) {
            check(false, "the stale-daemon add client exists");
            continue;
        }
        if (isRemoving) {
            check(GrantPersistenceAccess::persist(second, client), "the second daemon adds a new principal");
            check(GrantPersistenceAccess::revokeAlways(first, target), "the first daemon removes only its named principal");
            check((Ubiquitty::Host::readConsentList(scratch + "/consent").entries == std::vector<std::string> { unrelated, newer }),
                "a stale removal preserves another daemon's new principal");
        } else {
            check(GrantPersistenceAccess::revokeAlways(second, target), "the second daemon withdraws the cached principal");
            check(GrantPersistenceAccess::persist(first, client), "the first daemon adds after that withdrawal");
            check((Ubiquitty::Host::readConsentList(scratch + "/consent").entries == std::vector<std::string> { newer }),
                "a stale addition cannot restore a withdrawn principal");
        }
        GrantPersistenceAccess::retireWithdrawals(first);
        GrantPersistenceAccess::retireWithdrawals(second);
    }

    // A failed consent reload withholds normal token authority, including a
    // lost attachment, while the local owner and in-memory bootstrap survive.
    [&] {
        const std::string liveToken = "consent-live";
        const std::string lostToken = "consent-lost";
        writeStore(tokenPath, { liveToken, lostToken });
        check(writeConsent(scratch + "/consent", Ubiquitty::Host::grantFileHeader("consent")), "the initial consent store is whole");
        Daemon d;
        GrantPersistenceAccess::useTokenStore(d, tokenPath);
        GrantPersistenceAccess::useScratchStore(d, scratch);
        std::array<int, 4> descriptors { -1, -1, -1, -1 };
        const auto human = GrantPersistenceAccess::addClient(d, "ubiquitty-host", "", "", &descriptors[0]);
        const auto bootstrap = GrantPersistenceAccess::addClient(d, "", "", "", &descriptors[1]);
        const auto live = GrantPersistenceAccess::addClient(d, "/usr/bin/live-agent", "agent", "", &descriptors[2]);
        const auto lost = GrantPersistenceAccess::addClient(d, "/usr/bin/lost-agent", "agent", "", &descriptors[3]);
        std::array<Ubiquitty::PAL::UnixFileDescriptor, 4> readers {
            Ubiquitty::PAL::UnixFileDescriptor(descriptors[0]), Ubiquitty::PAL::UnixFileDescriptor(descriptors[1]),
            Ubiquitty::PAL::UnixFileDescriptor(descriptors[2]), Ubiquitty::PAL::UnixFileDescriptor(descriptors[3])
        };
        const auto invalid = static_cast<std::size_t>(-1);
        const bool wereClientsCreated = human != invalid && bootstrap != invalid && live != invalid && lost != invalid;
        check(wereClientsCreated, "all consent reload fixture transports exist");
        if (!wereClientsCreated)
            return;
        const bool isAdmitted = GrantPersistenceAccess::admit(d, human, {}) && GrantPersistenceAccess::admitBootstrap(d, bootstrap)
            && GrantPersistenceAccess::admit(d, live, liveToken) && GrantPersistenceAccess::admit(d, lost, lostToken);
        check(isAdmitted, "all fixture authorities are admitted before the consent failure");
        if (!isAdmitted)
            return;
        const auto lostAttachment = GrantPersistenceAccess::attachmentID(d, lost);
        Ubiquitty::Host::ScreenPlaneAccess::reap(d, lost);
        check(Ubiquitty::Host::ScreenPlaneAccess::attachments(d).find(lostAttachment) != nullptr,
            "the lost attachment retains authority before the failed reload");
        const std::string malformed = Ubiquitty::Host::grantFileHeader("consent") + "token:" + Ubiquitty::Crypto::sha256Hex(liveToken) + "\n" + std::string("\0", 1);
        check(writeConsent(scratch + "/consent", malformed), "the consent failure follows a valid grant prefix");
        GrantPersistenceAccess::reload(d);
        check(!GrantPersistenceAccess::isConsentWhole(d) && GrantPersistenceAccess::grants(d).empty(), "failed consent publishes no standing grants");
        check(GrantPersistenceAccess::sawRevoked(d, live, readers[2].get()), "failed consent revokes a live normal token");
        check(!Ubiquitty::Host::ScreenPlaneAccess::attachments(d).find(lostAttachment), "failed consent retires a lost normal-token attachment");
        check(GrantPersistenceAccess::isLive(d, human), "the local owner retains repair access");
        check(GrantPersistenceAccess::isLive(d, bootstrap), "an in-memory bootstrap retains its independent authority");
        check(!GrantPersistenceAccess::hasStoreStamp(d), "a failed read is never cached as a successful refresh");
    }();

    // A failed refresh is retried with unchanged metadata. A repaired store
    // may then publish a successful stamp and admit fresh authority again.
    {
        writeStore(tokenPath, { "refresh-token" });
        check(writeConsent(scratch + "/consent", Ubiquitty::Host::grantFileHeader("consent") + "token:healthy\n"), "the refresh control is whole");
        Daemon d;
        GrantPersistenceAccess::useTokenStore(d, tokenPath);
        GrantPersistenceAccess::useScratchStore(d, scratch);
        GrantPersistenceAccess::refresh(d);
        check(GrantPersistenceAccess::hasStoreStamp(d), "a whole consent store publishes its refresh stamp");
        check(writeConsent(scratch + "/consent", Ubiquitty::Host::grantFileHeader("consent") + std::string("\0", 1)), "the refresh fixture becomes malformed");
        GrantPersistenceAccess::refresh(d);
        check(!GrantPersistenceAccess::hasStoreStamp(d), "a malformed store clears the successful stamp");
        GrantPersistenceAccess::grants(d).push_back("token:sentinel");
        GrantPersistenceAccess::refresh(d);
        check(GrantPersistenceAccess::grants(d).empty(), "unchanged malformed bytes are re-read rather than trusted from a failed stamp");
        int freshRead = -1;
        auto fresh = GrantPersistenceAccess::addClient(d, "/usr/bin/fresh-agent", "agent", "", &freshRead);
        Ubiquitty::PAL::UnixFileDescriptor freshOutput(freshRead);
        if (fresh != static_cast<std::size_t>(-1))
            check(!GrantPersistenceAccess::mayDispatch(d, fresh), "a fresh agent is withheld before it can supply a normal credential");
        else
            check(false, "the fresh-agent fixture transport exists");
        check(writeConsent(scratch + "/consent", Ubiquitty::Host::grantFileHeader("consent") + "token:repaired\n"), "the consent store is repaired");
        GrantPersistenceAccess::refresh(d);
        check(GrantPersistenceAccess::isConsentWhole(d) && GrantPersistenceAccess::hasStoreStamp(d), "a repaired whole read restores the successful stamp");
        check(GrantPersistenceAccess::grants(d) == std::vector<std::string> { "token:repaired" }, "only repaired consent is published");
    }

    // A bootstrap transport can carry a normal credential in Greet. Its
    // pre-auth exemption cannot authorize that replacement from a bad store.
    for (const bool isStoreWhole : { true, false }) {
        writeStore(tokenPath, { "replacement-token" });
        const std::string contents = Ubiquitty::Host::grantFileHeader("consent") + (isStoreWhole ? std::string {} : std::string("\0", 1));
        check(writeConsent(scratch + "/consent", contents), "the credential replacement fixture was written");
        Daemon d;
        GrantPersistenceAccess::useTokenStore(d, tokenPath);
        GrantPersistenceAccess::useScratchStore(d, scratch);
        int readEnd = -1;
        const auto client = GrantPersistenceAccess::addClient(d, "", "", "", &readEnd);
        Ubiquitty::PAL::UnixFileDescriptor output(readEnd);
        if (client == static_cast<std::size_t>(-1)) {
            check(false, "the replacement fixture transport exists");
            continue;
        }
        check(GrantPersistenceAccess::isConsentWhole(d), "the greeting checks the file rather than a cached failed-read flag");
        const bool isAdmitted = GrantPersistenceAccess::admitBootstrap(d, client, "replacement-token");
        check(isAdmitted == isStoreWhole, "normal credentials replacing bootstrap authority require a whole consent read");
        if (!isStoreWhole)
            check(GrantPersistenceAccess::hasTokenInvalidGreeting(d, client), "malformed consent explicitly refuses the replacement token");
    }

    // Bootstrap agents can depend on Always consent despite their credential
    // being independent of the token store. Lost records keep that dependency.
    for (const bool isLost : { false, true }) {
        for (const bool isMalformed : { false, true }) {
            writeStore(tokenPath, {});
            const auto digest = Ubiquitty::Crypto::sha256Hex("bootstrap-always");
            const auto identity = "token:" + digest;
            check(writeConsent(scratch + "/consent", Ubiquitty::Host::grantFileHeader("consent") + identity + "\n"), "the bootstrap agent's Always grant is seeded");
            Daemon d;
            GrantPersistenceAccess::useTokenStore(d, tokenPath);
            GrantPersistenceAccess::useScratchStore(d, scratch);
            check(GrantPersistenceAccess::loadWhole(d), "the bootstrap Always snapshot is cached");
            std::array<int, 3> descriptors { -1, -1, -1 };
            const auto human = GrantPersistenceAccess::addClient(d, "", "", "", &descriptors[0]);
            const auto independent = GrantPersistenceAccess::addClient(d, "", "agent", "", &descriptors[1]);
            const auto target = GrantPersistenceAccess::addClient(d, "", "agent", "", &descriptors[2]);
            std::array<Ubiquitty::PAL::UnixFileDescriptor, 3> readers {
                Ubiquitty::PAL::UnixFileDescriptor(descriptors[0]), Ubiquitty::PAL::UnixFileDescriptor(descriptors[1]), Ubiquitty::PAL::UnixFileDescriptor(descriptors[2])
            };
            const auto invalid = static_cast<std::size_t>(-1);
            const bool wereClientsCreated = human != invalid && independent != invalid && target != invalid;
            check(wereClientsCreated, "the bootstrap consent transports exist");
            if (!wereClientsCreated)
                continue;
            const bool isAdmitted = GrantPersistenceAccess::admitBootstrap(d, human)
                && GrantPersistenceAccess::admitBootstrapAgent(d, independent, Ubiquitty::Crypto::sha256Hex("independent-session"), GrantPersistenceAccess::BootstrapConsent::Session)
                && GrantPersistenceAccess::admitBootstrapAgent(d, target, digest, GrantPersistenceAccess::BootstrapConsent::Always);
            check(isAdmitted, "the bootstrap agents and human are actually admitted");
            if (!isAdmitted)
                continue;
            const auto attachment = GrantPersistenceAccess::attachmentID(d, target);
            if (isLost)
                Ubiquitty::Host::ScreenPlaneAccess::reap(d, target);
            check(GrantPersistenceAccess::holdsStoreAuthority(d), "a live or lost bootstrap Always agent keeps credential refresh active");
            if (isMalformed)
                check(writeConsent(scratch + "/consent", Ubiquitty::Host::grantFileHeader("consent") + std::string("\0", 1)), "the bootstrap grant store becomes malformed");
            else
                check(Ubiquitty::Host::removeStandingConsent(scratch + "/consent", identity) == Ubiquitty::Host::ConsentEdit::Changed, "the bootstrap Always principal is removed externally");
            GrantPersistenceAccess::reload(d);
            GrantPersistenceAccess::retireWithdrawals(d);
            check(!Ubiquitty::Host::ScreenPlaneAccess::attachments(d).find(attachment), "withdrawing Always retires its live or lost bootstrap-agent record");
            check(GrantPersistenceAccess::isLive(d, human), "bootstrap human authority remains independent of standing agent consent");
            check(GrantPersistenceAccess::isLive(d, independent), "an independent session-granted bootstrap agent remains attached");
        }
    }

    // Approval can promote without any further inbound frame. Its newly
    // resumable attachment must already carry the exact token ceiling.
    for (const bool isNarrowing : { false, true }) {
        const std::string token = "promoted-normal-token";
        const Sync::ScopeSet initialScope { Sync::Scope::Observe, Sync::Scope::Drive };
        check(writeScopedToken(tokenPath, token, initialScope, 0), "the pending token begins with observation and input scope");
        check(writeConsent(scratch + "/consent", Ubiquitty::Host::grantFileHeader("consent")), "the promotion fixture begins without standing consent");
        Daemon d;
        GrantPersistenceAccess::useTokenStore(d, tokenPath);
        GrantPersistenceAccess::useScratchStore(d, scratch);
        std::array<int, 2> descriptors { -1, -1 };
        const auto human = GrantPersistenceAccess::addClient(d, "ubiquitty-host", "", "", &descriptors[0]);
        const auto agent = GrantPersistenceAccess::addClient(d, "/usr/bin/promoted-agent", "agent", "", &descriptors[1]);
        std::array<Ubiquitty::PAL::UnixFileDescriptor, 2> readers {
            Ubiquitty::PAL::UnixFileDescriptor(descriptors[0]), Ubiquitty::PAL::UnixFileDescriptor(descriptors[1])
        };
        const auto invalid = static_cast<std::size_t>(-1);
        const bool wereClientsCreated = human != invalid && agent != invalid;
        check(wereClientsCreated, "both pending promotion transports exist");
        if (!wereClientsCreated)
            continue;
        const bool isHumanAdmitted = GrantPersistenceAccess::admit(d, human, {});
        check(isHumanAdmitted, "the target session has an admitted approver");
        if (!isHumanAdmitted)
            continue;
        const bool pending = GrantPersistenceAccess::beginPendingAttach(d, agent, GrantPersistenceAccess::sessionID(d, human), token);
        check(pending, "the normal token starts awaiting exact attach consent");
        if (!pending)
            continue;
        check(!GrantPersistenceAccess::hasRetainedTokenGrant(d, agent), "an unapproved identity carries no retained token grant");
        const bool isApproved = GrantPersistenceAccess::holdOnceApproval(d, agent, human);
        check(isApproved, "the human allows this exact pending attachment once");
        if (!isApproved)
            continue;
        const bool isPromoted = GrantPersistenceAccess::servicePendingAttach(d, agent);
        check(isPromoted, "approval promotes through delivery without another inbound frame");
        if (!isPromoted)
            continue;
        check(GrantPersistenceAccess::hasResumableAttachment(d, agent) && GrantPersistenceAccess::hasRetainedTokenGrant(d, agent),
            "successful promotion immediately records its resumable token authority");
        if (isNarrowing) {
            check(writeScopedToken(tokenPath, token, { Sync::Scope::Observe }, 0), "the same token loses input scope before the next batch");
            check(GrantPersistenceAccess::feedRawBatch(d, agent), "the next inbound batch contains a correctly addressed Raw input frame");
            check(!GrantPersistenceAccess::isLive(d, agent) && GrantPersistenceAccess::sawRevoked(d, agent, readers[1].get()),
                "scope narrowing refuses the promoted token before its input batch is decoded");
        } else {
            check(writeScopedToken(tokenPath, token, initialScope, farFuture - 1000), "the promoted token receives an earlier future expiry");
            GrantPersistenceAccess::reverify(d);
            check(GrantPersistenceAccess::isLive(d, agent) && GrantPersistenceAccess::tokenExpiry(d, agent) == farFuture - 1000,
                "reverification shortens the promoted client's cached expiry");
            check(GrantPersistenceAccess::retainedExpiry(d, agent) == farFuture - 1000, "the retained promotion grant receives the same shortened expiry");
            check(writeScopedToken(tokenPath, token, initialScope, farFuture), "a later store write extends the token's expiry");
            GrantPersistenceAccess::reverify(d);
            check(GrantPersistenceAccess::tokenExpiry(d, agent) == farFuture - 1000 && GrantPersistenceAccess::retainedExpiry(d, agent) == farFuture - 1000,
                "neither live nor retained promoted authority is renewed by the later extension");
        }
    }

    // Unbound control and management connections retain the same token ceiling
    // in their live credential cache as their nonresumable attachment records.
    for (const auto target : { Sync::AdmissionTarget::Control, Sync::AdmissionTarget::Management }) {
        const auto scope = target == Sync::AdmissionTarget::Control ? Sync::Scope::Control : Sync::Scope::Manage;
        const std::string token = target == Sync::AdmissionTarget::Control ? "unbound-control" : "unbound-management";
        for (const bool isNarrowing : { false, true }) {
            check(writeScopedToken(tokenPath, token, { scope }, 0), "the unbound token is written without an expiry");
            check(writeConsent(scratch + "/consent", Ubiquitty::Host::grantFileHeader("consent")), "the unbound fixture's consent is whole");
            Daemon d;
            GrantPersistenceAccess::useTokenStore(d, tokenPath);
            GrantPersistenceAccess::useScratchStore(d, scratch);
            int readEnd = -1;
            const auto client = GrantPersistenceAccess::addClient(d, "/usr/bin/control-agent", "agent", "", &readEnd);
            Ubiquitty::PAL::UnixFileDescriptor output(readEnd);
            if (client == static_cast<std::size_t>(-1)) {
                check(false, "the unbound fixture transport exists");
                continue;
            }
            const bool isAdmitted = GrantPersistenceAccess::admitUnbound(d, client, token, target);
            check(isAdmitted, "the real unbound greeting is admitted");
            if (!isAdmitted)
                continue;
            check(!GrantPersistenceAccess::hasResumableAttachment(d, client), "an unbound greeting has no resumable pane attachment");
            if (isNarrowing) {
                check(writeScopedToken(tokenPath, token, { Sync::Scope::Observe }, 0), "the same digest loses its control or management scope");
                GrantPersistenceAccess::reverify(d);
                check(!GrantPersistenceAccess::isLive(d, client), "scope narrowing withdraws a live unbound authorization");
            } else {
                check(GrantPersistenceAccess::tokenExpiry(d, client) == 0, "the unbound token begins without an expiry");
                check(writeScopedToken(tokenPath, token, { scope }, farFuture - 1000), "the same digest receives a shorter future expiry");
                GrantPersistenceAccess::reverify(d);
                check(GrantPersistenceAccess::isLive(d, client) && GrantPersistenceAccess::tokenExpiry(d, client) == farFuture - 1000,
                    "reverification shortens the live unbound expiry");
                check(writeScopedToken(tokenPath, token, { scope }, farFuture), "the store later extends that digest's expiry");
                GrantPersistenceAccess::reverify(d);
                check(GrantPersistenceAccess::tokenExpiry(d, client) == farFuture - 1000, "a later store rewrite cannot renew an admitted unbound expiry");
            }
        }
    }

    // Token read failures participate in the same whole-store cache contract
    // as consent failures, even while the consent file itself is valid.
    {
        writeStore(tokenPath, { "aggregate-token" });
        const std::string consent = Ubiquitty::Host::grantFileHeader("consent") + "token:aggregate\n";
        check(writeConsent(scratch + "/consent", consent), "the aggregate control consent is whole");
        Daemon d;
        GrantPersistenceAccess::useTokenStore(d, tokenPath);
        GrantPersistenceAccess::useScratchStore(d, scratch);
        GrantPersistenceAccess::refresh(d);
        check(GrantPersistenceAccess::areStoresWhole(d) && GrantPersistenceAccess::hasStoreStamp(d), "both whole files establish a successful stamp");
        const std::string malformed = Ubiquitty::Crypto::sha256Hex("aggregate-token") + " 1 0 kept\ninvalid-record\n";
        check(writeConsent(tokenPath, malformed), "a token-store error follows a valid prefix");
        GrantPersistenceAccess::refresh(d);
        check(GrantPersistenceAccess::isConsentWhole(d) && !GrantPersistenceAccess::areStoresWhole(d), "whole consent does not hide an incomplete token read");
        check(!GrantPersistenceAccess::hasStoreStamp(d) && GrantPersistenceAccess::grants(d).empty(), "token read failure withholds cached Always and its stamp");
        GrantPersistenceAccess::grants(d).push_back("token:sentinel");
        GrantPersistenceAccess::refresh(d);
        check(GrantPersistenceAccess::grants(d).empty() && !GrantPersistenceAccess::hasStoreStamp(d), "unchanged malformed token bytes are retried without trusting a failed stamp");
        writeStore(tokenPath, { "aggregate-token" });
        GrantPersistenceAccess::refresh(d);
        check(GrantPersistenceAccess::areStoresWhole(d) && GrantPersistenceAccess::hasStoreStamp(d), "a repaired whole token read restores the aggregate status");
        check((GrantPersistenceAccess::grants(d) == std::vector<std::string> { "token:aggregate" }), "the repaired aggregate read restores only current consent");
    }

    // A held Always answer is not permission to restore a principal after
    // another connection's already-persisted grant has been withdrawn.
    [&] {
        const std::string principalToken = "pending-always-principal";
        const std::string otherToken = "pending-unrelated-principal";
        writeStore(tokenPath, { principalToken, otherToken });
        check(writeConsent(scratch + "/consent", Ubiquitty::Host::grantFileHeader("consent")), "the pending-approval fixture begins without Always consent");
        Daemon d;
        GrantPersistenceAccess::useTokenStore(d, tokenPath);
        GrantPersistenceAccess::useScratchStore(d, scratch);
        check(GrantPersistenceAccess::loadWhole(d), "the pending-approval consent is cached");
        std::array<int, 4> descriptors { -1, -1, -1, -1 };
        const auto human = GrantPersistenceAccess::addClient(d, "ubiquitty-host", "", "", &descriptors[0]);
        const auto pending = GrantPersistenceAccess::addClient(d, "/usr/bin/pending-agent", "agent", "", &descriptors[1]);
        const auto active = GrantPersistenceAccess::addClient(d, "/usr/bin/active-agent", "agent", "", &descriptors[2]);
        const auto unrelated = GrantPersistenceAccess::addClient(d, "/usr/bin/unrelated-agent", "agent", "", &descriptors[3]);
        std::array<Ubiquitty::PAL::UnixFileDescriptor, 4> readers {
            Ubiquitty::PAL::UnixFileDescriptor(descriptors[0]), Ubiquitty::PAL::UnixFileDescriptor(descriptors[1]),
            Ubiquitty::PAL::UnixFileDescriptor(descriptors[2]), Ubiquitty::PAL::UnixFileDescriptor(descriptors[3])
        };
        const auto invalid = static_cast<std::size_t>(-1);
        const bool wereClientsCreated = human != invalid && pending != invalid && active != invalid && unrelated != invalid;
        check(wereClientsCreated, "all held-approval transports exist");
        if (!wereClientsCreated)
            return;
        const bool isHumanAdmitted = GrantPersistenceAccess::admit(d, human, {});
        check(isHumanAdmitted, "the pending session has an admitted human");
        if (!isHumanAdmitted)
            return;
        const bool isPending = GrantPersistenceAccess::beginPendingAttach(d, pending, GrantPersistenceAccess::sessionID(d, human), principalToken);
        check(isPending, "the principal asks on the human's session");
        if (!isPending)
            return;
        check(GrantPersistenceAccess::holdAlwaysApproval(d, pending, human) && GrantPersistenceAccess::hasHeldApproval(d, pending), "the first connection holds an undelivered Always approval");
        const bool activeAdmitted = GrantPersistenceAccess::admit(d, active, principalToken) && GrantPersistenceAccess::admit(d, unrelated, otherToken);
        check(activeAdmitted, "the second principal connection and unrelated session are admitted independently");
        if (!activeAdmitted)
            return;
        check(GrantPersistenceAccess::sessionID(d, active) != GrantPersistenceAccess::sessionID(d, human), "the same principal's admitted connection selects another session");
        check(GrantPersistenceAccess::persist(d, active), "the other connection persists the principal's Always grant");
        GrantPersistenceAccess::retireWithdrawals(d);
        const auto identity = "token:" + Ubiquitty::Crypto::sha256Hex(principalToken);
        check(Ubiquitty::Host::removeStandingConsent(scratch + "/consent", identity) == Ubiquitty::Host::ConsentEdit::Changed, "the persisted principal is withdrawn externally");
        GrantPersistenceAccess::reload(d);
        GrantPersistenceAccess::retireWithdrawals(d);
        check(!GrantPersistenceAccess::hasHeldApproval(d, pending), "withdrawing Always voids the same principal's undelivered approval");
        check(!GrantPersistenceAccess::servicePendingAttach(d, pending), "servicing the revoked pending attach cannot promote or persist it");
        const auto current = Ubiquitty::Host::readConsentList(scratch + "/consent");
        check(current.isWhole && std::ranges::find(current.entries, identity) == current.entries.end(), "the held approval cannot resurrect the withdrawn disk principal");
        check(GrantPersistenceAccess::isLive(d, active) && GrantPersistenceAccess::isLive(d, unrelated), "independent session grants and unrelated principals remain live");
    }();

    // Two daemons sharing one control directory share its capability grants.
    // Each edit applies to the file as it is now, so one daemon's older cache
    // never writes back a grant the other withdrew, and the withdrawal ends
    // the other daemon's cached grant at its next check.
    [&] {
        using Ubiquitty::Capability;
        using Ubiquitty::CapabilityDecision;
        using Ubiquitty::CapabilityGrantKey;
        using Ubiquitty::CapabilityOrigin;
        using Ubiquitty::Host::CapabilityGrantOperation;
        const std::string grantsPath = scratch + "/capability_grants";
        ::unlink(grantsPath.c_str());
        const std::string withdrawnIdentity = "token:" + Ubiquitty::Crypto::sha256Hex("withdrawn-capability-principal");
        const CapabilityGrantKey withdrawn { Capability::ClipboardWrite, CapabilityOrigin::Agent, withdrawnIdentity };
        const CapabilityGrantKey withdrawnFileRead { Capability::FileReadAccess, CapabilityOrigin::Agent, withdrawnIdentity };
        const CapabilityGrantKey approved { Capability::ClipboardWrite, CapabilityOrigin::Agent, "token:" + Ubiquitty::Crypto::sha256Hex("approved-capability-principal") };
        const CapabilityGrantKey unrelated { Capability::FileReadAccess, CapabilityOrigin::Agent, "token:" + Ubiquitty::Crypto::sha256Hex("unrelated-capability-principal") };

        Daemon first;
        Daemon second;
        GrantPersistenceAccess::useScratchStore(first, scratch);
        GrantPersistenceAccess::useScratchStore(second, scratch);
        check(GrantPersistenceAccess::editCapabilityGrants(first, CapabilityGrantOperation::Grant, withdrawn)
                && GrantPersistenceAccess::editCapabilityGrants(first, CapabilityGrantOperation::Grant, withdrawnFileRead)
                && GrantPersistenceAccess::editCapabilityGrants(first, CapabilityGrantOperation::Grant, unrelated),
            "the first daemon records two principals' Always grants");
        GrantPersistenceAccess::loadCapabilityGrants(second);
        check(GrantPersistenceAccess::isCapabilityCached(first, withdrawn) && GrantPersistenceAccess::isCapabilityCached(second, withdrawn),
            "both daemons hold the grant that is about to be withdrawn");
        check(GrantPersistenceAccess::editCapabilityGrants(second, CapabilityGrantOperation::WithdrawIdentity, withdrawn),
            "the second daemon withdraws every grant its principal holds");
        check(!GrantPersistenceAccess::isCapabilityCached(second, withdrawn) && !GrantPersistenceAccess::isCapabilityCached(second, withdrawnFileRead)
                && GrantPersistenceAccess::isCapabilityCached(second, unrelated),
            "the withdrawing daemon keeps only the unrelated principal's grant");
        check(GrantPersistenceAccess::isCapabilityCached(first, withdrawn),
            "the first daemon's cache still lists the withdrawn grant, since it has not checked since");

        check(GrantPersistenceAccess::editCapabilityGrants(first, CapabilityGrantOperation::Grant, approved),
            "the first daemon records an unrelated approval from its older cache");
        check(!GrantPersistenceAccess::isCapabilityCached(first, withdrawn) && !GrantPersistenceAccess::isCapabilityCached(first, withdrawnFileRead)
                && GrantPersistenceAccess::isCapabilityCached(first, approved) && GrantPersistenceAccess::isCapabilityCached(first, unrelated),
            "the approval adopts the file as it is, not the cache it was made from");
        Daemon reader;
        GrantPersistenceAccess::useScratchStore(reader, scratch);
        GrantPersistenceAccess::loadCapabilityGrants(reader);
        check(!GrantPersistenceAccess::isCapabilityCached(reader, withdrawn) && !GrantPersistenceAccess::isCapabilityCached(reader, withdrawnFileRead),
            "a later reader never recovers the withdrawn grants");
        check(GrantPersistenceAccess::isCapabilityCached(reader, approved) && GrantPersistenceAccess::isCapabilityCached(reader, unrelated),
            "a later reader finds the approval and the unrelated principal");

        // A withdrawal elsewhere ends a confirmed grant at this daemon's next check.
        check(GrantPersistenceAccess::checkCapability(reader, approved) == CapabilityDecision::Allow, "the reader's confirmed Always grant authorizes");
        check(GrantPersistenceAccess::editCapabilityGrants(second, CapabilityGrantOperation::Withdraw, approved),
            "the second daemon withdraws exactly the approval");
        check(GrantPersistenceAccess::isCapabilityCached(reader, approved), "the reader's cache lists the approval until it checks");
        check(GrantPersistenceAccess::checkCapability(reader, approved) == CapabilityDecision::Prompt,
            "the reader's next check asks again rather than honouring the withdrawn grant");
        check(!GrantPersistenceAccess::isCapabilityCached(reader, approved) && GrantPersistenceAccess::isCapabilityCached(reader, unrelated),
            "that check adopted the file, which still holds the unrelated grant");
        check(GrantPersistenceAccess::checkCapability(reader, unrelated) == CapabilityDecision::Allow, "the unrelated grant still authorizes");

        // Grants handed over from a predecessor are its cache, not the file:
        // each is confirmed before it authorizes.
        Ubiquitty::CapabilityGrantStore predecessor;
        predecessor.grant(approved.capability, approved.origin, approved.identity, Ubiquitty::CapabilityGrantScope::Always);
        predecessor.grant(unrelated.capability, unrelated.origin, unrelated.identity, Ubiquitty::CapabilityGrantScope::Always);
        GrantPersistenceAccess::handOverCapabilityGrants(reader, std::move(predecessor));
        check(GrantPersistenceAccess::checkCapability(reader, approved) == CapabilityDecision::Prompt,
            "a handed-over grant the file no longer holds does not authorize");
        check(GrantPersistenceAccess::checkCapability(reader, unrelated) == CapabilityDecision::Allow,
            "a handed-over grant the file holds still does");

        // A file that cannot be read whole grants nothing from the next check on.
        const std::string malformed = Ubiquitty::Host::grantFileHeader("capability-grants") + std::to_string(std::to_underlying(unrelated.capability)) + "\t"
            + std::to_string(std::to_underlying(unrelated.origin)) + "\t" + unrelated.identity + "\nnot a grant\n";
        check(writeConsent(grantsPath, malformed), "the grant file is damaged in place");
        check(GrantPersistenceAccess::checkCapability(reader, unrelated) == CapabilityDecision::Prompt,
            "a damaged file withdraws every cached Always grant");
        check(!GrantPersistenceAccess::editCapabilityGrants(first, CapabilityGrantOperation::Withdraw, unrelated),
            "a withdrawal against a damaged file is reported as failed");
        check(GrantPersistenceAccess::isCapabilityCached(first, unrelated), "and leaves the editing daemon's grants as they were");
        check(GrantPersistenceAccess::checkCapability(first, unrelated) == CapabilityDecision::Prompt,
            "until its own next check finds the damaged file");
    }();

    ::unlink((scratch + "/consent").c_str());
    ::unlink((scratch + "/consent.lock").c_str());
    ::unlink(tokenPath.c_str());
    ::unlink((scratch + "/capability_grants").c_str());
    ::unlink((scratch + "/capability_grants.lock").c_str());
    ::unlink((tokenPath + ".lock").c_str());
    ::unlink((tokenPath + ".metadata").c_str());
    ::rmdir(scratch.c_str());

    if (failures == 0) {
        std::printf("grant_persistence: a withdrawn token disconnects its holders and nobody else "
                    "(asserted with several interleaved principals); "
                    "grant keys come from the live client, prefer the token digest, deduplicate, "
                    "cap at the oldest, and round-trip through disk\n");
        return 0;
    }
    std::fprintf(stderr, "grant_persistence: %d failure(s)\n", failures);
    return 1;
}
