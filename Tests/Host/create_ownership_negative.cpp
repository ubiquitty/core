// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Creator standing belongs to one authenticated principal and one session.
// A fresh physical connection of that principal can return to its created
// session; neither another principal nor an empty grant key can inherit it.

#include "Assertions.h"
#include "HostConnection.h"
#include "ScreenPlaneAccess.h"

#include <fcntl.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Host {

struct CreateOwnershipNegativeAccess {
    static void setShellPath(Daemon& daemon, const std::string& path) { daemon.m_shellPath = path; }
    static void permitMissingToken(Daemon& daemon) { daemon.m_requireToken = false; }
    static std::uint32_t clientSessionID(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].sessionID; }
    static bool sessionGrant(const Daemon& daemon, std::uint32_t sessionID)
    {
        const auto session = std::ranges::find_if(daemon.m_sessions, [&](const auto& candidate) { return candidate->id == sessionID; });
        return session != daemon.m_sessions.end() && !(*session)->agentSessionGrantIdentities.empty();
    }
    static std::string sessionGrantIdentity(const Daemon& daemon, std::uint32_t sessionID)
    {
        const auto session = std::ranges::find_if(daemon.m_sessions, [&](const auto& candidate) { return candidate->id == sessionID; });
        return session == daemon.m_sessions.end() || (*session)->agentSessionGrantIdentities.empty() ? std::string {} : (*session)->agentSessionGrantIdentities.front();
    }
    static bool hasStandingGrant(Daemon& daemon, std::size_t index, const Daemon::Session& session)
    {
        return daemon.agentHasStandingGrant(daemon.m_clients[index], session);
    }
    static std::string grantKey(Daemon& daemon, std::size_t index) { return daemon.grantKeyFor(daemon.m_clients[index]); }
};

}

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
using Access = Ubiquitty::Host::CreateOwnershipNegativeAccess;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
using Ubiquitty::Host::Daemon;

struct Peer {
    std::size_t index;
    Ubiquitty::PAL::UnixFileDescriptor socket;
    Ubiquitty::HostTest::Connection reader;
};

std::optional<Peer> connectAgent(Daemon& daemon, std::string_view tokenID)
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
    if (!tokenID.empty()) {
        // This is the verified, non-interactive bootstrap premise, not a Greet
        // credential or a human-origin override.
        Screen::grantScope(daemon, index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::NewSession });
        Screen::setTokenID(daemon, index, std::string(tokenID));
        Screen::markBootstrapToken(daemon, index);
        Screen::markQUICAuthenticated(daemon, index);
    }
    Peer peer { .index = index, .socket = Ubiquitty::PAL::UnixFileDescriptor { ends[1] }, .reader = {} };
    peer.reader.fileDescriptor = peer.socket.get();
    return peer;
}

Sync::Greet greetingFor(Daemon& daemon, std::uint32_t sessionID, std::uint64_t instance)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = instance, .high = 0x63726561746f72 };
    greeting.clientKind = Sync::ClientKind::Agent;
    greeting.requestedRole = Sync::Role::Agent;
    greeting.request.target = Sync::AdmissionTarget::Session;
    greeting.request.sessionID = sessionID;
    greeting.request.isAttachOnly = sessionID != 0;
    greeting.request.expectedDaemonInstanceID = Screen::daemonInstanceID(daemon);
    greeting.request.agentLabel = "test-agent";
    greeting.facts.viewportColumns = 80;
    greeting.facts.viewportRows = 24;
    return greeting;
}

std::span<const std::uint8_t> bytes(const std::string& body)
{
    return { reinterpret_cast<const std::uint8_t*>(body.data()), body.size() };
}

void deliver(Daemon& daemon, Peer& peer, const Sync::Greet& greeting)
{
    const auto frame = Ubiquitty::HostTest::greetFrame(greeting);
    REQUIRE(!frame.empty());
    peer.reader.greeting = greeting;
    Screen::receive(daemon, peer.index, bytes(frame));
    Screen::service(daemon);
    Screen::flush(daemon, peer.index);
}

void checkGranted(Peer& peer, std::uint32_t sessionID)
{
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer.reader, Sync::FrameKind::Welcome, answer, 2000));
    const auto welcome = Sync::decodeWelcome(bytes(answer));
    REQUIRE(welcome);
    CHECK(welcome->admission == Sync::AttachmentAdmission::Granted);
    CHECK(welcome->selection.target == Sync::AdmissionTarget::Session);
    CHECK_EQ(welcome->selection.sessionID, sessionID);
    CHECK_EQ(welcome->selection.daemonInstanceID, peer.reader.greeting->request.expectedDaemonInstanceID);
    CHECK(welcome->grantedRole == Sync::Role::Agent);
    CHECK(!peer.reader.hasFailed);
}

void checkRefused(Peer& peer, Sync::ByeReason reason)
{
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer.reader, Sync::FrameKind::Bye, answer, 2000));
    const auto bye = Sync::decodeBye(bytes(answer));
    REQUIRE(bye);
    CHECK(bye->reason == reason);
    CHECK(!peer.reader.welcome);
    CHECK(peer.reader.panes.empty());
    CHECK(peer.reader.frames.empty());
    CHECK(!peer.reader.hasFailed);
}

void run()
{
    Daemon daemon;
    Access::setShellPath(daemon, "/bin/sh");
    Access::permitMissingToken(daemon);
    std::uint32_t createdSessionID = 0;
    {
        auto creator = connectAgent(daemon, "tok-creator");
        REQUIRE(creator);
        deliver(daemon, *creator, greetingFor(daemon, 0, 1));
        createdSessionID = Access::clientSessionID(daemon, creator->index);
        REQUIRE(createdSessionID);
        checkGranted(*creator, createdSessionID);
        CHECK(Access::sessionGrant(daemon, createdSessionID));
        CHECK_STR_EQ(Access::sessionGrantIdentity(daemon, createdSessionID), "token:tok-creator");
        Screen::drop(daemon, creator->index);
    }
    {
        auto returning = connectAgent(daemon, "tok-creator");
        REQUIRE(returning);
        deliver(daemon, *returning, greetingFor(daemon, createdSessionID, 2));
        checkGranted(*returning, createdSessionID);
        CHECK_EQ(Access::clientSessionID(daemon, returning->index), createdSessionID);
        CHECK(Screen::isAgent(daemon, returning->index));
        Screen::drop(daemon, returning->index);
    }
    {
        auto intruder = connectAgent(daemon, "tok-intruder");
        REQUIRE(intruder);
        deliver(daemon, *intruder, greetingFor(daemon, createdSessionID, 3));
        checkRefused(*intruder, Sync::ByeReason::NoConsent);
        CHECK_EQ(Access::clientSessionID(daemon, intruder->index), 0u);
        CHECK(Access::sessionGrant(daemon, createdSessionID));
        CHECK_STR_EQ(Access::sessionGrantIdentity(daemon, createdSessionID), "token:tok-creator");
    }
    {
        auto& foreign = Screen::makeSession(daemon, 24, 80);
        (void)foreign.addSessionGrant("token:tok-other-creator");
        auto samePrincipal = connectAgent(daemon, "tok-creator");
        REQUIRE(samePrincipal);
        deliver(daemon, *samePrincipal, greetingFor(daemon, foreign.id, 4));
        checkRefused(*samePrincipal, Sync::ByeReason::NoConsent);
        CHECK_EQ(Access::clientSessionID(daemon, samePrincipal->index), 0u);
        CHECK(foreign.hasSessionGrantFor("token:tok-other-creator"));
        CHECK_STR_EQ(foreign.agentSessionGrantIdentities.front(), "token:tok-other-creator");
    }
    // A peer with neither a token nor a verified local identity is Remote, and
    // is refused for the missing token however the requirement is set.
    {
        auto anonymous = connectAgent(daemon, {});
        REQUIRE(anonymous);
        const auto before = Screen::sessionCount(daemon);
        deliver(daemon, *anonymous, greetingFor(daemon, 0, 5));
        checkRefused(*anonymous, Sync::ByeReason::TokenMissing);
        CHECK_EQ(Access::clientSessionID(daemon, anonymous->index), 0u);
        CHECK_EQ(Screen::sessionCount(daemon), before);
        CHECK(Access::grantKey(daemon, anonymous->index).empty());
    }
    {
        auto& emptyGrant = Screen::makeSession(daemon, 24, 80);
        // An empty key is never a grant: addSessionGrant refuses it, so the set
        // stays empty and a tokenless agent still re-prompts.
        (void)emptyGrant.addSessionGrant(std::string {});
        auto anonymous = connectAgent(daemon, {});
        REQUIRE(anonymous);
        CHECK(Access::grantKey(daemon, anonymous->index).empty());
        CHECK(!Access::hasStandingGrant(daemon, anonymous->index, emptyGrant));
        const auto before = Screen::sessionCount(daemon);
        deliver(daemon, *anonymous, greetingFor(daemon, emptyGrant.id, 6));
        checkRefused(*anonymous, Sync::ByeReason::TokenMissing);
        CHECK_EQ(Access::clientSessionID(daemon, anonymous->index), 0u);
        CHECK_EQ(Screen::sessionCount(daemon), before);
        CHECK(emptyGrant.agentSessionGrantIdentities.empty());
        CHECK(!Access::hasStandingGrant(daemon, anonymous->index, emptyGrant));
    }
}

}

int main()
{
    run();
    return UBIQUITTY_TEST_SUMMARY();
}
