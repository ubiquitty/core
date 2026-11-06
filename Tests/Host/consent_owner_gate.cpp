// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Detached sessions retain their owner's authority. An ungranted Agent can
// neither attach nor terminate one. Exact prior owner consent and creation
// remain independent positive paths, with one greeting per physical stream.

#include "Assertions.h"
#include "Host/HostConnection.h"
#include "Host/ScreenPlaneAccess.h"
#include "Host/TerminationClient.h"

#include <fcntl.h>
#include <sys/socket.h>

#include <array>
#include <optional>
#include <span>
#include <string>

namespace Ubiquitty::Host {

struct ConsentOwnerGateAccess {
    static void setShellPath(Daemon& daemon, const std::string& path) { daemon.m_shellPath = path; }
    static void grantAlwaysAllow(Daemon& daemon, const std::string& key) { daemon.m_alwaysAllowedAgents.push_back(key); }
    static std::uint32_t clientSessionID(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].sessionID; }
    static void receiveNativeInput(Daemon& daemon, std::size_t index)
    {
        std::array<std::uint8_t, 8192> buffer {};
        for (;;) {
            const auto count = ::recv(daemon.m_clients[index].fileDescriptor.get(), buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (count <= 0)
                return;
            ScreenPlaneAccess::receive(daemon, index, std::span(buffer).first(static_cast<std::size_t>(count)));
        }
    }
};

}

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
using Access = Ubiquitty::Host::ConsentOwnerGateAccess;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
using Ubiquitty::Host::Daemon;

struct Peer {
    std::size_t index;
    Ubiquitty::PAL::UnixFileDescriptor socket;
    Ubiquitty::HostTest::Connection reader;
};

std::optional<Peer> connectAgent(Daemon& daemon, std::string tokenID)
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
    Screen::grantScope(daemon, index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::NewSession });
    Screen::setTokenID(daemon, index, std::move(tokenID));
    Screen::markBootstrapToken(daemon, index);
    Screen::markQUICAuthenticated(daemon, index);
    Peer peer { .index = index, .socket = Ubiquitty::PAL::UnixFileDescriptor { ends[1] }, .reader = {} };
    peer.reader.fileDescriptor = peer.socket.get();
    return peer;
}

enum class Target : std::uint8_t {
    Session,
    Control
};

Sync::Greet greetingFor(Daemon& daemon, Target target, std::uint32_t sessionID, std::uint64_t instance)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = instance, .high = 0x6f776e6572 };
    greeting.clientKind = Sync::ClientKind::Agent;
    greeting.requestedRole = Sync::Role::Agent;
    greeting.request.target = target == Target::Control ? Sync::AdmissionTarget::Control : Sync::AdmissionTarget::Session;
    greeting.request.sessionID = sessionID;
    greeting.request.isAttachOnly = sessionID != 0;
    greeting.request.agentLabel = "test-agent";
    greeting.request.expectedDaemonInstanceID = Screen::daemonInstanceID(daemon);
    greeting.facts.viewportRows = 24;
    greeting.facts.viewportColumns = 80;
    return greeting;
}

std::span<const std::uint8_t> bytes(const std::string& body)
{
    return { reinterpret_cast<const std::uint8_t*>(body.data()), body.size() };
}

void admit(Daemon& daemon, Peer& peer, Sync::Greet greeting)
{
    const auto frame = Ubiquitty::HostTest::greetFrame(greeting);
    REQUIRE(!frame.empty());
    peer.reader.greeting = std::move(greeting);
    Screen::receive(daemon, peer.index, bytes(frame));
    Screen::service(daemon);
    Screen::flush(daemon, peer.index);
}

void checkGranted(Peer& peer, Sync::AdmissionTarget target, std::uint32_t sessionID)
{
    std::string body;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer.reader, Sync::FrameKind::Welcome, body, 2000));
    const auto welcome = Sync::decodeWelcome(bytes(body));
    REQUIRE(welcome);
    CHECK(welcome->admission == Sync::AttachmentAdmission::Granted);
    CHECK(welcome->selection.target == target);
    CHECK_EQ(welcome->selection.sessionID, sessionID);
    CHECK_EQ(welcome->selection.daemonInstanceID, peer.reader.greeting->request.expectedDaemonInstanceID);
    CHECK(welcome->grantedRole == Sync::Role::Agent);
    CHECK(!peer.reader.hasFailed);
}

Ubiquitty::Protocol::Control::TerminationOutcome terminate(Daemon& daemon, Peer& peer, std::uint32_t sessionID)
{
    Ubiquitty::HostTest::TerminationClient termination(peer.reader);
    if (!termination.send(sessionID, "owner.termination"))
        return Ubiquitty::Protocol::Control::TerminationOutcome::Failed;
    Access::receiveNativeInput(daemon, peer.index);
    Screen::service(daemon);
    Screen::flush(daemon, peer.index);
    const auto outcome = termination.wait();
    // The answer leaves before the session ends: the daemon ends an owed
    // termination at the start of its next pass, which this runs.
    Screen::settleClosures(daemon);
    return outcome;
}

void run()
{
    Daemon daemon;
    const auto sessionID = Screen::makeSession(daemon, 24, 80).id;
    {
        auto stranger = connectAgent(daemon, "tok-thirdparty");
        REQUIRE(stranger);
        admit(daemon, *stranger, greetingFor(daemon, Target::Session, sessionID, 1));
        std::string answer;
        REQUIRE(Ubiquitty::HostTest::waitFor(stranger->reader, Sync::FrameKind::Bye, answer, 2000));
        const auto bye = Sync::decodeBye(bytes(answer));
        REQUIRE(bye);
        CHECK(bye->reason == Sync::ByeReason::NoConsent);
        CHECK(!stranger->reader.welcome);
        CHECK(stranger->reader.panes.empty());
        CHECK(stranger->reader.frames.empty());
        CHECK_EQ(Access::clientSessionID(daemon, stranger->index), 0u);
        CHECK(Screen::hasSession(daemon, sessionID));
    }
    {
        auto stranger = connectAgent(daemon, "tok-terminator");
        REQUIRE(stranger);
        admit(daemon, *stranger, greetingFor(daemon, Target::Control, 0, 2));
        checkGranted(*stranger, Sync::AdmissionTarget::Control, 0);
        CHECK(terminate(daemon, *stranger, sessionID) == Ubiquitty::Protocol::Control::TerminationOutcome::Refused);
        CHECK(Screen::hasSession(daemon, sessionID));
        Screen::drop(daemon, stranger->index);
    }
    {
        auto granted = connectAgent(daemon, "tok-granted");
        REQUIRE(granted);
        Access::grantAlwaysAllow(daemon, "token:tok-granted");
        admit(daemon, *granted, greetingFor(daemon, Target::Session, sessionID, 3));
        checkGranted(*granted, Sync::AdmissionTarget::Session, sessionID);
        CHECK_EQ(Access::clientSessionID(daemon, granted->index), sessionID);
        CHECK(terminate(daemon, *granted, sessionID) == Ubiquitty::Protocol::Control::TerminationOutcome::Terminated);
        CHECK(!Screen::hasSession(daemon, sessionID));
        Screen::drop(daemon, granted->index);
    }
    {
        Access::setShellPath(daemon, "/bin/sh");
        auto creator = connectAgent(daemon, "tok-creator");
        REQUIRE(creator);
        admit(daemon, *creator, greetingFor(daemon, Target::Session, 0, 4));
        const auto createdID = Access::clientSessionID(daemon, creator->index);
        REQUIRE(createdID);
        checkGranted(*creator, Sync::AdmissionTarget::Session, createdID);
        Screen::drop(daemon, creator->index);
    }
}

}

int main()
{
    run();
    return UBIQUITTY_TEST_SUMMARY();
}
