// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Canonical admission stamps no authority wider than a verified credential or
// carried bootstrap scope. Requiring a token exempts only the trusted local
// owner, not a remote peer or a local binary claiming to be an agent.

#include "Assertions.h"
#include "Helpers/TempDir.h"
#include "HostConnection.h"
#include "ScreenPlaneAccess.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/TokenRecord.h"
#include "Platform/Host/TokenStore.h"

#include <fcntl.h>
#include <sys/socket.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Host {

struct HelloAuthAccess {
    static void requireToken(Daemon& daemon) { daemon.m_requireToken = true; }
    static void permitMissingToken(Daemon& daemon) { daemon.m_requireToken = false; }
    static Protocol::Sync::ScopeSet stampedScope(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].tokenScope; }
    static bool hasToken(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].hasToken; }
    static bool helloSeen(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].helloSeen; }
    static std::size_t clientCount(const Daemon& daemon) { return daemon.m_clients.size(); }
};

}

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
using Access = Ubiquitty::Host::HelloAuthAccess;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
using Ubiquitty::Host::Daemon;

enum class Origin : std::uint8_t {
    Remote,
    TrustedLocal
};

struct Peer {
    std::size_t index;
    Ubiquitty::PAL::UnixFileDescriptor socket;
    Ubiquitty::HostTest::Connection reader;
};

std::optional<Peer> connectPeer(Daemon& daemon, Origin origin = Origin::Remote)
{
    std::array<int, 2> ends { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()))
        return std::nullopt;
    if (::fcntl(ends[0], F_SETFL, O_NONBLOCK) < 0) {
        ::close(ends[0]);
        ::close(ends[1]);
        return std::nullopt;
    }
    const auto index = Screen::connect(daemon, Ubiquitty::PAL::UnixFileDescriptor { ends[0] });
    if (origin == Origin::TrustedLocal)
        Screen::markTrustedLocal(daemon, index);
    Peer peer { .index = index, .socket = Ubiquitty::PAL::UnixFileDescriptor { ends[1] }, .reader = {} };
    peer.reader.fileDescriptor = peer.socket.get();
    return peer;
}

Sync::Greet greetingFor(Daemon& daemon, std::uint32_t sessionID, std::string_view token = {})
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = 1, .high = 0x61757468 };
    greeting.request.target = sessionID ? Sync::AdmissionTarget::Session : Sync::AdmissionTarget::Control;
    greeting.request.sessionID = sessionID;
    greeting.request.isAttachOnly = sessionID != 0;
    greeting.request.expectedDaemonInstanceID = Screen::daemonInstanceID(daemon);
    if (!token.empty())
        greeting.credential = std::vector<std::uint8_t>(token.begin(), token.end());
    greeting.facts.viewportRows = 24;
    greeting.facts.viewportColumns = 80;
    return greeting;
}

std::span<const std::uint8_t> bytes(const std::string& body)
{
    return { reinterpret_cast<const std::uint8_t*>(body.data()), body.size() };
}

void deliverBody(Daemon& daemon, Peer& peer, const Sync::Greet& greeting, std::span<const std::uint8_t> body)
{
    std::vector<std::uint8_t> frame;
    REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Greet, 0, body));
    peer.reader.greeting = greeting;
    Screen::receive(daemon, peer.index, frame);
    Screen::service(daemon);
    Screen::flush(daemon, peer.index);
}

void deliver(Daemon& daemon, Peer& peer, const Sync::Greet& greeting)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, greeting);
    REQUIRE(Sync::decodeGreet(body));
    deliverBody(daemon, peer, greeting, body);
}

void checkRefused(Peer& peer, Sync::ByeReason reason, std::string_view detail = {})
{
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer.reader, Sync::FrameKind::Bye, answer, 2000));
    const auto bye = Sync::decodeBye(bytes(answer));
    REQUIRE(bye);
    CHECK(bye->reason == reason);
    CHECK_MSG(detail.empty() || bye->detail == detail, bye->detail.c_str());
    CHECK(!peer.reader.welcome);
    CHECK(peer.reader.panes.empty());
    CHECK(!peer.reader.hasFailed);
}

void checkGranted(Peer& peer, Sync::AdmissionTarget target, std::uint32_t sessionID = 0)
{
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer.reader, Sync::FrameKind::Welcome, answer, 2000));
    const auto welcome = Sync::decodeWelcome(bytes(answer));
    REQUIRE(welcome);
    CHECK(welcome->admission == Sync::AttachmentAdmission::Granted);
    CHECK(welcome->selection.target == target);
    CHECK_EQ(welcome->selection.sessionID, sessionID);
    CHECK_EQ(welcome->selection.daemonInstanceID, peer.reader.greeting->request.expectedDaemonInstanceID);
    CHECK(!peer.reader.hasFailed);
}

bool storeToken(const std::string& path, Sync::ScopeSet scope, std::uint64_t expiry, std::string_view plaintext)
{
    Sync::TokenRecord record;
    record.hashHex = Ubiquitty::Crypto::sha256Hex(std::string(plaintext));
    record.scope = scope;
    record.expiryUnix = expiry;
    record.label = "auth-gate";
    return Ubiquitty::Host::writeTokens(path, { record }) == Ubiquitty::Host::TokenWriteOutcome::Durable;
}

void storedCredentialsKeepTheirExactScope(const std::string& path)
{
    constexpr std::uint64_t farFuture = 4'102'444'800;
    {
        REQUIRE(storeToken(path, { Sync::Scope::Observe }, farFuture, "real-token"));
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        Access::requireToken(daemon);
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        const auto sessionID = Screen::makeSession(daemon, 24, 80).id;
        deliver(daemon, *peer, greetingFor(daemon, sessionID, "unknown-token"));
        checkRefused(*peer, Sync::ByeReason::TokenInvalid);
        CHECK(!Access::hasToken(daemon, peer->index));
    }
    {
        REQUIRE(storeToken(path, { Sync::Scope::Observe }, 1, "expired-token"));
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        Access::requireToken(daemon);
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        const auto sessionID = Screen::makeSession(daemon, 24, 80).id;
        deliver(daemon, *peer, greetingFor(daemon, sessionID, "expired-token"));
        checkRefused(*peer, Sync::ByeReason::TokenExpired);
        CHECK(!Access::hasToken(daemon, peer->index));
    }
    for (const auto scope : { Sync::Scope::Observe, Sync::Scope::Control }) {
        REQUIRE(storeToken(path, { scope }, farFuture, "one-scope"));
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        Access::requireToken(daemon);
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        const auto sessionID = scope == Sync::Scope::Control ? Screen::makeSession(daemon, 24, 80).id : 0;
        deliver(daemon, *peer, greetingFor(daemon, sessionID, "one-scope"));
        // The refusal says what the token carries and what it would need, so
        // whoever issues tokens can issue the right one.
        checkRefused(*peer, Sync::ByeReason::OutsideScope,
            scope == Sync::Scope::Control ? "This token carries control; attaching to a session needs observe."
                                          : "This token carries observe; a control connection, which lists and ends sessions, needs control.");
        CHECK(Access::stampedScope(daemon, peer->index) == Sync::ScopeSet { scope });
    }
    {
        REQUIRE(storeToken(path, { Sync::Scope::Control }, farFuture, "control-ok"));
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        Access::requireToken(daemon);
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        deliver(daemon, *peer, greetingFor(daemon, 0, "control-ok"));
        checkGranted(*peer, Sync::AdmissionTarget::Control);
        CHECK(Access::hasToken(daemon, peer->index));
        CHECK(Access::stampedScope(daemon, peer->index) == Sync::ScopeSet { Sync::Scope::Control });
    }
    {
        REQUIRE(storeToken(path, { Sync::Scope::Observe }, farFuture, "observe-ok"));
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        Access::requireToken(daemon);
        auto& session = Screen::makeSession(daemon, 24, 80);
        // Owner consent is an independent prerequisite, not another scope bit.
        (void)session.addSessionGrant("token:" + Ubiquitty::Crypto::sha256Hex("observe-ok"));
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        deliver(daemon, *peer, greetingFor(daemon, session.id, "observe-ok"));
        checkGranted(*peer, Sync::AdmissionTarget::Session, session.id);
        CHECK(Access::stampedScope(daemon, peer->index) == Sync::ScopeSet { Sync::Scope::Observe });
        CHECK(Screen::isAgent(daemon, peer->index));
    }
}

void carriedScopeIsNotWidened(const std::string& path)
{
    for (const auto scope : { Sync::Scope::Observe, Sync::Scope::Control }) {
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        Access::requireToken(daemon);
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        Screen::grantScope(daemon, peer->index, { scope });
        Screen::setTokenID(daemon, peer->index, "verified-preamble-digest");
        Screen::markBootstrapToken(daemon, peer->index);
        Screen::markQUICAuthenticated(daemon, peer->index);
        deliver(daemon, *peer, greetingFor(daemon, 0));
        if (scope == Sync::Scope::Control)
            checkGranted(*peer, Sync::AdmissionTarget::Control);
        else
            checkRefused(*peer, Sync::ByeReason::OutsideScope);
        CHECK(Access::stampedScope(daemon, peer->index) == Sync::ScopeSet { scope });
        CHECK(!Access::stampedScope(daemon, peer->index).contains(Sync::Scope::NewSession));
    }
}

void aTokenRequirementExemptsOnlyTheTrustedOwner(const std::string& path)
{
    for (const auto origin : { Origin::Remote, Origin::TrustedLocal }) {
        for (const auto& label : { std::string {}, std::string { "mcp" } }) {
            Daemon daemon;
            Screen::useTokenStore(daemon, path);
            Access::requireToken(daemon);
            auto peer = connectPeer(daemon, origin);
            REQUIRE(peer);
            auto greeting = greetingFor(daemon, 0);
            greeting.request.agentLabel = label;
            deliver(daemon, *peer, greeting);
            if (origin == Origin::TrustedLocal && label.empty())
                checkGranted(*peer, Sync::AdmissionTarget::Control);
            else
                checkRefused(*peer, Sync::ByeReason::TokenMissing);
            CHECK(!Access::hasToken(daemon, peer->index));
        }
    }
}

// UBIQUITTY_REQUIRE_TOKEN=0 turns the requirement off for a same-user local
// agent, which is then held to what an `all` token carries. A Remote peer still
// needs a token, and the switch grants no scope an `all` token lacks.
void turningTheRequirementOffAdmitsOnlyALocalAgent(const std::string& path)
{
    CHECK(Ubiquitty::Host::requiresTokenFor(nullptr));
    CHECK(Ubiquitty::Host::requiresTokenFor("1"));
    CHECK(Ubiquitty::Host::requiresTokenFor(""));
    CHECK(Ubiquitty::Host::requiresTokenFor("00"));
    CHECK(Ubiquitty::Host::requiresTokenFor("off"));
    CHECK(!Ubiquitty::Host::requiresTokenFor("0"));
    {
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        Access::permitMissingToken(daemon);
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        deliver(daemon, *peer, greetingFor(daemon, 0));
        checkRefused(*peer, Sync::ByeReason::TokenMissing);
        CHECK(!Access::hasToken(daemon, peer->index));
    }
    for (const auto requiresToken : { true, false }) {
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        if (requiresToken)
            Access::requireToken(daemon);
        else
            Access::permitMissingToken(daemon);
        auto peer = connectPeer(daemon, Origin::TrustedLocal);
        REQUIRE(peer);
        auto greeting = greetingFor(daemon, 0);
        greeting.request.agentLabel = "mcp";
        deliver(daemon, *peer, greeting);
        if (requiresToken)
            checkRefused(*peer, Sync::ByeReason::TokenMissing);
        else
            checkGranted(*peer, Sync::AdmissionTarget::Control);
        CHECK(Screen::isAgent(daemon, peer->index));
        CHECK(!Access::hasToken(daemon, peer->index));
    }
    {
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        Access::permitMissingToken(daemon);
        auto peer = connectPeer(daemon, Origin::TrustedLocal);
        REQUIRE(peer);
        auto greeting = greetingFor(daemon, 0);
        greeting.request.agentLabel = "mcp";
        greeting.request.target = Sync::AdmissionTarget::Management;
        deliver(daemon, *peer, greeting);
        checkRefused(*peer, Sync::ByeReason::OutsideScope);
    }
}

void malformedAndVersionRefusalsPrecedeAuthentication(const std::string& path)
{
    // Half a version, and a version of zero, name no version at all.
    for (const std::vector<std::uint8_t>& body : { std::vector<std::uint8_t> { 1 }, std::vector<std::uint8_t> { 0, 0 } }) {
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        Access::requireToken(daemon);
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        deliverBody(daemon, *peer, greetingFor(daemon, 0), body);
        checkRefused(*peer, Sync::ByeReason::Malformed);
        CHECK(!Access::helloSeen(daemon, peer->index));
        CHECK(!Access::hasToken(daemon, peer->index));
    }
    {
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        Access::requireToken(daemon);
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        auto future = greetingFor(daemon, 0);
        future.wireVersion = Sync::wireVersion + 1;
        deliver(daemon, *peer, future);
        checkRefused(*peer, Sync::ByeReason::VersionMismatch);
        CHECK(!Access::helloSeen(daemon, peer->index));
        CHECK(!Access::hasToken(daemon, peer->index));
    }
    REQUIRE(storeToken(path, { Sync::Scope::Control }, 0, "truncation-token"));
    Daemon encoderOwner;
    const auto greeting = greetingFor(encoderOwner, 0, "truncation-token");
    std::vector<std::uint8_t> fullBody;
    Sync::encode(fullBody, greeting);
    REQUIRE(Sync::decodeGreet(fullBody));
    for (std::size_t cut = 0; cut < fullBody.size(); ++cut) {
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        Access::requireToken(daemon);
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        const auto exact = greetingFor(daemon, 0, "truncation-token");
        std::vector<std::uint8_t> exactBody;
        Sync::encode(exactBody, exact);
        REQUIRE(Sync::decodeGreet(exactBody) && exactBody.size() == fullBody.size());
        deliverBody(daemon, *peer, exact, std::span(exactBody).first(cut));
        checkRefused(*peer, Sync::ByeReason::Malformed);
        CHECK(!Access::helloSeen(daemon, peer->index));
        CHECK(!Access::hasToken(daemon, peer->index));
    }
}

void anOutOfRangeClientDoesNotTouchItsNeighbor()
{
    Daemon daemon;
    auto peer = connectPeer(daemon);
    REQUIRE(peer);
    const auto before = Access::clientCount(daemon);
    const auto frame = Ubiquitty::HostTest::greetFrame(greetingFor(daemon, 0));
    REQUIRE(!frame.empty());
    Screen::receive(daemon, before, bytes(frame));
    CHECK_EQ(Access::clientCount(daemon), before);
    CHECK(!Access::helloSeen(daemon, peer->index));
    CHECK(!Access::hasToken(daemon, peer->index));
    CHECK(!Screen::connection(daemon, peer->index));
    CHECK(Screen::takeOutput(daemon, peer->index).empty());
}

}

int main()
{
    Ubiquitty::Test::TempDir directory;
    if (!directory.valid())
        return 1;
    const auto path = directory.path() + "/tokens";
    storedCredentialsKeepTheirExactScope(path);
    carriedScopeIsNotWidened(path);
    aTokenRequirementExemptsOnlyTheTrustedOwner(path);
    turningTheRequirementOffAdmitsOnlyALocalAgent(path);
    malformedAndVersionRefusalsPrecedeAuthentication(path);
    anOutOfRangeClientDoesNotTouchItsNeighbor();
    return UBIQUITTY_TEST_SUMMARY();
}
