// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The wire has one version. A peer at any other is refused by the host before
// complete-schema admission, with a refusal naming the version the host
// speaks; a peer at this version is admitted.

#include "Assertions.h"
#include "HostConnection.h"
#include "ScreenPlaneAccess.h"

#include "Core/Protocol/Sync/Wire.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>

#include <array>
#include <csignal>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty::Host {

struct HelloWireVersionSkewAccess {
    static void setWireVersion(Daemon& daemon, std::uint16_t version) { daemon.m_wireVersion = version; }
    static bool helloSeen(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].helloSeen; }
    static bool didOverflow(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].didOverflow; }
    static std::uint32_t clientSessionID(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].sessionID; }
};

}

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
using Access = Ubiquitty::Host::HelloWireVersionSkewAccess;
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
    const auto index = Screen::connect(daemon, Ubiquitty::PAL::UnixFileDescriptor { ends[0] });
    Screen::markTrustedLocal(daemon, index);
    Peer peer { .index = index, .socket = Ubiquitty::PAL::UnixFileDescriptor { ends[1] }, .reader = {} };
    peer.reader.fileDescriptor = peer.socket.get();
    return peer;
}

Sync::Greet greetingFor(Daemon& daemon, std::uint32_t sessionID = 0)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = 1, .high = 0x76657273696f6e };
    greeting.request.target = sessionID ? Sync::AdmissionTarget::Session : Sync::AdmissionTarget::Control;
    greeting.request.sessionID = sessionID;
    greeting.request.isAttachOnly = sessionID != 0;
    greeting.request.expectedDaemonInstanceID = Screen::daemonInstanceID(daemon);
    greeting.facts.viewportRows = 24;
    greeting.facts.viewportColumns = 80;
    return greeting;
}

std::span<const std::uint8_t> bytes(const std::string& body)
{
    return { reinterpret_cast<const std::uint8_t*>(body.data()), body.size() };
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

void checkRefused(std::uint16_t version, std::uint16_t hostVersion = Sync::wireVersion)
{
    Daemon daemon;
    Access::setWireVersion(daemon, hostVersion);
    const auto sessionID = Screen::makeSession(daemon, 24, 80).id;
    auto peer = connectPeer(daemon);
    REQUIRE(peer);
    auto greeting = greetingFor(daemon, sessionID);
    greeting.wireVersion = version;
    std::vector<std::uint8_t> body;
    Sync::encode(body, greeting);
    REQUIRE(Sync::decodeGreet(body));
    deliver(daemon, *peer, greeting, body);

    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer->reader, Sync::FrameKind::Bye, answer, 2000));
    const auto bye = Sync::decodeBye(bytes(answer));
    REQUIRE(bye && bye->peer);
    CHECK(bye->reason == Sync::ByeReason::VersionMismatch);
    CHECK(bye->peer->build.isKnown);
    CHECK_EQ(bye->peer->build.wireVersion, hostVersion);
    CHECK_STR_EQ(bye->peer->build.appVersion, Sync::appVersion());
    CHECK(!bye->peer->platform.empty());
    CHECK(!peer->reader.welcome);
    CHECK(!Access::helloSeen(daemon, peer->index));
    CHECK_EQ(Access::clientSessionID(daemon, peer->index), 0u);
    CHECK(Access::didOverflow(daemon, peer->index));
}

void aPeerAtThisVersionIsAdmitted()
{
    Daemon daemon;
    const auto sessionID = Screen::makeSession(daemon, 24, 80).id;
    auto peer = connectPeer(daemon);
    REQUIRE(peer);
    auto greeting = greetingFor(daemon, sessionID);
    std::vector<std::uint8_t> body;
    Sync::encode(body, greeting);
    REQUIRE(Sync::decodeGreet(body));
    deliver(daemon, *peer, greeting, body);
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer->reader, Sync::FrameKind::Welcome, answer, 2000));
    const auto welcome = Sync::decodeWelcome(bytes(answer));
    REQUIRE(welcome);
    CHECK_EQ(welcome->wireVersion, Sync::wireVersion);
    CHECK(welcome->admission == Sync::AttachmentAdmission::Granted);
    CHECK_EQ(welcome->selection.sessionID, sessionID);
    CHECK_EQ(welcome->selection.daemonInstanceID, greeting.request.expectedDaemonInstanceID);
    CHECK_EQ(Access::clientSessionID(daemon, peer->index), sessionID);
    CHECK(!peer->reader.hasFailed);
}

void theVersionPrefixDoesNotBypassTheCompleteSchema()
{
    for (const auto version : { static_cast<std::uint16_t>(Sync::wireVersion + 1), Sync::wireVersion }) {
        Daemon daemon;
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        const auto greeting = greetingFor(daemon);
        std::vector<std::uint8_t> body;
        Sync::WireWriter writer(body);
        writer.u16(version);
        std::vector<std::uint8_t> frame;
        REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Greet, 0, body));
        peer->reader.greeting = greeting;
        Screen::receive(daemon, peer->index, std::span(frame).first(frame.size() - 1));
        Screen::service(daemon);
        Screen::flush(daemon, peer->index);
        pollfd waiting { .fd = peer->socket.get(), .events = POLLIN, .revents = 0 };
        CHECK_EQ(::poll(&waiting, 1, 0), 0);
        CHECK(!Access::helloSeen(daemon, peer->index));
        Screen::receive(daemon, peer->index, std::span(frame).last(1));
        Screen::service(daemon);
        Screen::flush(daemon, peer->index);
        std::string answer;
        REQUIRE(Ubiquitty::HostTest::waitFor(peer->reader, Sync::FrameKind::Bye, answer, 2000));
        const auto bye = Sync::decodeBye(bytes(answer));
        REQUIRE(bye);
        CHECK(bye->reason == (version != Sync::wireVersion ? Sync::ByeReason::VersionMismatch : Sync::ByeReason::Malformed));
        CHECK(!Access::helloSeen(daemon, peer->index));
        CHECK(!peer->reader.welcome);
        CHECK_EQ(Access::clientSessionID(daemon, peer->index), 0u);
    }
}

}

int main()
{
    // A refused peer's socket is closed under it; a write that races the close
    // is the refusal, not a crash.
    ::signal(SIGPIPE, SIG_IGN);
    // Clients one version behind, one version ahead and far ahead meet this
    // host.
    checkRefused(static_cast<std::uint16_t>(Sync::wireVersion - 1));
    checkRefused(static_cast<std::uint16_t>(Sync::wireVersion + 1));
    checkRefused(28);
    // A host one version ahead meets this build's client and names its own.
    checkRefused(Sync::wireVersion, static_cast<std::uint16_t>(Sync::wireVersion + 1));
    aPeerAtThisVersionIsAdmitted();
    theVersionPrefixDoesNotBypassTheCompleteSchema();
    return UBIQUITTY_TEST_SUMMARY();
}
