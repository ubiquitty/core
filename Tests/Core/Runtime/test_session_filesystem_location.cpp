// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "HostedPaneFixture.h"
#include "SoleModelThread.h"

#include "Core/Runtime/LaunchCommand.h"
#include "Core/Runtime/TerminalSession.h"
#include "Platform/StreamDouble.h"

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace Ubiquitty::PAL {

// The session clock is frontend-owned; these cases need no simulated time.
Ticks monotonicTicks()
{
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    const auto nanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(since).count());
    return Ticks::fromTimespec(nanoseconds / 1'000'000'000, nanoseconds % 1'000'000'000);
}

uint32_t monotonicMicros()
{
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(since).count() & 0xFFFFFFFFull);
}

// Exercise the real launch transition without starting a child process.
std::unique_ptr<Stream> openPTY(std::span<const std::string>, const PTYLaunchOptions&)
{
    return std::make_unique<TestDoubles::StreamDouble>();
}

} // namespace Ubiquitty::PAL

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Runtime;

using SessionOwner = std::unique_ptr<TerminalSession, decltype(&releaseTerminalSession)>;

SessionOwner makeSession()
{
    SessionOwner session(TerminalSession::make(), releaseTerminalSession);
    if (session)
        session->start(24, 80);
    return session;
}

} // namespace

TEST(aDirectOrUnplacedStreamCannotAuthorizeLocalFileOperations)
{
    auto session = makeSession();
    REQUIRE(session != nullptr);
    CHECK(!session->isFilesystemLocal());
    REQUIRE(session->adoptStream(std::make_unique<TestDoubles::StreamDouble>()));
    CHECK(!session->isFilesystemLocal());
    withSessionModel(*session, [](Terminal& terminal) { terminal.currentCapabilityOrigin = CapabilityOrigin::Local; });
    CHECK(!session->isFilesystemLocal());
}

TEST(aLaunchedLocalPTYKeepsItsFilesystemWhenAnAgentControlsIt)
{
    auto session = makeSession();
    REQUIRE(session != nullptr);
    REQUIRE(session->launch(LaunchCommand::fromArguments({ "/bin/sh" })));
    CHECK(session->isFilesystemLocal());
    withSessionModel(*session, [](Terminal& terminal) { terminal.currentCapabilityOrigin = CapabilityOrigin::Agent; });
    CHECK(session->isFilesystemLocal());
    withSessionModel(*session, [](Terminal& terminal) { terminal.currentCapabilityOrigin = CapabilityOrigin::Remote; });
    CHECK(session->isFilesystemLocal());
    session->close();
    CHECK(!session->isFilesystemLocal());
}

TEST(anAdoptedTmuxPaneUsesItsBackendsFilesystemProvenance)
{
    for (const FilesystemLocation location : { FilesystemLocation::Unknown, FilesystemLocation::ThisMachine, FilesystemLocation::AnotherMachine }) {
        UbiquittyTest::HostedPaneFixture hosted(24, 80, {}, location);
        CHECK(hosted.host().paneFacts(UbiquittyTest::HostedPaneFixture::paneID).filesystemLocation == location);
        auto session = makeSession();
        REQUIRE(session != nullptr);
        REQUIRE(session->adoptHostedPane(hosted.sourceAddress(), UbiquittyTest::HostedPaneFixture::paneID));
        CHECK(session->isFilesystemLocal() == (location == FilesystemLocation::ThisMachine));
        withSessionModel(*session, [](Terminal& terminal) { terminal.currentCapabilityOrigin = CapabilityOrigin::Remote; });
        CHECK(session->isFilesystemLocal() == (location == FilesystemLocation::ThisMachine));
        withSessionModel(*session, [](Terminal& terminal) { terminal.currentCapabilityOrigin = CapabilityOrigin::Local; });
        CHECK(session->isFilesystemLocal() == (location == FilesystemLocation::ThisMachine));
        session->endExternalAdoption();
        CHECK(!session->isFilesystemLocal());
    }
}

TEST(aTmuxLauncherCarriesFilesystemProvenanceIntoItsHostedPane)
{
    const PostedWorkLoop loop {
        .runSoon = [](PostedWorkToken token) { discardPostedSessionWork(token); },
        .runAfter = [](double, PostedWorkToken token) { discardPostedSessionWork(token); },
    };
    TerminalSession::enableHostedWindows(reinterpret_cast<std::uintptr_t>(&makeTmuxControlHost), loop);
    for (const std::string& host : { std::string {}, std::string { "remote.example" } }) {
        auto primary = makeSession();
        REQUIRE(primary != nullptr);
        LaunchCommand command = LaunchCommand::fromArguments({ "/bin/sh" });
        command.setTmuxControlHost(host);
        REQUIRE(primary->launch(command));
        REQUIRE(primary->isHostingWindows());
        const std::string reply = "%begin 1 1 0\n%end 1 1 0\n%begin 2 2 1\n@1 %1 80 24 1 ef01,80x24,0,0,1 work editor\n%end 2 2 1\n";
        primary->feedBytes(ByteSpan(reply.data(), reply.size()));
        const HostedPaneFacts facts = primary->hostedPaneFacts(1);
        REQUIRE(facts.terminalAddress != 0);
        CHECK(facts.filesystemLocation == (host.empty() ? FilesystemLocation::ThisMachine : FilesystemLocation::AnotherMachine));
        auto pane = makeSession();
        REQUIRE(pane != nullptr);
        REQUIRE(pane->adoptHostedPane(primary->hostedWindowSourceAddress(), 1));
        pane->routeInputViaTmux(reinterpret_cast<const Protocol::Tmux::TmuxClient*>(facts.inputClientAddress), reinterpret_cast<PAL::Stream*>(facts.inputStreamAddress), 1);
        CHECK(!pane->isRemoteAttached());
        CHECK(pane->isFilesystemLocal() == host.empty());
        pane->endExternalAdoption();
    }
}

UBIQUITTY_TEST_MAIN()
