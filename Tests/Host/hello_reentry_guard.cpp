// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// One physical connection establishes its identity once. A Control-only
// credential cannot acquire observation authority by greeting again without
// a credential and naming an unattended session.

#include "Assertions.h"
#include "HostConnection.h"
#include "ScreenPlaneAccess.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/TokenRecord.h"
#include "Platform/Host/TokenStore.h"

#include <fcntl.h>
#include <sys/socket.h>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty::Host {

struct HelloReentryGuardAccess {
    static bool helloSeen(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].helloSeen; }
    static bool hasToken(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].hasToken; }
    static Protocol::Sync::ScopeSet tokenScope(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].tokenScope; }
    static bool didOverflow(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].didOverflow; }
    static std::uint32_t clientSessionID(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].sessionID; }
    static std::size_t sessionCount(const Daemon& daemon) { return daemon.m_sessions.size(); }
};

}

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
using Access = Ubiquitty::Host::HelloReentryGuardAccess;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
using Ubiquitty::Host::Daemon;

struct Peer {
    std::size_t index;
    Ubiquitty::PAL::UnixFileDescriptor socket;
    Ubiquitty::HostTest::Connection reader;
};

std::optional<Peer> connectPeer(Daemon& daemon)
{
    std::array<int, 2> ends { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()))
        return std::nullopt;
    if (::fcntl(ends[0], F_SETFL, O_NONBLOCK) < 0) {
        ::close(ends[0]);
        ::close(ends[1]);
        return std::nullopt;
    }
    // No Local-origin override: only the real stored credential admits this peer.
    const auto index = Screen::connect(daemon, Ubiquitty::PAL::UnixFileDescriptor { ends[0] });
    Peer peer { .index = index, .socket = Ubiquitty::PAL::UnixFileDescriptor { ends[1] }, .reader = {} };
    peer.reader.fileDescriptor = peer.socket.get();
    return peer;
}

Sync::Greet controlGreeting(Daemon& daemon, std::uint64_t identity, std::string_view token)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = identity, .high = 0x7265656e747279 };
    greeting.clientKind = Sync::ClientKind::Agent;
    greeting.requestedRole = Sync::Role::Agent;
    greeting.request.target = Sync::AdmissionTarget::Control;
    greeting.request.expectedDaemonInstanceID = Screen::daemonInstanceID(daemon);
    greeting.credential = std::vector<std::uint8_t>(token.begin(), token.end());
    greeting.facts.viewportRows = 24;
    greeting.facts.viewportColumns = 80;
    return greeting;
}

std::span<const std::uint8_t> bytes(const std::string& body)
{
    return { reinterpret_cast<const std::uint8_t*>(body.data()), body.size() };
}

void admitControl(Daemon& daemon, Peer& peer, const Sync::Greet& greeting)
{
    const auto frame = Ubiquitty::HostTest::greetFrame(greeting);
    REQUIRE(!frame.empty());
    peer.reader.greeting = greeting;
    Screen::receive(daemon, peer.index, bytes(frame));
    Screen::service(daemon);
    Screen::flush(daemon, peer.index);
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer.reader, Sync::FrameKind::Welcome, answer, 2000));
    const auto welcome = Sync::decodeWelcome(bytes(answer));
    REQUIRE(welcome);
    CHECK(welcome->admission == Sync::AttachmentAdmission::Granted);
    CHECK(welcome->selection.target == Sync::AdmissionTarget::Control);
    CHECK_EQ(welcome->selection.daemonInstanceID, greeting.request.expectedDaemonInstanceID);
    CHECK_EQ(welcome->selection.sessionID, 0u);
    CHECK(Access::helloSeen(daemon, peer.index));
    CHECK(Access::hasToken(daemon, peer.index));
    CHECK(Access::tokenScope(daemon, peer.index) == Sync::ScopeSet { Sync::Scope::Control });
    CHECK(!Access::tokenScope(daemon, peer.index).contains(Sync::Scope::Observe));
    CHECK(!peer.reader.hasFailed);
}

void aSecondGreetingCannotEscalate(Daemon& daemon, std::string_view token)
{
    const auto sessionID = Screen::makeSession(daemon, 24, 80).id;
    const auto before = Access::sessionCount(daemon);
    auto peer = connectPeer(daemon);
    REQUIRE(peer);
    auto greeting = controlGreeting(daemon, 1, token);
    admitControl(daemon, *peer, greeting);
    REQUIRE(peer->reader.welcome);
    const auto* connection = Screen::connection(daemon, peer->index);
    REQUIRE(connection && connection->state() == Sync::ConnectionState::Attached);

    greeting.request.target = Sync::AdmissionTarget::Session;
    greeting.request.sessionID = sessionID;
    greeting.request.isAttachOnly = true;
    greeting.credential.reset();
    const auto duplicate = Ubiquitty::HostTest::greetFrame(greeting);
    REQUIRE(!duplicate.empty());
    // Bypass the fixture's send-once API: the case is a hostile peer that greets twice.
    Screen::receive(daemon, peer->index, bytes(duplicate));
    CHECK(connection->state() == Sync::ConnectionState::Closed);
    CHECK(connection->byeReason() == Sync::ByeReason::Malformed);
    CHECK_EQ(Access::clientSessionID(daemon, peer->index), 0u);
    CHECK(Access::tokenScope(daemon, peer->index) == Sync::ScopeSet { Sync::Scope::Control });
    CHECK_EQ(Access::sessionCount(daemon), before);
    Screen::service(daemon);
    Screen::flush(daemon, peer->index);
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer->reader, Sync::FrameKind::Bye, answer, 2000));
    const auto bye = Sync::decodeBye(bytes(answer));
    REQUIRE(bye);
    CHECK(bye->reason == Sync::ByeReason::Malformed);
    CHECK(Access::didOverflow(daemon, peer->index));
    CHECK(peer->reader.panes.empty());
    CHECK(!peer->reader.hasFailed);
}

void aRepeatedGreetingHeaderAllocatesNoBody(Daemon& daemon, std::string_view token)
{
    auto peer = connectPeer(daemon);
    REQUIRE(peer);
    const auto greeting = controlGreeting(daemon, 2, token);
    admitControl(daemon, *peer, greeting);
    REQUIRE(peer->reader.welcome);
    const auto* connection = Screen::connection(daemon, peer->index);
    REQUIRE(connection && connection->state() == Sync::ConnectionState::Attached);
    const auto heldBefore = connection->inboundFrameBytes();

    std::vector<std::uint8_t> body(Sync::maxFrameBodyBytes, 0);
    std::vector<std::uint8_t> frame;
    REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Greet, 0, body));
    const auto headerBytes = frame.size() - body.size();
    Screen::receive(daemon, peer->index, std::span(frame).first(headerBytes));
    CHECK(connection->state() == Sync::ConnectionState::Closed);
    CHECK(connection->byeReason() == Sync::ByeReason::Malformed);
    CHECK(connection->inboundFrameBytes() <= heldBefore);
    CHECK_EQ(connection->pendingInboundFrameBytes(), 0u);
    CHECK_EQ(Access::clientSessionID(daemon, peer->index), 0u);
    CHECK(Access::tokenScope(daemon, peer->index) == Sync::ScopeSet { Sync::Scope::Control });

    // Completing the rejected body and a coalesced Call cannot revive dispatch.
    std::vector<std::uint8_t> tail(body);
    body.clear();
    Sync::encode(body, Sync::Call { .text = R"({"jsonrpc":"2.0","id":"after-duplicate","method":"host.describe","params":{}})" });
    REQUIRE(Sync::appendFrame(tail, Sync::FrameKind::Call, 0, body));
    Screen::receive(daemon, peer->index, tail);
    CHECK(connection->inboundFrameBytes() <= heldBefore);
    Screen::service(daemon);
    Screen::flush(daemon, peer->index);
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer->reader, Sync::FrameKind::Bye, answer, 2000));
    const auto bye = Sync::decodeBye(bytes(answer));
    REQUIRE(bye);
    CHECK(bye->reason == Sync::ByeReason::Malformed);
    CHECK(!Ubiquitty::HostTest::waitFor(peer->reader, Sync::FrameKind::Result, answer, 100));
    CHECK(!peer->reader.hasFailed);
}

void aFreshConnectionStillAdmitsItsFirstGreeting(Daemon& daemon, std::string_view token)
{
    auto peer = connectPeer(daemon);
    REQUIRE(peer);
    admitControl(daemon, *peer, controlGreeting(daemon, 3, token));
    REQUIRE(peer->reader.welcome);
    CHECK(!Access::didOverflow(daemon, peer->index));
    CHECK_EQ(Access::clientSessionID(daemon, peer->index), 0u);
}

}

int main()
{
    char tokenPath[] = "/tmp/hello_reentry_guard_tokens.XXXXXX";
    const int descriptor = ::mkstemp(tokenPath);
    if (descriptor < 0)
        return 1;
    ::close(descriptor);
    const std::string plaintext = "test-control-only-token";
    Sync::TokenRecord record;
    record.hashHex = Ubiquitty::Crypto::sha256Hex(plaintext);
    record.scope = Sync::ScopeSet { Sync::Scope::Control };
    record.label = "control-only";
    const auto written = Ubiquitty::Host::writeTokens(tokenPath, { record });
    CHECK(written == Ubiquitty::Host::TokenWriteOutcome::Durable);
    if (written == Ubiquitty::Host::TokenWriteOutcome::Durable) {
        Daemon daemon;
        Screen::useTokenStore(daemon, tokenPath);
        aSecondGreetingCannotEscalate(daemon, plaintext);
        aRepeatedGreetingHeaderAllocatesNoBody(daemon, plaintext);
        aFreshConnectionStillAdmitsItsFirstGreeting(daemon, plaintext);
    }
    ::unlink(tokenPath);
    return UBIQUITTY_TEST_SUMMARY();
}
