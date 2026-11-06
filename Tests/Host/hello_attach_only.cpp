// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// An existing-session request never creates a replacement. Both the wire
// verdict and the session registry are checked, independently of token policy.

#include "Assertions.h"
#include "HostConnection.h"
#include "ScreenPlaneAccess.h"

#include "Core/Protocol/Sync/Wire.h"

#include <fcntl.h>
#include <sys/socket.h>

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty::Host {

struct HelloAttachOnlyAccess {
    static std::size_t sessionCount(const Daemon& daemon) { return daemon.m_sessions.size(); }
    static std::uint32_t clientSessionID(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].sessionID; }
};

}

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
using Access = Ubiquitty::Host::HelloAttachOnlyAccess;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
using Ubiquitty::Host::Daemon;

constexpr std::uint32_t vanishedSessionID = 4242;

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
    const auto index = Screen::connect(daemon, Ubiquitty::PAL::UnixFileDescriptor { ends[0] });
    Screen::markTrustedLocal(daemon, index);
    Peer peer { .index = index, .socket = Ubiquitty::PAL::UnixFileDescriptor { ends[1] }, .reader = {} };
    peer.reader.fileDescriptor = peer.socket.get();
    return peer;
}

Sync::Greet existingSession(Daemon& daemon, std::uint32_t sessionID)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = 1, .high = 0x617474616368 };
    greeting.request = { .target = Sync::AdmissionTarget::Session,
        .sessionID = sessionID,
        .expectedDaemonInstanceID = Screen::daemonInstanceID(daemon),
        .isAttachOnly = true };
    greeting.facts.viewportRows = 24;
    greeting.facts.viewportColumns = 80;
    return greeting;
}

void deliver(Daemon& daemon, Peer& peer, const Sync::Greet& greeting, std::span<const std::uint8_t> body)
{
    std::vector<std::uint8_t> frame;
    REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Greet, 0, body));
    peer.reader.greeting = greeting;
    Screen::receive(daemon, peer.index, frame);
    Screen::service(daemon);
    Screen::flush(daemon, peer.index);
}

std::span<const std::uint8_t> bytes(const std::string& body)
{
    return { reinterpret_cast<const std::uint8_t*>(body.data()), body.size() };
}

void aVanishedSessionIsNotRecreated()
{
    Daemon daemon;
    auto peer = connectPeer(daemon);
    REQUIRE(peer);
    const auto before = Access::sessionCount(daemon);
    const auto greeting = existingSession(daemon, vanishedSessionID);
    std::vector<std::uint8_t> body;
    Sync::encode(body, greeting);
    REQUIRE(Sync::decodeGreet(body));
    deliver(daemon, *peer, greeting, body);
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer->reader, Sync::FrameKind::Bye, answer, 2000));
    const auto bye = Sync::decodeBye(bytes(answer));
    REQUIRE(bye);
    CHECK(bye->reason == Sync::ByeReason::SessionGone);
    CHECK(!peer->reader.welcome);
    CHECK_EQ(Access::sessionCount(daemon), before);
    CHECK_EQ(Access::clientSessionID(daemon, peer->index), 0u);
}

void anExistingSessionKeepsItsIdentity()
{
    Daemon daemon;
    const auto sessionID = Screen::makeSession(daemon, 24, 80).id;
    const auto before = Access::sessionCount(daemon);
    auto peer = connectPeer(daemon);
    REQUIRE(peer);
    const auto greeting = existingSession(daemon, sessionID);
    std::vector<std::uint8_t> body;
    Sync::encode(body, greeting);
    REQUIRE(Sync::decodeGreet(body));
    deliver(daemon, *peer, greeting, body);
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer->reader, Sync::FrameKind::Welcome, answer, 2000));
    const auto welcome = Sync::decodeWelcome(bytes(answer));
    REQUIRE(welcome);
    CHECK(welcome->admission == Sync::AttachmentAdmission::Granted);
    CHECK(welcome->selection.target == Sync::AdmissionTarget::Session);
    CHECK_EQ(welcome->selection.sessionID, sessionID);
    CHECK_EQ(welcome->selection.daemonInstanceID, greeting.request.expectedDaemonInstanceID);
    CHECK_EQ(Access::clientSessionID(daemon, peer->index), sessionID);
    CHECK_EQ(Access::sessionCount(daemon), before);
    CHECK(!peer->reader.hasFailed);
}

void aNonzeroSessionRequiresAttachOnly()
{
    Daemon daemon;
    auto peer = connectPeer(daemon);
    REQUIRE(peer);
    const auto before = Access::sessionCount(daemon);
    auto greeting = existingSession(daemon, vanishedSessionID);
    std::vector<std::uint8_t> body;
    Sync::encode(body, greeting);
    REQUIRE(Sync::decodeGreet(body));

    // Mutate only the attach-only byte in a complete production encoding.
    Sync::WireReader reader(body);
    (void)reader.u16();
    (void)reader.utf8(Sync::maxAppVersionBytes);
    (void)reader.take(greeting.engineFingerprint.size() + 16 + 2);
    (void)reader.u8();
    (void)reader.u32();
    (void)reader.u32();
    (void)reader.u64();
    (void)reader.utf8(Sync::maxFrameBodyBytes);
    (void)reader.utf8(Sync::maxFrameBodyBytes);
    (void)reader.utf8(Sync::maxFrameBodyBytes);
    REQUIRE(reader.ok() && reader.remaining());
    body[reader.offset()] = 0;
    CHECK(!Sync::decodeGreet(body));
    greeting.request.isAttachOnly = false;
    std::vector<std::uint8_t> refusedEncoding;
    Sync::encode(refusedEncoding, greeting);
    CHECK(refusedEncoding.empty());

    deliver(daemon, *peer, greeting, body);
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer->reader, Sync::FrameKind::Bye, answer, 2000));
    const auto bye = Sync::decodeBye(bytes(answer));
    REQUIRE(bye);
    CHECK(bye->reason == Sync::ByeReason::Malformed);
    CHECK(!peer->reader.welcome);
    CHECK_EQ(Access::sessionCount(daemon), before);
    CHECK_EQ(Access::clientSessionID(daemon, peer->index), 0u);

    greeting.request.sessionID = 0;
    Sync::encode(refusedEncoding, greeting);
    const auto fresh = Sync::decodeGreet(refusedEncoding);
    REQUIRE(fresh);
    CHECK(!fresh->request.sessionID && !fresh->request.isAttachOnly);
    // Fresh creation is a distinct valid intent, never executed by this fixture.
}

}

int main()
{
    aVanishedSessionIsNotRecreated();
    anExistingSessionKeepsItsIdentity();
    aNonzeroSessionRequiresAttachOnly();
    return UBIQUITTY_TEST_SUMMARY();
}
